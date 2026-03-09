#include "zarr_loader_internal.h"
#include <iostream>

bool ZarrDetectionLoader::loadKeypointHeadingData(const ts::kvstore::KvStore& store) {
    data_.flat_headings_deg.clear();
    data_.flat_swim_bladder_px.clear();
    data_.flat_heading_valid.clear();
    data_.has_heading_data = false;
    data_.keypoints_run_name.clear();
    data_.keypoints_source_crop_run.clear();
    data_.flat_keypoints_px.clear();
    data_.keypoint_labels.clear();
    data_.keypoints_per_detection = 0;
    data_.has_keypoints = false;
    data_.keypoint_roi_frame_indices.clear();
    data_.keypoint_detection_index_by_roi.clear();
    data_.keypoint_roi_index_by_detection.clear();
    data_.mask_roi_indices.clear();
    data_.roi_offset_x.clear();
    data_.roi_offset_y.clear();
    data_.roi_width_px.clear();
    data_.roi_height_px.clear();
    data_.eye_angle_run_name.clear();
    data_.eye_angle_frame_indices.clear();
    data_.eye_angle_valid_mask.clear();
    data_.eye_angle_left_deg.clear();
    data_.eye_angle_right_deg.clear();
    data_.eye_angle_indices_by_frame.clear();
    data_.has_eye_angles = false;
    data_.eye_vergence_signed_frame_deg.clear();
    data_.eye_vergence_frame_time_seconds.clear();
    data_.eye_vergence_frame_valid.clear();
    data_.has_eye_vergence_frame = false;

    // Clear refined keypoint quality fields
    data_.is_refined_keypoints = false;
    data_.refined_keypoints_run_name.clear();
    data_.flat_keypoint_quality_labels.clear();
    data_.flat_keypoint_reason.clear();
    data_.flat_keypoint_flip_corrected.clear();
    data_.flat_keypoint_usable.clear();
    data_.flat_keypoint_confidence_valid.clear();
    data_.flat_keypoint_geometry_valid.clear();
    data_.flat_keypoint_refined_success.clear();
    data_.flat_keypoint_detection_source.clear();
    data_.kp_review_state.clear();
    data_.kp_review_method.clear();
    data_.kp_review_intended_use.clear();
    data_.kp_review_timestamp.clear();
    data_.kp_review_reviewer.clear();
    data_.kp_review_notes.clear();
    data_.has_kp_review_status = false;

    if (data_.layout != ZarrLayoutType::kPaletteRuns) {
        return false;
    }

    if (data_.frame_offsets.size() < 2) {
        return false;
    }

    const size_t total_detections = data_.bbox_norm_coords.size();
    if (total_detections == 0) {
        return false;
    }

    // Phase 1: Try refined_keypoints_runs (preferred source)
    std::string latest_run;
    bool is_refined_source = false;
    std::string run_base;

    if (auto group_attrs = readAttrsAny(store, "refined_keypoints_runs")) {
        latest_run = extractLatestRunName(*group_attrs);
        if (!latest_run.empty()) {
            is_refined_source = true;
        }
    }
    if (latest_run.empty() && !root_path_.empty()) {
        auto refined_candidates = collect_runs_fs(
            root_path_,
            "refined_keypoints_runs",
            {"frame_indices", "keypoints_roi", "heading"});
        if (!refined_candidates.empty()) {
            latest_run = refined_candidates.back();
            is_refined_source = true;
        }
    }

    // Phase 2: Fall back to raw keypoints_runs
    if (latest_run.empty()) {
        if (auto group_attrs = readAttrsAny(store, "keypoints_runs")) {
            latest_run = extractLatestRunName(*group_attrs);
        }
    }
    if (latest_run.empty()) {
        if (root_path_.empty()) {
            return false;
        }
        auto candidates = collect_runs_fs(
            root_path_,
            "keypoints_runs",
            {"frame_indices", "keypoints_roi", "heading"});
        if (candidates.empty()) {
            return false;
        }
        latest_run = candidates.back();
    }

    if (is_refined_source) {
        run_base = "refined_keypoints_runs/" + latest_run + "/";
    } else {
        run_base = "keypoints_runs/" + latest_run + "/";
    }

    std::vector<int32_t> kp_frame_indices;
    if (!readInt32Array(store, run_base + "frame_indices", kp_frame_indices)) {
        return false;
    }
    if (kp_frame_indices.empty()) {
        return false;
    }
    const size_t roi_count = kp_frame_indices.size();
    data_.keypoint_roi_frame_indices = kp_frame_indices;
    data_.keypoint_detection_index_by_roi.assign(roi_count, -1);
    data_.keypoint_roi_index_by_detection.assign(total_detections, -1);

    std::vector<float> heading_values;
    if (!readFloatArray(store, run_base + "heading", heading_values)) {
        heading_values.assign(roi_count, 0.0f);
    } else if (heading_values.size() != roi_count) {
        heading_values.resize(roi_count, 0.0f);
    }

    std::vector<uint8_t> detection_success;
    if (!readBoolArray(store, run_base + "detection_success", detection_success) ||
        detection_success.size() != roi_count) {
        detection_success.assign(roi_count, 1);
    }

    size_t num_keypoints = 0;
    size_t coord_dim = 0;
    std::vector<float> kp_values;
    enum class KeypointSpace {
        kImage,
        kRoi,
        kNormalized
    };
    KeypointSpace keypoint_space = KeypointSpace::kImage;

    auto load_keypoints_dataset = [&](const std::string& dataset_path,
                                      KeypointSpace space) -> bool {
        auto store_float = openArrayAny<float, 3>(store, dataset_path, context_);
        if (store_float.ok()) {
            auto array_result = ts::Read(store_float.value()).result();
            if (!array_result.ok()) {
                return false;
            }
            auto array = array_result.value();
            auto shape = array.shape();
            if (shape.size() != 3 ||
                shape[0] != static_cast<tensorstore::Index>(roi_count)) {
                return false;
            }
            num_keypoints = static_cast<size_t>(shape[1]);
            coord_dim = static_cast<size_t>(shape[2]);
            if (num_keypoints == 0 || coord_dim < 2) {
                return false;
            }
            size_t total_values = roi_count * num_keypoints * coord_dim;
            kp_values.resize(total_values);
            const float* data_ptr = static_cast<const float*>(array.data());
            std::copy(data_ptr, data_ptr + total_values, kp_values.begin());
            keypoint_space = space;
            return true;
        }

        auto store_double = openArrayAny<double, 3>(store, dataset_path, context_);
        if (!store_double.ok()) {
            return false;
        }
        auto array_result = ts::Read(store_double.value()).result();
        if (!array_result.ok()) {
            return false;
        }
        auto array = array_result.value();
        auto shape = array.shape();
        if (shape.size() != 3 ||
            shape[0] != static_cast<tensorstore::Index>(roi_count)) {
            return false;
        }
        num_keypoints = static_cast<size_t>(shape[1]);
        coord_dim = static_cast<size_t>(shape[2]);
        if (num_keypoints == 0 || coord_dim < 2) {
            return false;
        }
        size_t total_values = roi_count * num_keypoints * coord_dim;
        kp_values.resize(total_values);
        const double* data_ptr = static_cast<const double*>(array.data());
        for (size_t i = 0; i < total_values; ++i) {
            kp_values[i] = static_cast<float>(data_ptr[i]);
        }
        keypoint_space = space;
        return true;
    };

    if (!load_keypoints_dataset(run_base + "keypoints_img", KeypointSpace::kImage) &&
        !load_keypoints_dataset(run_base + "keypoints_roi", KeypointSpace::kRoi) &&
        !load_keypoints_dataset(run_base + "keypoints_norm", KeypointSpace::kNormalized)) {
        return false;
    }

    auto run_attrs = readAttrsAny(store, run_base);
    size_t swim_index = 0;
    bool swim_label_found = false;
    std::vector<std::string> keypoint_labels_loaded;
    if (run_attrs.has_value() &&
        run_attrs->contains("keypoint_labels") &&
        (*run_attrs)["keypoint_labels"].is_array()) {
        const auto& labels = (*run_attrs)["keypoint_labels"];
        keypoint_labels_loaded.reserve(labels.size());
        for (size_t i = 0; i < labels.size(); ++i) {
            if (!labels[i].is_string()) {
                continue;
            }
            std::string label = labels[i].get<std::string>();
            keypoint_labels_loaded.push_back(label);
            if (!swim_label_found) {
                std::string lowered = label;
                std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                               [](unsigned char c) {
                                   return static_cast<char>(std::tolower(c));
                               });
                if (lowered.find("bladder") != std::string::npos ||
                    lowered.find("swim") != std::string::npos) {
                    swim_index = keypoint_labels_loaded.size() - 1;
                    swim_label_found = true;
                }
            }
        }
    }

    // Load skeleton edges from pose_schema
    data_.skeleton_edges.clear();
    if (run_attrs.has_value() &&
        run_attrs->contains("pose_schema") &&
        (*run_attrs)["pose_schema"].is_object()) {
        const auto& schema = (*run_attrs)["pose_schema"];
        if (schema.contains("edges") && schema["edges"].is_array()) {
            for (const auto& edge : schema["edges"]) {
                if (edge.is_array() && edge.size() == 2 &&
                    edge[0].is_number_unsigned() && edge[1].is_number_unsigned()) {
                    size_t a = edge[0].get<size_t>();
                    size_t b = edge[1].get<size_t>();
                    data_.skeleton_edges.push_back({a, b});
                }
            }
        }
    }

    if (num_keypoints > 0) {
        data_.keypoint_labels.clear();
        if (!keypoint_labels_loaded.empty()) {
            if (keypoint_labels_loaded.size() == num_keypoints) {
                data_.keypoint_labels = keypoint_labels_loaded;
            } else {
                data_.keypoint_labels.reserve(num_keypoints);
                for (size_t i = 0; i < num_keypoints; ++i) {
                    if (i < keypoint_labels_loaded.size()) {
                        data_.keypoint_labels.push_back(keypoint_labels_loaded[i]);
                    } else {
                        data_.keypoint_labels.push_back("kp" + std::to_string(i));
                    }
                }
            }
        }
        if (data_.keypoint_labels.empty()) {
            data_.keypoint_labels.reserve(num_keypoints);
            static const std::array<std::string, 3> kDefaultLabels = {
                "swim_bladder", "left_eye", "right_eye"};
            for (size_t i = 0; i < num_keypoints; ++i) {
                if (i < kDefaultLabels.size()) {
                    data_.keypoint_labels.push_back(kDefaultLabels[i]);
                } else {
                    data_.keypoint_labels.push_back("kp" + std::to_string(i));
                }
            }
        }
        if (swim_index >= num_keypoints) {
            swim_index = 0;
        }
    } else {
        data_.keypoint_labels.clear();
        swim_index = 0;
    }

    std::vector<std::string> crop_candidates;
    auto addCandidate = [&](const std::string& candidate) {
        if (candidate.empty()) {
            return;
        }
        std::string normalized = NormalizeCropRunName(candidate);
        if (std::find(crop_candidates.begin(), crop_candidates.end(), normalized) == crop_candidates.end()) {
            crop_candidates.push_back(normalized);
        }
    };

    if (run_attrs.has_value() && run_attrs->contains("source_crop_run") &&
        (*run_attrs)["source_crop_run"].is_string()) {
        addCandidate((*run_attrs)["source_crop_run"].get<std::string>());
    }

    if (auto crop_group_attrs = readAttrsAny(store, "crop_runs")) {
        std::string latest = extractLatestRunName(*crop_group_attrs);
        if (!latest.empty()) {
            addCandidate(latest);
        }
    }

    if (!root_path_.empty()) {
        auto fs_candidates = collect_runs_fs(
            root_path_,
            "crop_runs",
            {"roi_coordinates_full"});
        for (const auto& name : fs_candidates) {
            addCandidate(name);
        }
    }

    std::vector<float> roi_offsets;
    bool roi_ok = false;
    std::string crop_run;
    float roi_height_px = 0.0f;
    float roi_width_px = 0.0f;
    bool roi_size_available = false;

    auto updateRoiSizeFromAttrs = [&](const std::string& crop_base) {
        if (roi_size_available) {
            return;
        }
        if (auto crop_attrs = readAttrsAny(store, crop_base)) {
            const auto& attrs = *crop_attrs;
            if (attrs.contains("roi_size") && attrs["roi_size"].is_array()) {
                const auto& roi_size_attr = attrs["roi_size"];
                if (roi_size_attr.size() >= 2) {
                    if (roi_size_attr[0].is_number() && roi_size_attr[1].is_number()) {
                        roi_height_px = static_cast<float>(roi_size_attr[0].get<double>());
                        roi_width_px = static_cast<float>(roi_size_attr[1].get<double>());
                        if (roi_height_px > 0.0f && roi_width_px > 0.0f) {
                            roi_size_available = true;
                        }
                    }
                }
            }
        }
        if (!roi_size_available) {
            auto roi_image_store =
                openArrayAny<uint8_t, 3>(store, crop_base + "roi_images", context_);
            if (roi_image_store.ok()) {
                auto domain = roi_image_store.value().domain();
                auto shape = domain.shape();
                if (shape.size() == 3) {
                    float inferred_height = static_cast<float>(shape[1]);
                    float inferred_width = static_cast<float>(shape[2]);
                    if (inferred_height > 0.0f && inferred_width > 0.0f) {
                        roi_height_px = inferred_height;
                        roi_width_px = inferred_width;
                        roi_size_available = true;
                    }
                }
            }
        }
    };

    auto tryLoadCropRun = [&](const std::string& candidate) -> bool {
        if (candidate.empty()) {
            return false;
        }
        std::string crop_base = "crop_runs/" + candidate + "/";
        auto roi_float_store =
            openArrayAny<float, 2>(store, crop_base + "roi_coordinates_full", context_);
        if (roi_float_store.ok()) {
            auto roi_array_result = ts::Read(roi_float_store.value()).result();
            if (roi_array_result.ok()) {
                auto roi_array = roi_array_result.value();
                auto roi_shape = roi_array.shape();
                if (roi_shape.size() == 2 &&
                    roi_shape[0] == static_cast<tensorstore::Index>(roi_count)) {
                    size_t cols = static_cast<size_t>(roi_shape[1]);
                    if (cols >= 2) {
                        roi_offsets.assign(roi_count * 2, 0.0f);
                        const float* roi_ptr = static_cast<const float*>(roi_array.data());
                        for (size_t i = 0; i < roi_count; ++i) {
                            roi_offsets[i * 2 + 0] = roi_ptr[i * cols + 0]; // x / column
                            roi_offsets[i * 2 + 1] = roi_ptr[i * cols + 1]; // y / row
                        }
                        updateRoiSizeFromAttrs(crop_base);
                        return true;
                    }
                }
            }
        }

        auto roi_int_store =
            openArrayAny<int32_t, 2>(store, crop_base + "roi_coordinates_full", context_);
        if (roi_int_store.ok()) {
            auto roi_array_result = ts::Read(roi_int_store.value()).result();
            if (roi_array_result.ok()) {
                auto roi_array = roi_array_result.value();
                auto roi_shape = roi_array.shape();
                if (roi_shape.size() == 2 &&
                    roi_shape[0] == static_cast<tensorstore::Index>(roi_count)) {
                    size_t cols = static_cast<size_t>(roi_shape[1]);
                    if (cols >= 2) {
                        roi_offsets.assign(roi_count * 2, 0.0f);
                        const int32_t* roi_ptr = static_cast<const int32_t*>(roi_array.data());
                        for (size_t i = 0; i < roi_count; ++i) {
                            roi_offsets[i * 2 + 0] = static_cast<float>(roi_ptr[i * cols + 0]);
                            roi_offsets[i * 2 + 1] = static_cast<float>(roi_ptr[i * cols + 1]);
                        }
                        updateRoiSizeFromAttrs(crop_base);
                        return true;
                    }
                }
            }
        }
        return false;
    };

    for (const auto& candidate : crop_candidates) {
        if (tryLoadCropRun(candidate)) {
            crop_run = candidate;
            roi_ok = true;
            break;
        }
    }

    if (!roi_ok && !crop_candidates.empty()) {
        std::cerr << "[HEADING_WARNING] Failed to load ROI offsets from crop_runs for any candidate ("
                  << crop_candidates.size() << " tried)." << std::endl;
    }

    if (!roi_ok) {
        roi_offsets.clear();
    }

    const float nan_value = std::numeric_limits<float>::quiet_NaN();
    data_.flat_headings_deg.assign(total_detections, 0.0f);
    data_.flat_swim_bladder_px.assign(
        total_detections, std::array<float, 2>{nan_value, nan_value});
    data_.flat_heading_valid.assign(total_detections, 0);
    data_.mask_roi_indices.assign(total_detections, -1);
    data_.roi_offset_x.assign(total_detections, nan_value);
    data_.roi_offset_y.assign(total_detections, nan_value);
    data_.roi_width_px.assign(total_detections, 0.0f);
    data_.roi_height_px.assign(total_detections, 0.0f);
    if (num_keypoints > 0) {
        data_.keypoints_per_detection = num_keypoints;
        data_.flat_keypoints_px.assign(total_detections * num_keypoints * 2, nan_value);
        data_.has_keypoints = true;
    } else {
        data_.keypoints_per_detection = 0;
        data_.flat_keypoints_px.clear();
        data_.has_keypoints = false;
    }

    std::vector<size_t> frame_cursor(
        data_.frame_offsets.size() > 0 ? data_.frame_offsets.size() - 1 : 0, 0);
    std::vector<size_t> roi_to_det(roi_count, SIZE_MAX);
    size_t filled = 0;
    size_t finite_keypoint_count = 0;

    for (size_t roi_index = 0; roi_index < roi_count; ++roi_index) {
        int32_t frame = kp_frame_indices[roi_index];
        if (frame < 0) {
            continue;
        }
        if (static_cast<size_t>(frame) >= frame_cursor.size()) {
            continue;
        }

        size_t start = data_.frame_offsets[frame];
        size_t end = data_.frame_offsets[frame + 1];
        if (start >= end) {
            frame_cursor[frame]++;
            continue;
        }

        size_t offset = frame_cursor[frame];
        if (start + offset >= end) {
            frame_cursor[frame]++;
            continue;
        }

        size_t det_index = start + offset;
        frame_cursor[frame]++;
        roi_to_det[roi_index] = det_index;
        if (roi_index < data_.keypoint_detection_index_by_roi.size()) {
            data_.keypoint_detection_index_by_roi[roi_index] =
                static_cast<int32_t>(det_index);
        }
        if (det_index < data_.keypoint_roi_index_by_detection.size()) {
            data_.keypoint_roi_index_by_detection[det_index] =
                static_cast<int32_t>(roi_index);
        }

        data_.mask_roi_indices[det_index] = (roi_ok && roi_offsets.size() >= (roi_index * 2 + 2))
                                                ? static_cast<int32_t>(roi_index)
                                                : -1;

        std::array<float, 4> pixel_box = {nan_value, nan_value, nan_value, nan_value};
        bool have_pixel_box = false;
        if (det_index < data_.bbox_norm_coords.size() &&
            data_.image_width > 0 && data_.image_height > 0) {
            pixel_box = normalizedBoxToPixels(
                data_.bbox_norm_coords[det_index],
                data_.image_width,
                data_.image_height);
            have_pixel_box = true;
        }

        float pixel_box_width = have_pixel_box ? std::max(0.0f, pixel_box[2] - pixel_box[0]) : 0.0f;
        float pixel_box_height = have_pixel_box ? std::max(0.0f, pixel_box[3] - pixel_box[1]) : 0.0f;
        float roi_width_for_det = roi_size_available ? roi_width_px : pixel_box_width;
        float roi_height_for_det = roi_size_available ? roi_height_px : pixel_box_height;

        bool success = detection_success[roi_index] != 0;
        data_.flat_heading_valid[det_index] = success ? 1 : 0;
        data_.flat_headings_deg[det_index] =
            roi_index < heading_values.size() ? heading_values[roi_index] : 0.0f;

        float offset_x_det = nan_value;
        float offset_y_det = nan_value;
        if (roi_ok && roi_offsets.size() >= (roi_index * 2 + 2)) {
            float raw_offset_x = roi_offsets[roi_index * 2 + 0];
            float raw_offset_y = roi_offsets[roi_index * 2 + 1];
            if (std::isfinite(raw_offset_x) && std::isfinite(raw_offset_y)) {
                offset_x_det = raw_offset_x;
                offset_y_det = raw_offset_y;
            }
        }

        bool assigned_anchor = false;
        float assigned_x = nan_value;
        float assigned_y = nan_value;
        float raw_anchor_x = nan_value;
        float raw_anchor_y = nan_value;

        if (!kp_values.empty() && num_keypoints > 0 && coord_dim >= 2) {
            size_t stride = num_keypoints * coord_dim;
            size_t flat_stride = num_keypoints * 2;
            size_t flat_base = det_index * flat_stride;
            bool can_store_flat =
                data_.has_keypoints &&
                (flat_base + flat_stride) <= data_.flat_keypoints_px.size();

            auto convertRawToImage = [&](float raw_x, float raw_y) -> std::array<float, 2> {
                std::array<float, 2> result = {nan_value, nan_value};
                if (!std::isfinite(raw_x) || !std::isfinite(raw_y)) {
                    return result;
                }

                if (keypoint_space == KeypointSpace::kImage) {
                    result[0] = raw_x;
                    result[1] = raw_y;
                    return result;
                }

                float local_x = raw_x;
                float local_y = raw_y;

                if (keypoint_space == KeypointSpace::kNormalized) {
                    float scale_x = roi_width_for_det;
                    float scale_y = roi_height_for_det;
                    bool used_fallback_scale = false;
                    if (scale_x <= 0.0f || scale_y <= 0.0f) {
                        scale_x = pixel_box_width;
                        scale_y = pixel_box_height;
                        used_fallback_scale = true;
                    }
                    if (scale_x <= 0.0f || scale_y <= 0.0f) {
                        return result;
                    }
                    local_x = raw_x * scale_x;
                    local_y = raw_y * scale_y;
                    if (used_fallback_scale) {
                        roi_width_for_det = scale_x;
                        roi_height_for_det = scale_y;
                    }
                }

                float offset_x = offset_x_det;
                float offset_y = offset_y_det;
                bool has_offset = std::isfinite(offset_x) && std::isfinite(offset_y);
                if (!has_offset) {
                    if (std::isfinite(pixel_box[0]) && std::isfinite(pixel_box[1])) {
                        offset_x = pixel_box[0];
                        offset_y = pixel_box[1];
                        has_offset = true;
                    }
                }

                if (keypoint_space == KeypointSpace::kRoi ||
                    keypoint_space == KeypointSpace::kNormalized) {
                    if (!has_offset) {
                        return result;
                    }
                    result[0] = offset_x + local_x;
                    result[1] = offset_y + local_y;
                    return result;
                }

                result[0] = local_x;
                result[1] = local_y;
                return result;
            };

            for (size_t kp_idx = 0; kp_idx < num_keypoints; ++kp_idx) {
                size_t kp_base = roi_index * stride + kp_idx * coord_dim;
                float raw_x = nan_value;
                float raw_y = nan_value;
                if (kp_base + 1 < kp_values.size()) {
                    raw_x = kp_values[kp_base + 0];
                    raw_y = kp_values[kp_base + 1];
                }

                std::array<float, 2> converted = convertRawToImage(raw_x, raw_y);
                if (can_store_flat) {
                    data_.flat_keypoints_px[flat_base + kp_idx * 2 + 0] = converted[0];
                    data_.flat_keypoints_px[flat_base + kp_idx * 2 + 1] = converted[1];
                }
                if (kp_idx == swim_index) {
                    raw_anchor_x = raw_x;
                    raw_anchor_y = raw_y;
                    if (std::isfinite(converted[0]) && std::isfinite(converted[1])) {
                        assigned_x = converted[0];
                        assigned_y = converted[1];
                        assigned_anchor = true;
                    }
                }
                if (can_store_flat && std::isfinite(converted[0]) && std::isfinite(converted[1])) {
                    finite_keypoint_count++;
                }
            }
        }

        if (!assigned_anchor && have_pixel_box) {
            float fallback_x = 0.5f * (pixel_box[0] + pixel_box[2]);
            float fallback_y = 0.5f * (pixel_box[1] + pixel_box[3]);
            if (std::isfinite(fallback_x) && std::isfinite(fallback_y)) {
                assigned_x = fallback_x;
                assigned_y = fallback_y;
                assigned_anchor = true;
            }
        }

        if (assigned_anchor) {
            data_.flat_swim_bladder_px[det_index] = {assigned_x, assigned_y};
        }

        if (std::isfinite(offset_x_det) && std::isfinite(offset_y_det)) {
            data_.roi_offset_x[det_index] = offset_x_det;
            data_.roi_offset_y[det_index] = offset_y_det;
        }

        if (roi_width_for_det > 0.0f && roi_height_for_det > 0.0f) {
            data_.roi_width_px[det_index] = roi_width_for_det;
            data_.roi_height_px[det_index] = roi_height_for_det;
        } else if (have_pixel_box) {
            data_.roi_width_px[det_index] = std::max(0.0f, pixel_box[2] - pixel_box[0]);
            data_.roi_height_px[det_index] = std::max(0.0f, pixel_box[3] - pixel_box[1]);
        }

        constexpr int kMaxDetailLogs = 12;
        static int detail_log_count = 0;
        if (detail_log_count < kMaxDetailLogs && kChaserDebugLoggingEnabled) {
            std::cout << "[HEADING_LOAD_DETAIL] roi=" << roi_index
                      << " frame=" << frame
                      << " det_index=" << det_index
                      << " success=" << static_cast<int>(success)
                      << " raw_kp=(";
            if (std::isfinite(raw_anchor_x) && std::isfinite(raw_anchor_y)) {
                std::cout << raw_anchor_x << ", " << raw_anchor_y << ")";
            } else {
                std::cout << "nan, nan)";
            }
            std::cout << " converted=(";
            if (std::isfinite(assigned_x) && std::isfinite(assigned_y)) {
                std::cout << assigned_x << ", " << assigned_y << ")";
            } else {
                std::cout << "nan, nan)";
            }
            std::cout << " space=";
            switch (keypoint_space) {
                case KeypointSpace::kImage:
                    std::cout << "image";
                    break;
                case KeypointSpace::kRoi:
                    std::cout << "roi";
                    break;
                case KeypointSpace::kNormalized:
                    std::cout << "normalized";
                    break;
            }
            if (std::isfinite(offset_x_det) && std::isfinite(offset_y_det)) {
                std::cout << " roi_offsets=(y=" << offset_y_det << ", x=" << offset_x_det << ")";
            } else {
                std::cout << " roi_offsets=UNAVAILABLE";
            }
            std::cout << " roi_size=(" << roi_height_for_det << ", " << roi_width_for_det << ")";
            if (assigned_anchor) {
                std::cout << " assigned=yes anchor=(" << assigned_x << ", " << assigned_y << ")";
            } else {
                std::cout << " assigned=no anchor=(nan, nan)";
            }
            std::cout << std::endl;
            detail_log_count++;
        }

        filled++;
    }

    data_.has_heading_data = filled > 0;
    if (data_.keypoints_per_detection > 0) {
        data_.has_keypoints = finite_keypoint_count > 0;
        if (!data_.has_keypoints) {
            data_.flat_keypoints_px.clear();
            data_.keypoints_per_detection = 0;
        }
    } else {
        data_.has_keypoints = false;
    }
    if (data_.has_heading_data) {
        data_.keypoints_run_name = latest_run;
        data_.keypoints_source_crop_run = crop_run;

        constexpr size_t kMaxLogEntries = 8;
        if (kChaserDebugLoggingEnabled) {
            for (size_t i = 0; i < std::min(kMaxLogEntries, data_.flat_swim_bladder_px.size()); ++i) {
                const auto& anchor = data_.flat_swim_bladder_px[i];
                const float heading = i < data_.flat_headings_deg.size() ? data_.flat_headings_deg[i] : 0.0f;
                const uint8_t valid = i < data_.flat_heading_valid.size() ? data_.flat_heading_valid[i] : 0;
                std::cout << "[HEADING_LOAD_DEBUG] idx " << i
                          << " anchor=(" << anchor[0] << ", " << anchor[1]
                          << ") heading=" << heading
                          << " valid=" << static_cast<int>(valid)
                          << " finite=" << (std::isfinite(anchor[0]) && std::isfinite(anchor[1]))
                          << std::endl;
            }
        }
    }

    // Load refined keypoint quality arrays when using refined source
    if (is_refined_source && data_.has_heading_data) {
        data_.is_refined_keypoints = true;
        data_.refined_keypoints_run_name = latest_run;

        // Allocate detection-aligned flat vectors with defaults
        data_.flat_keypoint_quality_labels.assign(total_detections, -1);
        data_.flat_keypoint_reason.assign(total_detections, std::string());
        data_.flat_keypoint_flip_corrected.assign(total_detections, 0);
        data_.flat_keypoint_usable.assign(total_detections, 0);
        data_.flat_keypoint_confidence_valid.assign(total_detections, 0);
        data_.flat_keypoint_geometry_valid.assign(total_detections, 0);
        data_.flat_keypoint_refined_success.assign(total_detections, 0);
        data_.flat_keypoint_detection_source.assign(total_detections, 0);

        // Load ROI-aligned quality arrays
        std::vector<int32_t> roi_quality_labels;
        readInt32Array(store, run_base + "quality_labels", roi_quality_labels);

        std::vector<std::string> roi_reason;
        bool reason_loaded = readStringArray(store, run_base + "reason_bytes", roi_reason);
        if (!reason_loaded) {
            if (!readStringArray(store, run_base + "reason", roi_reason)) {
                static bool reason_warned = false;
                if (!reason_warned) {
                    std::cout << "[REFINED_KP_WARNING] reason_bytes and reason arrays unavailable for run '"
                              << latest_run << "'; quality reason display disabled." << std::endl;
                    reason_warned = true;
                }
            }
        }

        std::vector<uint8_t> roi_flip_corrected;
        readBoolArray(store, run_base + "flip_corrected", roi_flip_corrected);

        std::vector<uint8_t> roi_usable;
        readBoolArray(store, run_base + "usable_keypoints", roi_usable);

        std::vector<uint8_t> roi_confidence_valid;
        readBoolArray(store, run_base + "confidence_valid", roi_confidence_valid);

        std::vector<uint8_t> roi_geometry_valid;
        readBoolArray(store, run_base + "geometry_valid", roi_geometry_valid);

        std::vector<uint8_t> roi_refined_success;
        readBoolArray(store, run_base + "refined_success", roi_refined_success);

        std::vector<int32_t> roi_detection_source_i32;
        std::vector<uint8_t> roi_det_source;
        if (readInt32Array(store, run_base + "detection_source", roi_detection_source_i32)) {
            roi_det_source.reserve(roi_detection_source_i32.size());
            for (int32_t v : roi_detection_source_i32) {
                roi_det_source.push_back(static_cast<uint8_t>(v));
            }
        } else {
            readBoolArray(store, run_base + "detection_source", roi_det_source);
        }

        // Scatter ROI arrays → detection-aligned flat vectors using roi_to_det
        for (size_t roi = 0; roi < roi_count; ++roi) {
            size_t det = roi_to_det[roi];
            if (det == SIZE_MAX || det >= total_detections) continue;

            if (roi < roi_quality_labels.size()) {
                data_.flat_keypoint_quality_labels[det] = roi_quality_labels[roi];
            }
            if (roi < roi_reason.size()) {
                data_.flat_keypoint_reason[det] = roi_reason[roi];
            }
            if (roi < roi_flip_corrected.size()) {
                data_.flat_keypoint_flip_corrected[det] = roi_flip_corrected[roi];
            }
            if (roi < roi_usable.size()) {
                data_.flat_keypoint_usable[det] = roi_usable[roi];
            }
            if (roi < roi_confidence_valid.size()) {
                data_.flat_keypoint_confidence_valid[det] = roi_confidence_valid[roi];
            }
            if (roi < roi_geometry_valid.size()) {
                data_.flat_keypoint_geometry_valid[det] = roi_geometry_valid[roi];
            }
            if (roi < roi_refined_success.size()) {
                data_.flat_keypoint_refined_success[det] = roi_refined_success[roi];
            }
            if (roi < roi_det_source.size()) {
                data_.flat_keypoint_detection_source[det] = roi_det_source[roi];
            }
        }

        // Load keypoint_review_status from run attrs (same pattern as detect review)
        auto run_review_attrs = readAttrsAny(store, run_base);
        if (run_review_attrs.has_value() &&
            run_review_attrs->contains("keypoint_review_status") &&
            (*run_review_attrs)["keypoint_review_status"].is_object()) {
            const auto& rs = (*run_review_attrs)["keypoint_review_status"];
            auto str_field = [&](const char* key) -> std::string {
                if (rs.contains(key) && rs[key].is_string()) return rs[key].get<std::string>();
                return "";
            };
            data_.kp_review_state        = str_field("state");
            data_.kp_review_method       = str_field("method");
            data_.kp_review_intended_use = str_field("intended_use");
            data_.kp_review_timestamp    = str_field("timestamp");
            data_.kp_review_reviewer     = str_field("reviewer");
            data_.kp_review_notes        = str_field("notes");
            data_.has_kp_review_status   = !data_.kp_review_state.empty();
        }

        std::cout << "  Refined keypoints run '" << latest_run << "' loaded (review: "
                  << (data_.has_kp_review_status ? data_.kp_review_state : "none") << ")"
                  << std::endl;
    }

    if (!data_.has_eye_angles) {
        loadEyeAngleData(store, roi_count);
    }

    return data_.has_heading_data;
}

bool ZarrDetectionLoader::loadEyeAngleData(const ts::kvstore::KvStore& store,
                                           size_t roi_count) {
    data_.eye_angle_run_name.clear();
    data_.eye_angle_frame_indices.clear();
    data_.eye_angle_valid_mask.clear();
    data_.eye_angle_left_deg.clear();
    data_.eye_angle_right_deg.clear();
    data_.eye_angle_indices_by_frame.clear();
    data_.has_eye_angles = false;

    std::string latest_run;
    if (auto group_attrs = readAttrsAny(store, "analysis/eye_angle_runs")) {
        latest_run = extractLatestRunName(*group_attrs);
    }

    if (latest_run.empty() && !root_path_.empty()) {
        auto fs_candidates = collect_runs_fs(
            root_path_, "analysis/eye_angle_runs",
            {"angles/roi/left_feret_minor_signed_deg"});
        if (!fs_candidates.empty()) {
            latest_run = fs_candidates.back();
        }
    }

    if (latest_run.empty()) {
        return false;
    }

    std::string base = "analysis/eye_angle_runs/" + latest_run + "/angles/roi/";

    std::vector<float> left_angles;
    if (!readFloatArray(store, base + "left_feret_minor_signed_deg", left_angles)) {
        std::cout << "[EYE_ANGLE_WARNING] Failed to read left_feret_minor_signed_deg for run '"
                  << latest_run << "'" << std::endl;
        return false;
    }
    std::vector<float> right_angles;
    if (!readFloatArray(store, base + "right_feret_minor_signed_deg", right_angles)) {
        std::cout << "[EYE_ANGLE_WARNING] Failed to read right_feret_minor_signed_deg for run '"
                  << latest_run << "'" << std::endl;
        return false;
    }

    if (left_angles.empty() || right_angles.empty()) {
        return false;
    }

    std::vector<int32_t> frame_indices;
    readInt32Array(store, base + "frame_indices", frame_indices);

    std::vector<uint8_t> valid_mask;
    if (!readBoolArray(store, base + "valid_mask", valid_mask)) {
        valid_mask.assign(left_angles.size(), 1);
    }

    size_t count = std::min(left_angles.size(), right_angles.size());
    if (!frame_indices.empty()) {
        count = std::min(count, frame_indices.size());
    }
    if (!valid_mask.empty()) {
        count = std::min(count, valid_mask.size());
    }

    if (count == 0) {
        return false;
    }

    left_angles.resize(count);
    right_angles.resize(count);

    if (frame_indices.empty()) {
        frame_indices.assign(count, -1);
    } else if (frame_indices.size() != count) {
        frame_indices.resize(count, -1);
    }

    if (valid_mask.empty()) {
        valid_mask.assign(count, 1);
    } else if (valid_mask.size() != count) {
        valid_mask.resize(count, 1);
    }

    data_.eye_angle_run_name = latest_run;
    data_.eye_angle_left_deg = std::move(left_angles);
    data_.eye_angle_right_deg = std::move(right_angles);
    data_.eye_angle_frame_indices = std::move(frame_indices);
    data_.eye_angle_valid_mask = std::move(valid_mask);

    size_t max_frame_index = 0;
    for (auto frame : data_.eye_angle_frame_indices) {
        if (frame >= 0) {
            max_frame_index = std::max(max_frame_index,
                                       static_cast<size_t>(frame));
        }
    }
    size_t desired_size = std::max({max_frame_index + 1,
                                    data_.total_frames,
                                    roi_count});
    data_.eye_angle_indices_by_frame.clear();
    data_.eye_angle_indices_by_frame.resize(desired_size);
    for (size_t i = 0; i < data_.eye_angle_frame_indices.size(); ++i) {
        int32_t frame = data_.eye_angle_frame_indices[i];
        if (frame < 0) continue;
        size_t frame_index = static_cast<size_t>(frame);
        if (frame_index >= data_.eye_angle_indices_by_frame.size()) {
            data_.eye_angle_indices_by_frame.resize(frame_index + 1);
        }
        data_.eye_angle_indices_by_frame[frame_index].push_back(i);
    }

    data_.has_eye_angles = true;
    std::cout << "  Eye angle run '" << latest_run << "' loaded ("
              << data_.eye_angle_left_deg.size()
              << " ROI entries)" << std::endl;
    if (roi_count > 0 && data_.eye_angle_left_deg.size() != roi_count) {
        std::cout << "    [EYE_ANGLE_WARNING] ROI count mismatch: angles="
                  << data_.eye_angle_left_deg.size()
                  << ", expected " << roi_count << std::endl;
    }

    std::string frame_base = "analysis/eye_angle_runs/" + latest_run + "/angles/frame/";
    std::vector<float> vergence_signed;
    if (!readFloatArray(store, frame_base + "vergence_signed_deg_smoothed", vergence_signed) ||
        vergence_signed.empty()) {
        readFloatArray(store, frame_base + "vergence_signed_deg", vergence_signed);
    }
    if (!vergence_signed.empty()) {
        std::vector<uint8_t> frame_valid;
        readBoolArray(store,
                      "analysis/eye_angle_runs/" + latest_run + "/qa/frame/valid_frame",
                      frame_valid);
        std::vector<float> frame_time_seconds;
        readFloatArray(store,
                       "analysis/eye_angle_runs/" + latest_run + "/support/frame_time_seconds",
                       frame_time_seconds);

        size_t frame_count = vergence_signed.size();
        if (!frame_valid.empty()) {
            frame_count = std::min(frame_count, frame_valid.size());
        }
        if (!frame_time_seconds.empty()) {
            frame_count = std::min(frame_count, frame_time_seconds.size());
        }
        vergence_signed.resize(frame_count);
        if (frame_valid.empty()) {
            frame_valid.assign(frame_count, 1);
        } else {
            frame_valid.resize(frame_count, 1);
        }
        if (frame_time_seconds.empty()) {
            frame_time_seconds.resize(frame_count);
            double fps = data_.fps > 0.0 ? data_.fps : 30.0;
            double inv_fps = fps > 0.0 ? (1.0 / fps) : 0.033333333;
            for (size_t i = 0; i < frame_count; ++i) {
                frame_time_seconds[i] = static_cast<float>(static_cast<double>(i) * inv_fps);
            }
        } else {
            frame_time_seconds.resize(frame_count);
        }

        data_.eye_vergence_signed_frame_deg = std::move(vergence_signed);
        data_.eye_vergence_frame_time_seconds = std::move(frame_time_seconds);
        data_.eye_vergence_frame_valid = std::move(frame_valid);
        data_.has_eye_vergence_frame = !data_.eye_vergence_signed_frame_deg.empty();
    }
    return true;
}
