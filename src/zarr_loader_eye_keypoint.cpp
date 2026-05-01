#include "zarr_loader_internal.h"
#include <iostream>
#include <thread>

namespace {

constexpr size_t kInvalidMaskChannel = std::numeric_limits<size_t>::max();

void clearEyeMaskState(ZarrDetectionData& data) {
    data.eye_masks_run_name.clear();
    data.eye_masks_source_label.clear();
    data.eye_masks_source_path.clear();
    data.eye_masks_warning.clear();
    data.eye_masks_from_refined_subject_masks = false;
    data.eye_masks_tolerant_metadata = false;
    data.eye_masks_loaded = false;
    data.has_eye_masks = false;
    data.eye_masks_store = ts::TensorStore<uint8_t, 4>();
    data.eye_mask_roi_count = 0;
    data.eye_mask_height = 0;
    data.eye_mask_width = 0;
    data.eye_mask_chunk_rows = 0;
    data.eye_mask_channel_indices = {kInvalidMaskChannel, kInvalidMaskChannel};
    data.eye_mask_channel_labels = {"eye_left", "eye_right"};
    data.refined_subject_mask_labels.clear();
    data.refined_subject_mask_available_channels.clear();
    data.refined_subject_mask_overlay_components.clear();
    data.eye_mask_feret_axes_major.clear();
    data.eye_mask_feret_axes_minor.clear();
    data.eye_masks_have_feret_axes = false;
    std::lock_guard<std::mutex> cache_lock(*data.mask_chunk_cache_mutex);
    data.mask_chunk_cache.clear();
    data.mask_chunk_loads_in_flight.clear();
}

std::vector<std::string> extractStringListAttr(const nlohmann::json& attrs,
                                               const char* key) {
    std::vector<std::string> values;
    if (!attrs.contains(key) || !attrs[key].is_array()) {
        return values;
    }
    const auto& array = attrs[key];
    values.reserve(array.size());
    for (const auto& item : array) {
        if (item.is_string()) {
            values.push_back(item.get<std::string>());
        }
    }
    return values;
}

std::string joinMaskLabels(const std::vector<std::string>& labels) {
    std::ostringstream oss;
    for (size_t i = 0; i < labels.size(); ++i) {
        if (i > 0) {
            oss << ", ";
        }
        oss << labels[i];
    }
    return oss.str();
}

std::array<float, 4> invalidAxisSegment() {
    const float nan_value = std::numeric_limits<float>::quiet_NaN();
    return {nan_value, nan_value, nan_value, nan_value};
}

std::array<std::array<float, 4>, 2> invalidEyeAxisRow() {
    return {invalidAxisSegment(), invalidAxisSegment()};
}

void ensureEyeAxisRows(std::vector<std::array<std::array<float, 4>, 2>>& rows,
                       size_t roi_dim) {
    if (rows.size() == roi_dim) {
        return;
    }
    rows.assign(roi_dim, invalidEyeAxisRow());
}

bool stringAttrMatches(const nlohmann::json& attrs,
                       const char* key,
                       const char* expected) {
    return attrs.contains(key) &&
           attrs[key].is_string() &&
           attrs[key].get<std::string>() == expected;
}

std::string jsonStringAttr(const nlohmann::json& attrs,
                           const char* key) {
    if (attrs.contains(key) && attrs[key].is_string()) {
        return attrs[key].get<std::string>();
    }
    return {};
}

int jsonIntAttr(const nlohmann::json& attrs, const char* key) {
    if (attrs.contains(key) && attrs[key].is_number_integer()) {
        return attrs[key].get<int>();
    }
    if (attrs.contains(key) && attrs[key].is_number()) {
        return static_cast<int>(attrs[key].get<double>());
    }
    return -1;
}

std::vector<std::string> jsonStringVector(const nlohmann::json& obj,
                                          const char* key) {
    std::vector<std::string> values;
    if (!obj.contains(key) || !obj[key].is_array()) {
        return values;
    }
    for (const auto& item : obj[key]) {
        if (item.is_string()) {
            values.push_back(item.get<std::string>());
        }
    }
    return values;
}

void appendEyeAngleWarning(
    ZarrDetectionData::EyeAngleAnalysisData& eye_angles,
    const std::string& message) {
    if (message.empty()) {
        return;
    }
    if (!eye_angles.warning.empty()) {
        eye_angles.warning += " ";
    }
    eye_angles.warning += message;
}

void addUniqueString(std::vector<std::string>& values,
                     const std::string& value) {
    if (value.empty()) {
        return;
    }
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

bool endsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string unsmoothedBaseField(const std::string& field_name) {
    constexpr const char* kSmoothedSuffix = "_smoothed";
    if (!endsWith(field_name, kSmoothedSuffix)) {
        return {};
    }
    return field_name.substr(0, field_name.size() - std::strlen(kSmoothedSuffix));
}

ZarrDetectionData::EyeAngleFieldInfo makeEyeAngleFieldInfo(
    const std::string& name,
    const std::string& representation_key,
    const std::string& field_role,
    const std::string& display_name,
    const std::string& units,
    bool default_plot) {
    ZarrDetectionData::EyeAngleFieldInfo info;
    info.name = name;
    info.representation_key = representation_key;
    info.field_role = field_role;
    info.display_name = display_name;
    info.units = units;
    info.default_plot = default_plot;
    return info;
}

void appendCompatibilityEyeAngleSchema(
    ZarrDetectionData::EyeAngleAnalysisData& eye_angles) {
    eye_angles.variant_schema_inferred = true;
    eye_angles.variant_schema_id = "inferred.compatibility_eye_angle_variant_schema";
    eye_angles.default_representation = "eye_frame";
    eye_angles.representation_order = {
        "eye_frame", "gaze", "nasal_gaze", "major", "centroid", "legacy"};

    auto make_rep =
        [](const std::string& key,
           const std::string& display_name,
           std::vector<std::string> default_plot,
           std::vector<std::string> primary,
           std::vector<std::string> aggregate,
           std::vector<std::string> vectors = {})
            -> ZarrDetectionData::EyeAngleRepresentationInfo {
        ZarrDetectionData::EyeAngleRepresentationInfo rep;
        rep.key = key;
        rep.display_name = display_name;
        rep.units = "deg";
        rep.default_plot_fields = std::move(default_plot);
        rep.primary_roi_fields = std::move(primary);
        rep.aggregate_roi_fields = std::move(aggregate);
        rep.vector_roi_fields = std::move(vectors);
        return rep;
    };

    eye_angles.representations = {
        make_rep("eye_frame",
                 "Bianco/Engert eye-frame angles",
                 {"left_eye_angle_deg_smoothed",
                  "right_eye_angle_deg_smoothed",
                  "vergence_eye_angle_deg_smoothed"},
                 {"left_eye_angle_deg", "right_eye_angle_deg"},
                 {"vergence_eye_angle_deg"}),
        make_rep("gaze",
                 "Gaze direction",
                 {"left_gaze_signed_deg_smoothed",
                  "right_gaze_signed_deg_smoothed"},
                 {"left_gaze_signed_deg", "right_gaze_signed_deg"},
                 {"vergence_gaze_deg", "version_gaze_deg"},
                 {"left_gaze_xy", "right_gaze_xy"}),
        make_rep("nasal_gaze",
                 "BEAST/Johnson nasal-gaze convergence",
                 {"left_nasal_gaze_deg_smoothed",
                  "right_nasal_gaze_deg_smoothed",
                  "mean_eye_vergence_gaze_deg_smoothed"},
                 {"left_nasal_gaze_deg", "right_nasal_gaze_deg"},
                 {"mean_eye_vergence_gaze_deg"}),
        make_rep("major",
                 "Canonical major-axis orientation",
                 {"left_major_signed_deg", "right_major_signed_deg"},
                 {"left_major_signed_deg", "right_major_signed_deg"},
                 {"vergence_major_signed_deg", "version_major_deg"}),
        make_rep("centroid",
                 "Centroid-position diagnostics",
                 {"left_centroid_deg_smoothed",
                  "right_centroid_deg_smoothed"},
                 {"left_centroid_deg", "right_centroid_deg"},
                 {"vergence_centroid_deg"}),
        make_rep("legacy",
                 "Legacy compatibility aliases",
                 {},
                 {"left_deg", "right_deg", "left_signed_deg",
                  "right_signed_deg", "left_minor_signed_deg",
                  "right_minor_signed_deg"},
                 {"vergence_deg", "vergence_signed_deg", "version_deg",
                  "vergence_minor_signed_deg", "version_minor_deg"}),
    };
}

std::string decodeEyeAngleReasonCode(
    int32_t code,
    const std::unordered_map<int32_t, std::string>& reason_code_map) {
    if (code == 0) {
        return {};
    }
    std::vector<int32_t> bits;
    bits.reserve(reason_code_map.size());
    for (const auto& entry : reason_code_map) {
        if (entry.first > 0) {
            bits.push_back(entry.first);
        }
    }
    std::sort(bits.begin(), bits.end());
    std::ostringstream oss;
    int32_t known_bits = 0;
    bool first = true;
    for (int32_t bit : bits) {
        if ((code & bit) == 0) {
            continue;
        }
        if (!first) {
            oss << "|";
        }
        auto it = reason_code_map.find(bit);
        oss << (it != reason_code_map.end()
                    ? it->second
                    : ("reason_" + std::to_string(bit)));
        known_bits |= bit;
        first = false;
    }
    const int32_t unknown_bits = code & ~known_bits;
    if (unknown_bits != 0) {
        if (!first) {
            oss << "|";
        }
        oss << "unknown_" << unknown_bits;
    }
    return oss.str();
}

}  // namespace

bool ZarrDetectionLoader::loadKeypointHeadingData(const ts::kvstore::KvStore& store) {
    data_.flat_headings_deg.clear();
    data_.flat_swim_bladder_px.clear();
    data_.flat_heading_valid.clear();
    data_.has_heading_data = false;
    data_.keypoints_run_name.clear();
    data_.keypoints_source_crop_run.clear();
    data_.flat_keypoints_px.clear();
    data_.keypoint_roi_indices.clear();
    data_.keypoint_labels.clear();
    data_.heading_computation_spec = KeypointHeadingComputationSpec{};
    data_.keypoints_per_detection = 0;
    data_.has_keypoints = false;
    data_.mask_roi_indices.clear();
    data_.roi_offset_x.clear();
    data_.roi_offset_y.clear();
    data_.roi_width_px.clear();
    data_.roi_height_px.clear();
    clearEyeMaskState(data_);
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
    data_.eye_angle_analysis = ZarrDetectionData::EyeAngleAnalysisData{};

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
    } else {
        data_.keypoint_labels.clear();
    }

    data_.heading_computation_spec =
        resolveKeypointHeadingComputationSpec(
            run_attrs.has_value() ? &*run_attrs : nullptr,
            data_.keypoint_labels);

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
    data_.keypoint_roi_indices.assign(total_detections, -1);
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
        data_.keypoint_roi_indices[det_index] = static_cast<int32_t>(roi_index);

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
        std::vector<std::array<double, 2>> converted_positions;
        converted_positions.assign(num_keypoints,
                                   std::array<double, 2>{
                                       std::numeric_limits<double>::quiet_NaN(),
                                       std::numeric_limits<double>::quiet_NaN()});

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
                converted_positions[kp_idx] = {
                    static_cast<double>(converted[0]),
                    static_cast<double>(converted[1])};
                if (can_store_flat && std::isfinite(converted[0]) && std::isfinite(converted[1])) {
                    finite_keypoint_count++;
                }
            }
        }

        if (data_.heading_computation_spec.available &&
            data_.heading_computation_spec.enabled) {
            std::array<double, 2> resolved_origin{};
            if (evaluateKeypointHeadingOrigin(data_.heading_computation_spec,
                                              converted_positions,
                                              resolved_origin)) {
                assigned_x = static_cast<float>(resolved_origin[0]);
                assigned_y = static_cast<float>(resolved_origin[1]);
                assigned_anchor = true;
            }
            if (data_.heading_computation_spec.direction_from.indices.size() == 1) {
                const int raw_anchor_idx =
                    data_.heading_computation_spec.direction_from.indices[0];
                if (raw_anchor_idx >= 0 &&
                    static_cast<size_t>(raw_anchor_idx) < num_keypoints) {
                    size_t kp_base = roi_index * num_keypoints * coord_dim +
                                     static_cast<size_t>(raw_anchor_idx) * coord_dim;
                    if (kp_base + 1 < kp_values.size()) {
                        raw_anchor_x = kp_values[kp_base + 0];
                        raw_anchor_y = kp_values[kp_base + 1];
                    }
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

    if (!data_.has_eye_masks) {
        loadRefinedSubjectMaskEyeData(store, roi_count);
    }
    if (!data_.has_eye_masks) {
        loadRefinedEyeMaskData(store, roi_count);
    }
    if (!data_.has_eye_angles) {
        loadEyeAngleData(store, roi_count);
    }

    return data_.has_heading_data;
}

bool ZarrDetectionLoader::loadRefinedSubjectMaskEyeData(
    const ts::kvstore::KvStore& store,
    size_t roi_count) {
    clearEyeMaskState(data_);

    if (data_.layout != ZarrLayoutType::kPaletteRuns) {
        return false;
    }

    std::string latest_run;
    if (auto group_attrs = readAttrsAny(store, "refined_subject_masks_runs")) {
        latest_run = extractLatestRunName(*group_attrs);
    }

    if (latest_run.empty() && !root_path_.empty()) {
        auto fs_candidates = collect_runs_fs(
            root_path_,
            "refined_subject_masks_runs",
            {"masks_roi"});
        if (!fs_candidates.empty()) {
            latest_run = fs_candidates.back();
        }
    }

    if (latest_run.empty()) {
        return false;
    }

    const std::string run_base =
        "refined_subject_masks_runs/" + latest_run + "/";
    auto run_attrs = readAttrsAny(store, run_base);
    if (!run_attrs.has_value()) {
        std::cout << "[SUBJECT_MASK_WARNING] refined_subject_masks run '"
                  << latest_run
                  << "' has no readable attrs; falling back to legacy eye masks."
                  << std::endl;
        return false;
    }

    std::vector<std::string> mask_labels =
        extractStringListAttr(*run_attrs, "mask_labels");
    if (mask_labels.empty()) {
        std::cout << "[SUBJECT_MASK_WARNING] refined_subject_masks run '"
                  << latest_run
                  << "' missing mask_labels; falling back to legacy eye masks."
                  << std::endl;
        return false;
    }

    auto masks_store_result =
        openArrayAny<uint8_t, 4>(store, run_base + "masks_roi", context_);
    if (!masks_store_result.ok()) {
        std::cout << "[SUBJECT_MASK_WARNING] Failed to open refined subject masks for run '"
                  << latest_run << "': "
                  << masks_store_result.status().ToString()
                  << "; falling back to legacy eye masks." << std::endl;
        return false;
    }

    auto domain = masks_store_result.value().domain();
    auto shape = domain.shape();
    if (shape.size() != 4) {
        std::cout << "[SUBJECT_MASK_WARNING] Unexpected masks_roi rank in refined_subject_masks run '"
                  << latest_run << "' (expected 4, got " << shape.size()
                  << "); falling back to legacy eye masks." << std::endl;
        return false;
    }

    const size_t roi_dim = static_cast<size_t>(shape[0]);
    const size_t channel_dim = static_cast<size_t>(shape[1]);
    const size_t mask_rows = static_cast<size_t>(shape[2]);
    const size_t mask_cols = static_cast<size_t>(shape[3]);

    if (roi_dim == 0 || channel_dim == 0 || mask_rows == 0 || mask_cols == 0) {
        std::cout << "[SUBJECT_MASK_WARNING] refined_subject_masks run '"
                  << latest_run
                  << "' has empty masks_roi dimensions; falling back to legacy eye masks."
                  << std::endl;
        return false;
    }

    if (roi_count > 0 && roi_dim != roi_count) {
        std::cout << "[SUBJECT_MASK_WARNING] refined_subject_masks run '"
                  << latest_run << "' masks_roi row count (" << roi_dim
                  << ") does not match keypoint ROI count (" << roi_count
                  << "); falling back to legacy eye masks." << std::endl;
        return false;
    }

    auto find_label = [&](const std::string& label) -> size_t {
        auto it = std::find(mask_labels.begin(), mask_labels.end(), label);
        if (it == mask_labels.end()) {
            return kInvalidMaskChannel;
        }
        return static_cast<size_t>(std::distance(mask_labels.begin(), it));
    };

    const size_t left_channel = find_label("eye_left");
    const size_t right_channel = find_label("eye_right");
    if (left_channel == kInvalidMaskChannel &&
        right_channel == kInvalidMaskChannel) {
        std::cout << "[SUBJECT_MASK_WARNING] refined_subject_masks run '"
                  << latest_run
                  << "' has no eye_left/eye_right labels; falling back to legacy eye masks."
                  << std::endl;
        return false;
    }

    std::vector<uint8_t> available_channels;
    bool available_loaded =
        readBoolArray(store, run_base + "available_channels", available_channels);
    bool tolerant_metadata = false;
    std::string warning;
    if (!available_loaded || available_channels.size() != channel_dim) {
        const size_t observed_available_count = available_channels.size();
        available_channels.assign(channel_dim, 1);
        tolerant_metadata = true;
        std::ostringstream oss;
        oss << "available_channels ";
        if (!available_loaded) {
            oss << "missing/unreadable";
        } else {
            oss << "length " << observed_available_count
                << " did not match masks_roi channel count " << channel_dim;
        }
        oss << "; treating labeled channels as readable for UI inspection.";
        warning = oss.str();
    }

    auto channel_is_available = [&](size_t channel) -> bool {
        if (channel == kInvalidMaskChannel || channel >= channel_dim) {
            return false;
        }
        if (channel >= available_channels.size()) {
            return tolerant_metadata;
        }
        return available_channels[channel] != 0;
    };

    const bool left_available = channel_is_available(left_channel);
    const bool right_available = channel_is_available(right_channel);
    if (!left_available && !right_available) {
        std::cout << "[SUBJECT_MASK_WARNING] refined_subject_masks run '"
                  << latest_run
                  << "' has eye labels but neither eye channel is available; falling back to legacy eye masks."
                  << std::endl;
        return false;
    }

    data_.mask_chunk_cache.clear();
    data_.eye_masks_store = masks_store_result.value();
    data_.eye_masks_run_name = latest_run;
    data_.eye_masks_source_label = tolerant_metadata
        ? "Refined subject masks (metadata inferred)"
        : "Refined subject masks";
    data_.eye_masks_source_path = run_base + "masks_roi";
    data_.eye_masks_warning = warning;
    data_.eye_masks_from_refined_subject_masks = true;
    data_.eye_masks_tolerant_metadata = tolerant_metadata;
    data_.eye_masks_loaded = true;
    data_.has_eye_masks = true;
    data_.eye_mask_roi_count = roi_dim;
    data_.eye_mask_height = mask_rows;
    data_.eye_mask_width = mask_cols;
    data_.eye_mask_channel_indices = {
        left_available ? left_channel : kInvalidMaskChannel,
        right_available ? right_channel : kInvalidMaskChannel};
    data_.eye_mask_channel_labels = {"eye_left", "eye_right"};
    data_.refined_subject_mask_labels = mask_labels;
    data_.refined_subject_mask_available_channels = available_channels;
    data_.refined_subject_mask_overlay_components.clear();
    const size_t component_label_count =
        std::min(mask_labels.size(), channel_dim);
    for (size_t channel = 0; channel < component_label_count; ++channel) {
        if (!channel_is_available(channel)) {
            continue;
        }
        data_.refined_subject_mask_overlay_components.push_back(
            ZarrDetectionData::RefinedSubjectMaskComponentInfo{
                mask_labels[channel],
                channel});
    }

    auto loadComponentContours =
        [&](ZarrDetectionData::RefinedSubjectMaskComponentInfo& component) {
            const std::string contour_base =
                run_base + "components/" + component.label + "/contours/";

            std::vector<int64_t> contour_ptr;
            std::vector<int32_t> contour_len;
            if (!readInt64Array(store, contour_base + "ptr", contour_ptr) ||
                !readInt32Array(store, contour_base + "len", contour_len)) {
                return;
            }
            if (contour_ptr.size() != roi_dim || contour_len.size() != roi_dim) {
                component.contour_warning =
                    "contour ptr/len shape does not match masks_roi rows";
                std::cout << "  [SUBJECT_MASK_WARNING] Refined subject mask component '"
                          << component.label << "' "
                          << component.contour_warning << "." << std::endl;
                return;
            }

            auto points_store =
                openArrayAny<float, 2>(store, contour_base + "points_xy", context_);
            if (!points_store.ok()) {
                component.contour_warning =
                    "contour points_xy is missing or unreadable";
                std::cout << "  [SUBJECT_MASK_WARNING] Refined subject mask component '"
                          << component.label << "' "
                          << component.contour_warning << ": "
                          << points_store.status().ToString()
                          << std::endl;
                return;
            }

            const auto points_shape = points_store.value().domain().shape();
            if (points_shape.size() != 2 || points_shape[1] < 2) {
                component.contour_warning =
                    "contour points_xy shape is not Nx2";
                std::cout << "  [SUBJECT_MASK_WARNING] Refined subject mask component '"
                          << component.label << "' "
                          << component.contour_warning << "." << std::endl;
                return;
            }

            bool attrs_compatible = false;
            if (auto attrs = readAttrsAny(store, contour_base)) {
                const bool schema_ok =
                    stringAttrMatches(*attrs, "schema_id", "component_contours_v1") ||
                    stringAttrMatches(*attrs, "contour_schema_id", "component_contours_v1");
                const bool coordinate_ok =
                    !attrs->contains("coordinate_space") ||
                    stringAttrMatches(*attrs, "coordinate_space", "roi_pixels");
                const bool order_ok =
                    !attrs->contains("point_order") ||
                    stringAttrMatches(*attrs, "point_order", "xy");
                const bool source_ok =
                    !attrs->contains("source_component") ||
                    stringAttrMatches(*attrs,
                                      "source_component",
                                      component.label.c_str());
                attrs_compatible =
                    schema_ok && coordinate_ok && order_ok && source_ok;
                if (!attrs_compatible) {
                    component.contour_warning =
                        "contour attrs are incomplete or not fully compatible; loading arrays tolerantly";
                }
            } else {
                component.contour_warning =
                    "contour attrs are missing; loading arrays tolerantly";
            }

            component.contour_ptr = std::move(contour_ptr);
            component.contour_len = std::move(contour_len);
            component.contour_points_store = points_store.value();
            component.contour_points_count =
                static_cast<size_t>(points_shape[0]);
            component.contours_available = true;
            component.contour_attrs_compatible = attrs_compatible;
        };

    size_t contour_component_count = 0;
    for (auto& component : data_.refined_subject_mask_overlay_components) {
        loadComponentContours(component);
        if (component.contours_available) {
            ++contour_component_count;
        }
    }

    auto loadEllipseAxes =
        [&](const std::string& component_label, size_t eye_slot) -> bool {
        if (eye_slot >= 2) {
            return false;
        }

        const std::string geometry_base =
            run_base + "components/" + component_label + "/geometry/";
        auto ellipse_store =
            openArrayAny<float, 2>(store, geometry_base + "ellipse_params", context_);
        if (!ellipse_store.ok()) {
            std::cout << "  [SUBJECT_MASK_WARNING] Refined subject mask component '"
                      << component_label
                      << "' has no readable ellipse_params; axis overlay unavailable for this eye."
                      << std::endl;
            return false;
        }

        auto ellipse_result = ts::Read(ellipse_store.value()).result();
        if (!ellipse_result.ok()) {
            std::cout << "  [SUBJECT_MASK_WARNING] Failed to read ellipse_params for refined subject mask component '"
                      << component_label << "': "
                      << ellipse_result.status().ToString()
                      << std::endl;
            return false;
        }

        auto ellipse_array = ellipse_result.value();
        auto ellipse_shape = ellipse_array.shape();
        if (ellipse_shape.size() != 2 ||
            ellipse_shape[0] != static_cast<ts::Index>(roi_dim) ||
            ellipse_shape[1] < 5) {
            std::cout << "  [SUBJECT_MASK_WARNING] Unexpected ellipse_params shape for refined subject mask component '"
                      << component_label << "' (expected " << roi_dim
                      << "x5, got ";
            for (size_t i = 0; i < ellipse_shape.size(); ++i) {
                std::cout << ellipse_shape[i]
                          << (i + 1 < ellipse_shape.size() ? "x" : "");
            }
            std::cout << "); axis overlay unavailable for this eye."
                      << std::endl;
            return false;
        }

        std::vector<uint8_t> ellipse_success;
        const bool success_loaded =
            readBoolArray(store, geometry_base + "ellipse_success", ellipse_success);
        if (!success_loaded) {
            ellipse_success.assign(roi_dim, 1);
            std::cout << "  [SUBJECT_MASK_WARNING] Refined subject mask component '"
                      << component_label
                      << "' missing ellipse_success; using finite ellipse_params for axis overlay."
                      << std::endl;
        } else if (ellipse_success.size() != roi_dim) {
            std::cout << "  [SUBJECT_MASK_WARNING] ellipse_success length for refined subject mask component '"
                      << component_label << "' is " << ellipse_success.size()
                      << " but expected " << roi_dim
                      << "; missing rows will be treated as invalid."
                      << std::endl;
        }

        ensureEyeAxisRows(data_.eye_mask_feret_axes_major, roi_dim);
        ensureEyeAxisRows(data_.eye_mask_feret_axes_minor, roi_dim);

        constexpr float kPi = 3.14159265358979323846f;
        size_t valid_axes = 0;
        for (size_t roi = 0; roi < roi_dim; ++roi) {
            if (roi >= ellipse_success.size() || ellipse_success[roi] == 0) {
                continue;
            }

            const float cx =
                ellipse_array(static_cast<ts::Index>(roi), static_cast<ts::Index>(0));
            const float cy =
                ellipse_array(static_cast<ts::Index>(roi), static_cast<ts::Index>(1));
            const float major =
                ellipse_array(static_cast<ts::Index>(roi), static_cast<ts::Index>(2));
            const float minor =
                ellipse_array(static_cast<ts::Index>(roi), static_cast<ts::Index>(3));
            const float angle_deg =
                ellipse_array(static_cast<ts::Index>(roi), static_cast<ts::Index>(4));

            if (!std::isfinite(cx) || !std::isfinite(cy) ||
                !std::isfinite(major) || !std::isfinite(minor) ||
                !std::isfinite(angle_deg) || major <= 1e-5f || minor <= 1e-5f) {
                continue;
            }

            const float theta = angle_deg * kPi / 180.0f;
            const float major_dx = std::cos(theta) * major * 0.5f;
            const float major_dy = std::sin(theta) * major * 0.5f;
            const float minor_theta = theta + 0.5f * kPi;
            const float minor_dx = std::cos(minor_theta) * minor * 0.5f;
            const float minor_dy = std::sin(minor_theta) * minor * 0.5f;

            data_.eye_mask_feret_axes_major[roi][eye_slot] = {
                cx - major_dx,
                cy - major_dy,
                cx + major_dx,
                cy + major_dy};
            data_.eye_mask_feret_axes_minor[roi][eye_slot] = {
                cx - minor_dx,
                cy - minor_dy,
                cx + minor_dx,
                cy + minor_dy};
            ++valid_axes;
        }

        if (valid_axes == 0) {
            std::cout << "  [SUBJECT_MASK_WARNING] Refined subject mask component '"
                      << component_label
                      << "' ellipse geometry contained no valid axis rows."
                      << std::endl;
            return false;
        }

        return true;
    };

    const bool left_geometry_ok =
        left_available && loadEllipseAxes("eye_left", 0);
    const bool right_geometry_ok =
        right_available && loadEllipseAxes("eye_right", 1);
    data_.eye_masks_have_feret_axes = left_geometry_ok || right_geometry_ok;

    data_.eye_mask_chunk_rows = 0;
    auto chunk_layout_result = data_.eye_masks_store.chunk_layout();
    if (chunk_layout_result.ok()) {
        const auto& chunk_layout = chunk_layout_result.value();
        auto chunk_shape = chunk_layout.read_chunk_shape();
        if (!chunk_shape.empty()) {
            auto chunk_size = chunk_shape[0];
            if (chunk_size > 0) {
                data_.eye_mask_chunk_rows =
                    static_cast<size_t>(chunk_size);
            }
        }
    }
    if (data_.eye_mask_chunk_rows == 0) {
        data_.eye_mask_chunk_rows = std::min<size_t>(roi_dim, 512);
    }

    std::cout << "  Refined subject mask run '" << latest_run
              << "' loaded for eye overlay (labels: "
              << joinMaskLabels(mask_labels) << "; eye_left channel "
              << (left_available ? std::to_string(left_channel) : "unavailable")
              << ", eye_right channel "
              << (right_available ? std::to_string(right_channel) : "unavailable")
              << "; ellipse axes "
              << (data_.eye_masks_have_feret_axes ? "loaded" : "unavailable")
              << "; contours " << contour_component_count << "/"
              << data_.refined_subject_mask_overlay_components.size()
              << " components"
              << ")" << std::endl;
    if (!warning.empty()) {
        std::cout << "  [SUBJECT_MASK_WARNING] " << warning << std::endl;
    }
    return true;
}

bool ZarrDetectionLoader::loadRefinedEyeMaskData(const ts::kvstore::KvStore& store,
                                                 size_t roi_count) {
    clearEyeMaskState(data_);

    if (data_.layout != ZarrLayoutType::kPaletteRuns) {
        return false;
    }

    std::string latest_run;
    if (auto group_attrs = readAttrsAny(store, "refined_eye_masks_runs")) {
        latest_run = extractLatestRunName(*group_attrs);
    }

    if (latest_run.empty() && !root_path_.empty()) {
        auto fs_candidates = collect_runs_fs(
            root_path_,
            "refined_eye_masks_runs",
            {"masks_roi"});
        if (!fs_candidates.empty()) {
            latest_run = fs_candidates.back();
        }
    }

    if (latest_run.empty()) {
        return false;
    }

    std::string run_base = "refined_eye_masks_runs/" + latest_run + "/";

    std::vector<int32_t> mask_frame_indices;
    if (!readInt32Array(store, run_base + "frame_indices", mask_frame_indices)) {
        std::cout << "[EYE_MASK_WARNING] refined_eye_masks run '" << latest_run
                  << "' missing frame_indices; skipping mask overlay." << std::endl;
        return false;
    }
    if (mask_frame_indices.size() != roi_count) {
        std::cout << "[EYE_MASK_WARNING] refined_eye_masks run '" << latest_run
                  << "' frame_indices size (" << mask_frame_indices.size()
                  << ") does not match keypoint ROI count (" << roi_count << ")." << std::endl;
        if (mask_frame_indices.empty()) {
            return false;
        }
    }

    auto masks_store_result =
        openArrayAny<uint8_t, 4>(store, run_base + "masks_roi", context_);
    if (!masks_store_result.ok()) {
        std::cout << "[EYE_MASK_WARNING] Failed to open refined eye masks for run '"
                  << latest_run << "': " << masks_store_result.status().ToString() << std::endl;
        return false;
    }

    auto domain = masks_store_result.value().domain();
    auto shape = domain.shape();
    if (shape.size() != 4) {
        std::cout << "[EYE_MASK_WARNING] Unexpected masks_roi rank in run '"
                  << latest_run << "' (expected 4, got " << shape.size() << ")." << std::endl;
        return false;
    }

    size_t roi_dim = static_cast<size_t>(shape[0]);
    size_t channel_dim = static_cast<size_t>(shape[1]);
    size_t mask_rows = static_cast<size_t>(shape[2]);
    size_t mask_cols = static_cast<size_t>(shape[3]);

    if (roi_dim == 0 || channel_dim == 0 || mask_rows == 0 || mask_cols == 0) {
        std::cout << "[EYE_MASK_WARNING] masks_roi dataset '" << latest_run
                  << "' has empty dimensions; skipping." << std::endl;
        return false;
    }

    data_.mask_chunk_cache.clear();
    data_.eye_masks_store = masks_store_result.value();
    data_.eye_masks_run_name = latest_run;
    data_.eye_masks_source_label = "Legacy refined eye masks";
    data_.eye_masks_source_path = run_base + "masks_roi";
    data_.eye_mask_channel_indices = {0, 1};
    data_.eye_mask_channel_labels = {"left", "right"};
    data_.eye_masks_loaded = true;
    data_.has_eye_masks = true;
    data_.eye_mask_roi_count = roi_dim;
    data_.eye_mask_height = mask_rows;
    data_.eye_mask_width = mask_cols;
    data_.eye_mask_chunk_rows = 0;
    auto chunk_layout_result = data_.eye_masks_store.chunk_layout();
    if (chunk_layout_result.ok()) {
        const auto& chunk_layout = chunk_layout_result.value();
        auto chunk_shape = chunk_layout.read_chunk_shape();
        if (!chunk_shape.empty()) {
            auto chunk_size = chunk_shape[0];
            if (chunk_size > 0) {
                data_.eye_mask_chunk_rows =
                    static_cast<size_t>(chunk_size);
            }
        }
    }
    if (data_.eye_mask_chunk_rows == 0) {
        data_.eye_mask_chunk_rows = std::min<size_t>(roi_dim, 512);
    }

    auto loadFeretAxes = [&](const std::string& dataset_name,
                             std::vector<std::array<std::array<float, 4>, 2>>& target) -> bool {
        target.clear();
        auto axes_store =
            openArrayAny<float, 3>(store, run_base + dataset_name, context_);
        if (!axes_store.ok()) {
            return false;
        }
        auto axes_result = ts::Read(axes_store.value()).result();
        if (!axes_result.ok()) {
            std::cout << "[EYE_MASK_WARNING] Failed to read " << dataset_name
                      << " for run '" << latest_run << "': "
                      << axes_result.status().ToString() << std::endl;
            return false;
        }

        auto axes_array = axes_result.value();
        auto axes_shape = axes_array.shape();
        if (axes_shape.size() != 3 ||
            axes_shape[0] != static_cast<ts::Index>(roi_dim) ||
            axes_shape[1] < 1 || axes_shape[2] < 4) {
            std::cout << "[EYE_MASK_WARNING] Unexpected shape for " << dataset_name
                      << " in run '" << latest_run << "' (expected "
                      << roi_dim << "x2x4, got ";
            for (size_t i = 0; i < axes_shape.size(); ++i) {
                std::cout << axes_shape[i] << (i + 1 < axes_shape.size() ? "x" : "");
            }
            std::cout << ")." << std::endl;
            return false;
        }

        const float nan_value = std::numeric_limits<float>::quiet_NaN();
        target.resize(roi_dim);
        for (auto& roi_entry : target) {
            roi_entry = {std::array<float, 4>{nan_value, nan_value, nan_value, nan_value},
                         std::array<float, 4>{nan_value, nan_value, nan_value, nan_value}};
        }

        const size_t eye_dim = std::min<size_t>(2, static_cast<size_t>(axes_shape[1]));
        const size_t axis_len = std::min<size_t>(4, static_cast<size_t>(axes_shape[2]));

        for (size_t roi = 0; roi < roi_dim; ++roi) {
            for (size_t eye = 0; eye < eye_dim; ++eye) {
                std::array<float, 4> values = {nan_value, nan_value, nan_value, nan_value};
                bool all_finite = true;
                for (size_t idx = 0; idx < axis_len; ++idx) {
                    float value = axes_array( static_cast<ts::Index>(roi),
                                             static_cast<ts::Index>(eye),
                                             static_cast<ts::Index>(idx));
                    values[idx] = value;
                    if (!std::isfinite(value)) {
                        all_finite = false;
                        break;
                    }
                }
                if (!all_finite) {
                    continue;
                }
                float dx = values[0] - values[2];
                float dy = values[1] - values[3];
                if (std::fabs(dx) < 1e-5f && std::fabs(dy) < 1e-5f) {
                    continue;
                }
                target[roi][eye] = values;
            }
        }
        return true;
    };

    bool feret_major_ok =
        loadFeretAxes("feret_axes_major", data_.eye_mask_feret_axes_major);
    bool feret_minor_ok =
        loadFeretAxes("feret_axes_minor", data_.eye_mask_feret_axes_minor);
    data_.eye_masks_have_feret_axes = feret_major_ok && feret_minor_ok;

    std::cout << "  Refined eye mask run '" << latest_run
              << "' loaded (" << channel_dim << " channels, "
              << mask_cols << "x" << mask_rows << " masks)" << std::endl;
    return true;
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

    {
        data_.eye_vergence_signed_frame_deg.clear();
        data_.eye_vergence_frame_time_seconds.clear();
        data_.eye_vergence_frame_valid.clear();
        data_.has_eye_vergence_frame = false;
        data_.eye_angle_analysis =
            ZarrDetectionData::EyeAngleAnalysisData{};

        std::string selected_run = requested_eye_angle_run_name_;
        if (selected_run.empty()) {
            if (auto group_attrs = readAttrsAny(store, "analysis/eye_angle_runs")) {
                selected_run = extractLatestRunName(*group_attrs);
            }
        }
        if (selected_run.empty() && !root_path_.empty()) {
            auto candidates = collect_runs_fs(
                root_path_,
                "analysis/eye_angle_runs",
                {"angles/roi/left_eye_angle_deg"});
            if (candidates.empty()) {
                candidates = collect_runs_fs(
                    root_path_,
                    "analysis/eye_angle_runs",
                    {"angles/roi/left_gaze_signed_deg"});
            }
            if (candidates.empty()) {
                candidates = collect_runs_fs(
                    root_path_,
                    "analysis/eye_angle_runs",
                    {"angles/roi/left_minor_signed_deg"});
            }
            if (!candidates.empty()) {
                selected_run = candidates.back();
            }
        }
        if (selected_run.empty()) {
            return false;
        }

        const std::string run_base =
            "analysis/eye_angle_runs/" + selected_run + "/";
        auto run_attrs = readAttrsAny(store, run_base);
        if (!run_attrs.has_value()) {
            if (!requested_eye_angle_run_name_.empty()) {
                std::cout
                    << "  [EYE_ANGLE_WARNING] Requested eye-angle run '"
                    << requested_eye_angle_run_name_ << "' was not found at "
                    << run_base << std::endl;
            }
            return false;
        }

        auto& eye = data_.eye_angle_analysis;
        eye.run_name = selected_run;
        eye.schema_id = jsonStringAttr(*run_attrs, "schema_id");
        eye.schema_version = jsonIntAttr(*run_attrs, "schema_version");
        eye.method = jsonStringAttr(*run_attrs, "method");
        eye.method_version = jsonStringAttr(*run_attrs, "method_version");
        eye.source_geometry_kind =
            jsonStringAttr(*run_attrs, "source_geometry_kind");
        eye.source_eye_geometry_run =
            jsonStringAttr(*run_attrs, "source_eye_geometry_run");
        eye.source_subject_shape_run =
            jsonStringAttr(*run_attrs, "source_subject_shape_run");
        eye.source_refined_subject_masks_run =
            jsonStringAttr(*run_attrs, "source_refined_subject_masks_run");
        eye.source_keypoints_run =
            jsonStringAttr(*run_attrs, "source_keypoints_run");
        if (eye.source_keypoints_run.empty()) {
            eye.source_keypoints_run =
                jsonStringAttr(*run_attrs, "source_keypoint_run");
        }
        if (!eye.schema_id.empty() &&
            eye.schema_id != "analysis.eye_angle_runs") {
            appendEyeAngleWarning(
                eye,
                "Unexpected eye-angle schema_id '" + eye.schema_id +
                    "'; attempting partial load.");
        }
        if (eye.schema_version >= 0 && eye.schema_version < 5) {
            appendEyeAngleWarning(
                eye,
                "Eye-angle run schema_version is older than 5; using compatibility load.");
        }

        if (run_attrs->contains("reason_code_map") &&
            (*run_attrs)["reason_code_map"].is_object()) {
            for (auto it = (*run_attrs)["reason_code_map"].begin();
                 it != (*run_attrs)["reason_code_map"].end(); ++it) {
                try {
                    const int32_t code = std::stoi(it.key());
                    if (it.value().is_string()) {
                        eye.reason_code_map[code] =
                            it.value().get<std::string>();
                    }
                } catch (const std::exception&) {
                    continue;
                }
            }
        }

        nlohmann::json variant_schema;
        if (run_attrs->contains("eye_angle_variant_schema") &&
            (*run_attrs)["eye_angle_variant_schema"].is_object()) {
            variant_schema = (*run_attrs)["eye_angle_variant_schema"];
        }
        if (run_attrs->contains("eye_angle_output_schema") &&
            (*run_attrs)["eye_angle_output_schema"].is_object()) {
            const auto& output_schema = (*run_attrs)["eye_angle_output_schema"];
            eye.output_schema_id = jsonStringAttr(output_schema, "schema_id");
            eye.output_schema_version =
                jsonIntAttr(output_schema, "schema_version");
            if (variant_schema.is_null() &&
                output_schema.contains("variant_schema") &&
                output_schema["variant_schema"].is_object()) {
                variant_schema = output_schema["variant_schema"];
            }
        }
        if (eye.output_schema_version >= 0 && eye.output_schema_version < 7) {
            appendEyeAngleWarning(
                eye,
                "Eye-angle output schema is older than v7; using compatibility UI metadata.");
        }

        if (variant_schema.is_object()) {
            eye.variant_schema_id =
                jsonStringAttr(variant_schema, "schema_id");
            eye.variant_schema_version =
                jsonIntAttr(variant_schema, "schema_version");
            eye.default_representation =
                jsonStringAttr(variant_schema, "default_representation");
            eye.representation_order =
                jsonStringVector(variant_schema, "representation_order");
            if (variant_schema.contains("representations") &&
                variant_schema["representations"].is_object()) {
                const auto& reps = variant_schema["representations"];
                for (const auto& key : eye.representation_order) {
                    if (!reps.contains(key) || !reps[key].is_object()) {
                        continue;
                    }
                    const auto& rep_json = reps[key];
                    ZarrDetectionData::EyeAngleRepresentationInfo rep;
                    rep.key = key;
                    rep.display_name =
                        jsonStringAttr(rep_json, "display_name");
                    rep.role = jsonStringAttr(rep_json, "role");
                    rep.axis = jsonStringAttr(rep_json, "axis");
                    rep.coordinate_frame =
                        jsonStringAttr(rep_json, "coordinate_frame");
                    rep.units = jsonStringAttr(rep_json, "units");
                    rep.sign_convention =
                        jsonStringAttr(rep_json, "sign_convention");
                    rep.derived_from =
                        jsonStringAttr(rep_json, "derived_from");
                    rep.default_plot_fields =
                        jsonStringVector(rep_json, "default_plot_fields");
                    rep.primary_roi_fields =
                        jsonStringVector(rep_json, "primary_roi_fields");
                    rep.aggregate_roi_fields =
                        jsonStringVector(rep_json, "aggregate_roi_fields");
                    rep.vector_roi_fields =
                        jsonStringVector(rep_json, "vector_roi_fields");
                    rep.frame_fields =
                        jsonStringVector(rep_json, "frame_fields");
                    eye.representations.push_back(std::move(rep));
                }
            }
            if (variant_schema.contains("fields") &&
                variant_schema["fields"].is_object()) {
                for (auto it = variant_schema["fields"].begin();
                     it != variant_schema["fields"].end(); ++it) {
                    if (!it.value().is_object()) {
                        continue;
                    }
                    const auto& field_json = it.value();
                    eye.fields.push_back(makeEyeAngleFieldInfo(
                        it.key(),
                        jsonStringAttr(field_json, "representation"),
                        jsonStringAttr(field_json, "field_role"),
                        jsonStringAttr(field_json, "display_name"),
                        jsonStringAttr(field_json, "units"),
                        field_json.contains("default_plot") &&
                            field_json["default_plot"].is_boolean() &&
                            field_json["default_plot"].get<bool>()));
                }
            }
        }
        if (eye.representations.empty()) {
            appendCompatibilityEyeAngleSchema(eye);
            appendEyeAngleWarning(
                eye,
                "eye_angle_variant_schema missing or incomplete; representation metadata is inferred.");
        }
        if (eye.default_representation.empty()) {
            eye.default_representation =
                eye.representation_order.empty()
                    ? "eye_frame"
                    : eye.representation_order.front();
        }

        std::vector<std::string> scalar_fields_to_load;
        std::vector<std::string> vector_fields_to_load;
        for (const auto& rep : eye.representations) {
            for (const auto& field : rep.default_plot_fields) {
                addUniqueString(scalar_fields_to_load, field);
                addUniqueString(scalar_fields_to_load,
                                unsmoothedBaseField(field));
            }
            for (const auto& field : rep.primary_roi_fields) {
                addUniqueString(scalar_fields_to_load, field);
            }
            for (const auto& field : rep.aggregate_roi_fields) {
                addUniqueString(scalar_fields_to_load, field);
            }
            for (const auto& field : rep.frame_fields) {
                addUniqueString(scalar_fields_to_load, field);
                addUniqueString(scalar_fields_to_load, field + "_smoothed");
            }
            for (const auto& field : rep.vector_roi_fields) {
                addUniqueString(vector_fields_to_load, field);
            }
        }
        addUniqueString(scalar_fields_to_load, "left_gaze_signed_deg");
        addUniqueString(scalar_fields_to_load, "right_gaze_signed_deg");
        addUniqueString(scalar_fields_to_load, "left_minor_signed_deg");
        addUniqueString(scalar_fields_to_load, "right_minor_signed_deg");
        addUniqueString(scalar_fields_to_load, "vergence_eye_angle_deg_smoothed");
        addUniqueString(scalar_fields_to_load, "vergence_eye_angle_deg");
        addUniqueString(scalar_fields_to_load, "vergence_signed_deg_smoothed");
        addUniqueString(scalar_fields_to_load, "vergence_signed_deg");
        addUniqueString(vector_fields_to_load, "left_gaze_xy");
        addUniqueString(vector_fields_to_load, "right_gaze_xy");

        auto field_info_for =
            [&](const std::string& name) -> ZarrDetectionData::EyeAngleFieldInfo {
            auto field_it = std::find_if(
                eye.fields.begin(),
                eye.fields.end(),
                [&](const auto& field) { return field.name == name; });
            if (field_it != eye.fields.end()) {
                return *field_it;
            }
            for (const auto& rep : eye.representations) {
                auto contains = [&](const std::vector<std::string>& values) {
                    return std::find(values.begin(), values.end(), name) !=
                           values.end();
                };
                if (contains(rep.default_plot_fields) ||
                    contains(rep.primary_roi_fields) ||
                    contains(rep.aggregate_roi_fields) ||
                    contains(rep.frame_fields) ||
                    contains(rep.vector_roi_fields)) {
                    return makeEyeAngleFieldInfo(
                        name,
                        rep.key,
                        contains(rep.vector_roi_fields) ? "vector_roi" : "",
                        name,
                        rep.units,
                        contains(rep.default_plot_fields));
                }
            }
            return makeEyeAngleFieldInfo(name, {}, {}, name, "deg", false);
        };

        auto readVec2Array =
            [&](const std::string& rel_path,
                std::vector<std::array<float, 2>>& out) -> bool {
            const std::string path = run_base + rel_path;
            auto try_read = [&](auto type_token) -> bool {
                using Source = decltype(type_token);
                auto open_result =
                    openArrayAny<Source, 2>(store, path, context_);
                if (!open_result.ok()) {
                    return false;
                }
                auto array_result = ts::Read(open_result.value()).result();
                if (!array_result.ok()) {
                    return false;
                }
                auto array = array_result.value();
                if (array.rank() != 2 || array.shape()[1] < 2) {
                    return false;
                }
                const size_t rows = static_cast<size_t>(array.shape()[0]);
                out.resize(rows);
                for (size_t row = 0; row < rows; ++row) {
                    out[row] = {
                        static_cast<float>(
                            array(static_cast<ts::Index>(row), 0)),
                        static_cast<float>(
                            array(static_cast<ts::Index>(row), 1))};
                }
                return true;
            };
            return try_read(float{}) || try_read(double{});
        };

        for (const auto& field_name : scalar_fields_to_load) {
            ZarrDetectionData::EyeAngleScalarField field;
            const auto info = field_info_for(field_name);
            field.name = field_name;
            field.representation_key = info.representation_key;
            field.field_role = info.field_role;
            field.display_name =
                info.display_name.empty() ? field_name : info.display_name;
            field.units = info.units;
            field.has_roi = readFloatArray(
                store, run_base + "angles/roi/" + field_name,
                field.roi_values);
            field.has_frame = readFloatArray(
                store, run_base + "angles/frame/" + field_name,
                field.frame_values);
            if (field.has_roi || field.has_frame) {
                if (field.has_roi) {
                    eye.row_count =
                        std::max(eye.row_count, field.roi_values.size());
                }
                if (field.has_frame) {
                    eye.frame_count =
                        std::max(eye.frame_count, field.frame_values.size());
                }
                eye.scalar_fields.push_back(std::move(field));
            }
        }
        for (const auto& field_name : vector_fields_to_load) {
            ZarrDetectionData::EyeAngleVectorField field;
            const auto info = field_info_for(field_name);
            field.name = field_name;
            field.representation_key = info.representation_key;
            field.field_role = info.field_role;
            field.display_name =
                info.display_name.empty() ? field_name : info.display_name;
            field.units = info.units;
            field.has_roi =
                readVec2Array("angles/roi/" + field_name, field.roi_values);
            if (field.has_roi) {
                eye.row_count =
                    std::max(eye.row_count, field.roi_values.size());
                eye.vector_fields.push_back(std::move(field));
            }
        }

        readInt32Array(store, run_base + "support/frame_indices",
                       eye.roi_frame_indices);
        if (eye.roi_frame_indices.empty()) {
            readInt32Array(store, run_base + "angles/roi/frame_indices",
                           eye.roi_frame_indices);
        }
        readFloatArray(store, run_base + "support/time_seconds",
                       eye.roi_time_seconds);
        readFloatArray(store, run_base + "support/frame_time_seconds",
                       eye.frame_time_seconds);
        readBoolArray(store, run_base + "qa/roi/valid_left",
                      eye.roi_valid_left);
        readBoolArray(store, run_base + "qa/roi/valid_right",
                      eye.roi_valid_right);
        readBoolArray(store, run_base + "qa/roi/valid_frame",
                      eye.roi_valid_frame);
        readBoolArray(store, run_base + "qa/frame/valid_frame",
                      eye.frame_valid_frame);
        readBoolArray(store, run_base + "qa/roi/left_major_axis_marginal",
                      eye.roi_left_major_axis_marginal);
        readBoolArray(store, run_base + "qa/roi/right_major_axis_marginal",
                      eye.roi_right_major_axis_marginal);
        readBoolArray(store, run_base + "qa/roi/major_axis_marginal",
                      eye.roi_major_axis_marginal);
        readBoolArray(store, run_base + "qa/frame/major_axis_marginal",
                      eye.frame_major_axis_marginal);
        readInt32Array(store, run_base + "qa/roi/reason_codes",
                       eye.roi_reason_codes);
        readInt32Array(store, run_base + "qa/frame/reason_codes",
                       eye.frame_reason_codes);

        eye.row_count = std::max(
            {eye.row_count,
             eye.roi_frame_indices.size(),
             eye.roi_time_seconds.size(),
             eye.roi_valid_left.size(),
             eye.roi_valid_right.size(),
             eye.roi_valid_frame.size(),
             eye.roi_reason_codes.size()});
        eye.frame_count = std::max(
            {eye.frame_count,
             eye.frame_time_seconds.size(),
             eye.frame_valid_frame.size(),
             eye.frame_reason_codes.size()});
        if (eye.row_count == 0 && eye.frame_count == 0) {
            return false;
        }
        eye.roi_reason_labels.resize(eye.roi_reason_codes.size());
        for (size_t i = 0; i < eye.roi_reason_codes.size(); ++i) {
            eye.roi_reason_labels[i] =
                decodeEyeAngleReasonCode(eye.roi_reason_codes[i],
                                         eye.reason_code_map);
        }
        eye.frame_reason_labels.resize(eye.frame_reason_codes.size());
        for (size_t i = 0; i < eye.frame_reason_codes.size(); ++i) {
            eye.frame_reason_labels[i] =
                decodeEyeAngleReasonCode(eye.frame_reason_codes[i],
                                         eye.reason_code_map);
        }
        if (!eye.roi_frame_indices.empty() &&
            eye.roi_frame_indices.size() == eye.row_count) {
            eye.row_to_frame = eye.roi_frame_indices;
        } else {
            eye.row_to_frame.assign(eye.row_count, -1);
            appendEyeAngleWarning(
                eye,
                "Eye-angle support/frame_indices missing or length-mismatched; QC seeking may be unavailable.");
        }

        auto scalar_roi_values =
            [&](const std::vector<std::string>& candidates)
                -> const std::vector<float>* {
            for (const auto& name : candidates) {
                auto it = std::find_if(
                    eye.scalar_fields.begin(),
                    eye.scalar_fields.end(),
                    [&](const auto& field) { return field.name == name; });
                if (it != eye.scalar_fields.end() && it->has_roi &&
                    !it->roi_values.empty()) {
                    return &it->roi_values;
                }
            }
            return nullptr;
        };
        const auto* left_angle_source = scalar_roi_values(
            {"left_gaze_signed_deg", "left_minor_signed_deg",
             "left_feret_minor_signed_deg"});
        const auto* right_angle_source = scalar_roi_values(
            {"right_gaze_signed_deg", "right_minor_signed_deg",
             "right_feret_minor_signed_deg"});
        if (left_angle_source != nullptr && right_angle_source != nullptr) {
            size_t count =
                std::min(left_angle_source->size(),
                         right_angle_source->size());
            if (eye.row_count > 0) {
                count = std::min(count, eye.row_count);
            }
            data_.eye_angle_left_deg.assign(
                left_angle_source->begin(),
                left_angle_source->begin() + count);
            data_.eye_angle_right_deg.assign(
                right_angle_source->begin(),
                right_angle_source->begin() + count);
            if (!eye.row_to_frame.empty()) {
                data_.eye_angle_frame_indices.assign(
                    eye.row_to_frame.begin(),
                    eye.row_to_frame.begin() +
                        std::min(count, eye.row_to_frame.size()));
            }
            data_.eye_angle_frame_indices.resize(count, -1);
            data_.eye_angle_valid_mask.assign(count, 1);
            for (size_t i = 0; i < count; ++i) {
                const bool frame_valid =
                    eye.roi_valid_frame.empty() ||
                    (i < eye.roi_valid_frame.size() &&
                     eye.roi_valid_frame[i] != 0);
                data_.eye_angle_valid_mask[i] = frame_valid ? 1 : 0;
            }
            data_.has_eye_angles = true;
        } else {
            appendEyeAngleWarning(
                eye,
                "No ROI gaze/minor signed fields were available for eye-angle arc overlays.");
        }
        data_.eye_angle_run_name = selected_run;

        size_t max_frame_index = 0;
        for (auto frame : data_.eye_angle_frame_indices) {
            if (frame >= 0) {
                max_frame_index =
                    std::max(max_frame_index, static_cast<size_t>(frame));
            }
        }
        const size_t desired_size =
            std::max({max_frame_index + 1, data_.total_frames, roi_count});
        data_.eye_angle_indices_by_frame.assign(desired_size, {});
        for (size_t i = 0; i < data_.eye_angle_frame_indices.size(); ++i) {
            const int32_t frame = data_.eye_angle_frame_indices[i];
            if (frame < 0) {
                continue;
            }
            const size_t frame_index = static_cast<size_t>(frame);
            if (frame_index >= data_.eye_angle_indices_by_frame.size()) {
                data_.eye_angle_indices_by_frame.resize(frame_index + 1);
            }
            data_.eye_angle_indices_by_frame[frame_index].push_back(i);
        }

        auto scalar_field_by_name =
            [&](const std::vector<std::string>& candidates)
                -> const ZarrDetectionData::EyeAngleScalarField* {
            for (const auto& name : candidates) {
                auto it = std::find_if(
                    eye.scalar_fields.begin(),
                    eye.scalar_fields.end(),
                    [&](const auto& field) { return field.name == name; });
                if (it != eye.scalar_fields.end()) {
                    return &(*it);
                }
            }
            return nullptr;
        };
        const auto* vergence_field = scalar_field_by_name(
            {"vergence_eye_angle_deg_smoothed", "vergence_eye_angle_deg",
             "vergence_signed_deg_smoothed", "vergence_signed_deg"});
        if (vergence_field != nullptr) {
            if (vergence_field->has_frame &&
                !vergence_field->frame_values.empty()) {
                data_.eye_vergence_signed_frame_deg =
                    vergence_field->frame_values;
            } else if (vergence_field->has_roi &&
                       !vergence_field->roi_values.empty()) {
                data_.eye_vergence_signed_frame_deg =
                    vergence_field->roi_values;
            }
            if (!data_.eye_vergence_signed_frame_deg.empty()) {
                const size_t frame_count =
                    data_.eye_vergence_signed_frame_deg.size();
                if (vergence_field->has_frame &&
                    eye.frame_time_seconds.size() == frame_count) {
                    data_.eye_vergence_frame_time_seconds =
                        eye.frame_time_seconds;
                } else if (eye.roi_time_seconds.size() == frame_count) {
                    data_.eye_vergence_frame_time_seconds =
                        eye.roi_time_seconds;
                } else {
                    data_.eye_vergence_frame_time_seconds.resize(frame_count);
                    const double fps = data_.fps > 0.0 ? data_.fps : 30.0;
                    const double inv_fps =
                        fps > 0.0 ? (1.0 / fps) : 0.033333333;
                    for (size_t i = 0; i < frame_count; ++i) {
                        data_.eye_vergence_frame_time_seconds[i] =
                            static_cast<float>(
                                static_cast<double>(i) * inv_fps);
                    }
                }
                if (vergence_field->has_frame &&
                    eye.frame_valid_frame.size() == frame_count) {
                    data_.eye_vergence_frame_valid = eye.frame_valid_frame;
                } else if (eye.roi_valid_frame.size() == frame_count) {
                    data_.eye_vergence_frame_valid = eye.roi_valid_frame;
                } else {
                    data_.eye_vergence_frame_valid.assign(frame_count, 1);
                }
                data_.has_eye_vergence_frame = true;
            }
        }

        eye.loaded = true;
        std::cout << "  Eye angle run '" << selected_run << "' loaded ("
                  << eye.row_count << " ROI rows, " << eye.frame_count
                  << " frame rows, default representation '"
                  << eye.default_representation << "')" << std::endl;
        if (roi_count > 0 && eye.row_count != 0 && eye.row_count != roi_count) {
            std::cout << "    [EYE_ANGLE_WARNING] ROI count mismatch: angles="
                      << eye.row_count << ", expected " << roi_count
                      << std::endl;
        }
        if (!eye.warning.empty()) {
            std::cout << "  [EYE_ANGLE_WARNING] " << eye.warning
                      << std::endl;
        }
        return true;
    }
    return false;
}

const ZarrDetectionData::EyeAngleScalarField*
ZarrDetectionLoader::findEyeAngleScalarField(
    const std::string& field_name) const {
    const auto& fields = data_.eye_angle_analysis.scalar_fields;
    auto it = std::find_if(
        fields.begin(),
        fields.end(),
        [&](const auto& field) { return field.name == field_name; });
    if (it == fields.end()) {
        return nullptr;
    }
    return &(*it);
}

const ZarrDetectionData::EyeAngleVectorField*
ZarrDetectionLoader::findEyeAngleVectorField(
    const std::string& field_name) const {
    const auto& fields = data_.eye_angle_analysis.vector_fields;
    auto it = std::find_if(
        fields.begin(),
        fields.end(),
        [&](const auto& field) { return field.name == field_name; });
    if (it == fields.end()) {
        return nullptr;
    }
    return &(*it);
}

ZarrDetectionLoader::EyeAngleQcJumpResult
ZarrDetectionLoader::computeEyeAngleQcJump(
    const EyeAngleQcFilterOptions& filters,
    int current_frame_num,
    bool forward) const {
    EyeAngleQcJumpResult result;
    const auto& eye = data_.eye_angle_analysis;
    if (!eye.loaded || eye.row_count == 0) {
        result.status = "Eye-angle data unavailable.";
        return result;
    }

    const std::string reason_filter =
        toLowerCopy(filters.reason_substring);
    const bool filter_enabled =
        filters.invalid_rows ||
        filters.major_axis_marginal ||
        !reason_filter.empty();
    if (!filter_enabled) {
        result.status = "Enable at least one eye-angle QC filter.";
        return result;
    }

    auto boolAtRow = [](const std::vector<uint8_t>& values,
                        size_t row) -> bool {
        return row < values.size() && values[row] != 0;
    };
    auto rowInvalid = [&](size_t row) -> bool {
        const bool left_invalid =
            row < eye.roi_valid_left.size() && eye.roi_valid_left[row] == 0;
        const bool right_invalid =
            row < eye.roi_valid_right.size() && eye.roi_valid_right[row] == 0;
        const bool frame_invalid =
            row < eye.roi_valid_frame.size() && eye.roi_valid_frame[row] == 0;
        return left_invalid || right_invalid || frame_invalid;
    };
    auto rowMarginal = [&](size_t row) -> bool {
        return boolAtRow(eye.roi_major_axis_marginal, row) ||
               boolAtRow(eye.roi_left_major_axis_marginal, row) ||
               boolAtRow(eye.roi_right_major_axis_marginal, row);
    };
    auto rowHasReason = [&](size_t row) -> bool {
        if (reason_filter.empty() || row >= eye.roi_reason_labels.size()) {
            return false;
        }
        return toLowerCopy(eye.roi_reason_labels[row]).find(reason_filter) !=
               std::string::npos;
    };
    auto rowMatches = [&](size_t row) -> bool {
        bool matched = false;
        matched = matched || (filters.invalid_rows && rowInvalid(row));
        matched = matched ||
                  (filters.major_axis_marginal && rowMarginal(row));
        matched = matched || rowHasReason(row);
        return matched;
    };

    std::vector<std::pair<int, size_t>> frame_rows;
    frame_rows.reserve(eye.row_count);
    for (size_t row = 0; row < eye.row_count; ++row) {
        if (!rowMatches(row)) {
            continue;
        }
        result.match_count++;
        if (row < eye.row_to_frame.size() && eye.row_to_frame[row] >= 0) {
            frame_rows.emplace_back(eye.row_to_frame[row], row);
        }
    }
    std::sort(frame_rows.begin(), frame_rows.end());
    frame_rows.erase(std::unique(frame_rows.begin(), frame_rows.end()),
                     frame_rows.end());
    if (frame_rows.empty()) {
        if (result.match_count == 0) {
            result.status = "No eye-angle rows match QC filters.";
        } else {
            result.status =
                "Eye-angle rows matched, but no video frame mapping is available.";
        }
        return result;
    }

    auto select_forward = [&]() -> std::pair<int, size_t> {
        auto it = std::upper_bound(
            frame_rows.begin(),
            frame_rows.end(),
            std::make_pair(current_frame_num,
                           std::numeric_limits<size_t>::max()));
        if (it == frame_rows.end()) {
            it = frame_rows.begin();
        }
        return *it;
    };
    auto select_backward = [&]() -> std::pair<int, size_t> {
        auto it = std::lower_bound(
            frame_rows.begin(),
            frame_rows.end(),
            std::make_pair(current_frame_num, static_cast<size_t>(0)));
        if (it == frame_rows.begin()) {
            return frame_rows.back();
        }
        --it;
        return *it;
    };
    const auto selected = forward ? select_forward() : select_backward();
    result.target_frame = selected.first;
    result.target_row = selected.second;
    result.status = "Eye-angle QC rows: " +
                    std::to_string(result.match_count) +
                    " matching rows across " +
                    std::to_string(frame_rows.size()) + " frame/row entries.";
    return result;
}

std::optional<size_t> ZarrDetectionLoader::findEyeAngleRowForFrame(
    int frame) const {
    const auto& eye = data_.eye_angle_analysis;
    if (!eye.loaded || frame < 0) {
        return std::nullopt;
    }
    for (size_t row = 0; row < eye.row_to_frame.size(); ++row) {
        if (eye.row_to_frame[row] == frame) {
            return row;
        }
    }
    return std::nullopt;
}

std::optional<int32_t> ZarrDetectionLoader::getEyeAngleFrameForRow(
    size_t row) const {
    const auto& eye = data_.eye_angle_analysis;
    if (!eye.loaded || row >= eye.row_to_frame.size() ||
        eye.row_to_frame[row] < 0) {
        return std::nullopt;
    }
    return eye.row_to_frame[row];
}

const ZarrDetectionData::EyeMaskChunkCacheEntry*
ZarrDetectionLoader::findEyeMaskChunk(size_t chunk_id) const {
    for (auto& entry : data_.mask_chunk_cache) {
        if (entry.chunk_id == chunk_id) {
            return &entry;
        }
    }
    return nullptr;
}

bool ZarrDetectionLoader::ensureEyeMaskChunk(size_t chunk_id,
                                             bool allow_prefetch) const {
    if (!data_.eye_masks_loaded || data_.eye_mask_roi_count == 0) {
        return false;
    }

    bool cache_hit = false;
    {
        std::lock_guard<std::mutex> cache_lock(*data_.mask_chunk_cache_mutex);
        auto& cache = data_.mask_chunk_cache;
        auto it = std::find_if(
            cache.begin(),
            cache.end(),
            [&](const ZarrDetectionData::EyeMaskChunkCacheEntry& entry) {
                return entry.chunk_id == chunk_id;
            });
        if (it != cache.end()) {
            if (std::next(it) != cache.end()) {
                ZarrDetectionData::EyeMaskChunkCacheEntry entry = std::move(*it);
                cache.erase(it);
                cache.push_back(std::move(entry));
            }
            cache_hit = true;
        }
    }
    if (cache_hit) {
        if (allow_prefetch) {
            prefetchAdjacentEyeMaskChunks(chunk_id);
        }
        return true;
    }

    size_t chunk_rows =
        data_.eye_mask_chunk_rows > 0 ? data_.eye_mask_chunk_rows : 512;
    size_t chunk_start = chunk_id * chunk_rows;
    if (chunk_start >= data_.eye_mask_roi_count) {
        return false;
    }
    size_t chunk_end =
        std::min(chunk_start + chunk_rows, data_.eye_mask_roi_count);

    auto slice = data_.eye_masks_store |
                 ts::Dims(0).HalfOpenInterval(
                     static_cast<ts::Index>(chunk_start),
                     static_cast<ts::Index>(chunk_end));
    auto read_result = ts::Read(slice).result();
    if (!read_result.ok()) {
        std::cerr << "[EYE_MASK_WARNING] Failed to read mask chunk "
                  << chunk_id << ": " << read_result.status().ToString()
                  << std::endl;
        return false;
    }

    auto array = read_result.value();
    auto shape = array.shape();
    if (shape.size() != 4) {
        std::cerr << "[EYE_MASK_WARNING] Unexpected mask chunk rank ("
                  << shape.size() << ")" << std::endl;
        return false;
    }

    size_t chunk_len = static_cast<size_t>(shape[0]);
    size_t channels = static_cast<size_t>(shape[1]);
    size_t rows = static_cast<size_t>(shape[2]);
    size_t cols = static_cast<size_t>(shape[3]);

    ZarrDetectionData::EyeMaskChunkCacheEntry entry;
    entry.chunk_id = chunk_id;
    entry.chunk_start = chunk_start;
    entry.chunk_length = chunk_len;
    entry.pixel_indices.resize(chunk_len);
    const size_t component_count =
        data_.refined_subject_mask_overlay_components.size();
    if (component_count > 0) {
        entry.component_pixel_indices.resize(chunk_len);
        entry.component_contours_xy.resize(chunk_len);
    }

    const uint8_t* base_ptr =
        static_cast<const uint8_t*>(array.byte_strided_origin_pointer());
    auto byte_strides = array.byte_strides();
    if (byte_strides.size() != 4) {
        std::cerr << "[EYE_MASK_WARNING] Unexpected mask chunk stride rank ("
                  << byte_strides.size() << ")" << std::endl;
        return false;
    }

    const ts::Index stride_roi = byte_strides[0];
    const ts::Index stride_channel = byte_strides[1];
    const ts::Index stride_row = byte_strides[2];
    const ts::Index stride_col = byte_strides[3];

    auto collectChannelPixels =
        [&](const uint8_t* roi_ptr,
            size_t source_channel,
            std::vector<uint32_t>& indices_vec) {
            indices_vec.clear();
            indices_vec.reserve(256);
            if (source_channel == kInvalidMaskChannel ||
                source_channel >= channels) {
                return;
            }

            const auto channel_offset =
                stride_channel * static_cast<ts::Index>(source_channel);
            const uint8_t* channel_ptr = roi_ptr + channel_offset;
            for (size_t r = 0; r < rows; ++r) {
                const auto row_offset =
                    stride_row * static_cast<ts::Index>(r);
                const uint8_t* row_ptr = channel_ptr + row_offset;
                for (size_t c = 0; c < cols; ++c) {
                    const auto col_offset =
                        stride_col * static_cast<ts::Index>(c);
                    const uint8_t* elem_ptr = row_ptr + col_offset;
                    if (*elem_ptr != 0) {
                        indices_vec.push_back(
                            static_cast<uint32_t>(r * cols + c));
                    }
                }
            }
        };

    for (size_t roi = 0; roi < chunk_len; ++roi) {
        const auto roi_offset =
            stride_roi * static_cast<ts::Index>(roi);
        const uint8_t* roi_ptr = base_ptr + roi_offset;

        if (component_count > 0) {
            auto& components_for_roi = entry.component_pixel_indices[roi];
            components_for_roi.resize(component_count);
            entry.component_contours_xy[roi].resize(component_count);
            for (size_t component = 0; component < component_count; ++component) {
                collectChannelPixels(
                    roi_ptr,
                    data_.refined_subject_mask_overlay_components[component]
                        .channel_index,
                    components_for_roi[component]);
            }

            for (size_t eye = 0; eye < 2; ++eye) {
                auto& indices_vec = entry.pixel_indices[roi][eye];
                const size_t source_channel =
                    data_.eye_mask_channel_indices[eye];
                auto component_it = std::find_if(
                    data_.refined_subject_mask_overlay_components.begin(),
                    data_.refined_subject_mask_overlay_components.end(),
                    [&](const ZarrDetectionData::RefinedSubjectMaskComponentInfo&
                            component) {
                        return component.channel_index == source_channel;
                    });
                if (component_it ==
                    data_.refined_subject_mask_overlay_components.end()) {
                    indices_vec.clear();
                    continue;
                }
                const size_t component_index = static_cast<size_t>(
                    std::distance(
                        data_.refined_subject_mask_overlay_components.begin(),
                        component_it));
                indices_vec = components_for_roi[component_index];
            }
            continue;
        }

        for (size_t eye = 0; eye < 2; ++eye) {
            auto& indices_vec = entry.pixel_indices[roi][eye];
            collectChannelPixels(
                roi_ptr,
                data_.eye_mask_channel_indices[eye],
                indices_vec);
        }
    }

    for (size_t component = 0; component < component_count; ++component) {
        const auto& component_info =
            data_.refined_subject_mask_overlay_components[component];
        if (!component_info.contours_available ||
            component_info.contour_ptr.size() < data_.eye_mask_roi_count ||
            component_info.contour_len.size() < data_.eye_mask_roi_count ||
            component_info.contour_points_count == 0) {
            continue;
        }

        int64_t read_start = std::numeric_limits<int64_t>::max();
        int64_t read_end = 0;
        std::vector<uint8_t> row_has_contour(chunk_len, 0);
        for (size_t local_roi = 0; local_roi < chunk_len; ++local_roi) {
            const size_t absolute_roi = chunk_start + local_roi;
            if (absolute_roi >= data_.eye_mask_roi_count ||
                absolute_roi >= component_info.contour_ptr.size() ||
                absolute_roi >= component_info.contour_len.size()) {
                continue;
            }
            const int64_t start = component_info.contour_ptr[absolute_roi];
            const int32_t length = component_info.contour_len[absolute_roi];
            if (start < 0 || length <= 1) {
                continue;
            }
            const int64_t end = start + static_cast<int64_t>(length);
            if (end <= start ||
                static_cast<uint64_t>(end) >
                    static_cast<uint64_t>(component_info.contour_points_count)) {
                continue;
            }
            row_has_contour[local_roi] = 1;
            read_start = std::min(read_start, start);
            read_end = std::max(read_end, end);
        }

        if (read_start == std::numeric_limits<int64_t>::max() ||
            read_end <= read_start) {
            continue;
        }

        auto contour_slice = component_info.contour_points_store |
            ts::Dims(0).HalfOpenInterval(
                static_cast<ts::Index>(read_start),
                static_cast<ts::Index>(read_end));
        auto contour_result = ts::Read(contour_slice).result();
        if (!contour_result.ok()) {
            std::cerr << "[SUBJECT_MASK_WARNING] Failed to read contour chunk for component '"
                      << component_info.label << "': "
                      << contour_result.status().ToString()
                      << std::endl;
            continue;
        }

        auto contour_array = contour_result.value();
        auto contour_shape = contour_array.shape();
        auto contour_strides = contour_array.byte_strides();
        if (contour_shape.size() != 2 || contour_shape[1] < 2 ||
            contour_strides.size() != 2) {
            std::cerr << "[SUBJECT_MASK_WARNING] Unexpected contour chunk shape for component '"
                      << component_info.label << "'" << std::endl;
            continue;
        }

        const float* contour_origin =
            static_cast<const float*>(
                contour_array.byte_strided_origin_pointer());
        const uint8_t* contour_base =
            reinterpret_cast<const uint8_t*>(contour_origin);
        const ts::Index stride_point = contour_strides[0];
        const ts::Index stride_xy = contour_strides[1];
        auto pointValue = [&](size_t local_point, size_t xy) -> float {
            const uint8_t* ptr = contour_base +
                stride_point * static_cast<ts::Index>(local_point) +
                stride_xy * static_cast<ts::Index>(xy);
            return *reinterpret_cast<const float*>(ptr);
        };

        for (size_t local_roi = 0; local_roi < chunk_len; ++local_roi) {
            if (row_has_contour[local_roi] == 0) {
                continue;
            }
            const size_t absolute_roi = chunk_start + local_roi;
            const int64_t start = component_info.contour_ptr[absolute_roi];
            const int32_t length = component_info.contour_len[absolute_roi];
            const int64_t local_start = start - read_start;
            if (local_start < 0) {
                continue;
            }

            auto& contour_points =
                entry.component_contours_xy[local_roi][component];
            contour_points.clear();
            contour_points.reserve(static_cast<size_t>(length));
            for (int32_t point = 0; point < length; ++point) {
                const size_t local_point =
                    static_cast<size_t>(local_start + point);
                if (local_point >= static_cast<size_t>(contour_shape[0])) {
                    break;
                }
                const float x = pointValue(local_point, 0);
                const float y = pointValue(local_point, 1);
                if (std::isfinite(x) && std::isfinite(y)) {
                    contour_points.push_back({x, y});
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> cache_lock(*data_.mask_chunk_cache_mutex);
        auto& cache = data_.mask_chunk_cache;
        auto existing = std::find_if(
            cache.begin(),
            cache.end(),
            [&](const ZarrDetectionData::EyeMaskChunkCacheEntry& cache_entry) {
                return cache_entry.chunk_id == chunk_id;
            });
        if (existing == cache.end()) {
            if (cache.size() >= kEyeMaskChunkCacheCapacity) {
                cache.erase(cache.begin());
            }
            cache.push_back(std::move(entry));
        }
    }

    if (allow_prefetch) {
        prefetchAdjacentEyeMaskChunks(chunk_id);
    }
    return true;
}

void ZarrDetectionLoader::prefetchAdjacentEyeMaskChunks(size_t chunk_id) const {
    size_t chunk_rows =
        data_.eye_mask_chunk_rows > 0 ? data_.eye_mask_chunk_rows : 512;
    if (chunk_id > 0) {
        requestEyeMaskChunkPrefetch(chunk_id - 1);
    }
    if ((chunk_id + 1) * chunk_rows < data_.eye_mask_roi_count) {
        requestEyeMaskChunkPrefetch(chunk_id + 1);
    }
}

void ZarrDetectionLoader::requestEyeMaskChunkPrefetch(size_t chunk_id) const {
    if (!data_.eye_masks_loaded || data_.eye_mask_roi_count == 0) {
        return;
    }

    size_t chunk_rows =
        data_.eye_mask_chunk_rows > 0 ? data_.eye_mask_chunk_rows : 512;
    if (chunk_id * chunk_rows >= data_.eye_mask_roi_count) {
        return;
    }

    {
        std::lock_guard<std::mutex> cache_lock(*data_.mask_chunk_cache_mutex);
        const bool already_cached =
            std::find_if(
                data_.mask_chunk_cache.begin(),
                data_.mask_chunk_cache.end(),
                [&](const ZarrDetectionData::EyeMaskChunkCacheEntry& entry) {
                    return entry.chunk_id == chunk_id;
                }) != data_.mask_chunk_cache.end();
        if (already_cached ||
            data_.mask_chunk_loads_in_flight.count(chunk_id) != 0) {
            return;
        }
        data_.mask_chunk_loads_in_flight.insert(chunk_id);
    }

    std::thread([this, chunk_id]() {
        ensureEyeMaskChunk(chunk_id, /*allow_prefetch=*/false);
        std::lock_guard<std::mutex> cache_lock(*data_.mask_chunk_cache_mutex);
        data_.mask_chunk_loads_in_flight.erase(chunk_id);
    }).detach();
}

bool ZarrDetectionLoader::readRefinedSubjectMaskComponentRow(
    size_t roi_index,
    const std::string& component_name,
    RefinedSubjectMaskComponentRow& out_row,
    std::string* error_message) const {
    out_row = RefinedSubjectMaskComponentRow{};

    auto fail = [&](const std::string& message) -> bool {
        if (error_message != nullptr) {
            *error_message = message;
        }
        return false;
    };

    if (!data_.eye_masks_loaded || !data_.eye_masks_from_refined_subject_masks) {
        return fail("Refined subject masks are not the active mask source.");
    }
    if (roi_index >= data_.eye_mask_roi_count) {
        std::ostringstream oss;
        oss << "ROI row " << roi_index
            << " is outside refined subject-mask row count "
            << data_.eye_mask_roi_count << ".";
        return fail(oss.str());
    }
    if (component_name.empty()) {
        return fail("No refined subject-mask component selected.");
    }

    auto component_it = std::find_if(
        data_.refined_subject_mask_overlay_components.begin(),
        data_.refined_subject_mask_overlay_components.end(),
        [&](const ZarrDetectionData::RefinedSubjectMaskComponentInfo& component) {
            return component.label == component_name;
        });
    if (component_it == data_.refined_subject_mask_overlay_components.end()) {
        return fail("Selected refined subject-mask component is unavailable: " +
                    component_name);
    }

    const size_t channel_index = component_it->channel_index;
    if (channel_index == kInvalidMaskChannel ||
        channel_index >= data_.refined_subject_mask_labels.size()) {
        return fail("Selected refined subject-mask component has no readable channel: " +
                    component_name);
    }
    if (channel_index >= data_.refined_subject_mask_available_channels.size() ||
        data_.refined_subject_mask_available_channels[channel_index] == 0) {
        return fail("Selected refined subject-mask component is not marked available: " +
                    component_name);
    }

    auto slice = data_.eye_masks_store |
                 ts::Dims(0).HalfOpenInterval(
                     static_cast<ts::Index>(roi_index),
                     static_cast<ts::Index>(roi_index + 1)) |
                 ts::Dims(1).HalfOpenInterval(
                     static_cast<ts::Index>(channel_index),
                     static_cast<ts::Index>(channel_index + 1));
    auto read_result = ts::Read(slice).result();
    if (!read_result.ok()) {
        return fail("Failed to read refined subject-mask component row: " +
                    read_result.status().ToString());
    }

    auto array = read_result.value();
    auto shape = array.shape();
    auto byte_strides = array.byte_strides();
    if (shape.size() != 4 || byte_strides.size() != 4 ||
        shape[0] != 1 || shape[1] != 1) {
        return fail("Unexpected refined subject-mask component row shape.");
    }

    const size_t rows = static_cast<size_t>(shape[2]);
    const size_t cols = static_cast<size_t>(shape[3]);
    if (rows == 0 || cols == 0) {
        return fail("Selected refined subject-mask component row is empty.");
    }

    std::vector<uint8_t> mask(rows * cols, 0);
    const uint8_t* origin =
        static_cast<const uint8_t*>(array.byte_strided_origin_pointer());
    const ts::Index stride_row = byte_strides[2];
    const ts::Index stride_col = byte_strides[3];
    for (size_t row = 0; row < rows; ++row) {
        const uint8_t* row_ptr =
            origin + stride_row * static_cast<ts::Index>(row);
        for (size_t col = 0; col < cols; ++col) {
            const uint8_t* value_ptr =
                row_ptr + stride_col * static_cast<ts::Index>(col);
            mask[row * cols + col] = *value_ptr != 0 ? 1 : 0;
        }
    }

    out_row.valid = true;
    out_row.run_name = data_.eye_masks_run_name;
    out_row.component_name = component_it->label;
    out_row.channel_index = channel_index;
    out_row.roi_index = roi_index;
    out_row.rows = rows;
    out_row.cols = cols;
    out_row.mask = std::move(mask);
    return true;
}

bool ZarrDetectionLoader::populateEyeMaskEntry(
    size_t roi_index, FrameDetections::EyeMask& out_mask) const {
    if (!data_.eye_masks_loaded || roi_index >= data_.eye_mask_roi_count) {
        return false;
    }
    size_t chunk_rows =
        data_.eye_mask_chunk_rows > 0 ? data_.eye_mask_chunk_rows : 512;
    size_t chunk_id = roi_index / chunk_rows;
    if (!ensureEyeMaskChunk(chunk_id)) {
        return false;
    }
    out_mask.rows = static_cast<int>(data_.eye_mask_height);
    out_mask.cols = static_cast<int>(data_.eye_mask_width);
    out_mask.valid = false;
    out_mask.has_feret_axes = false;
    const float angle_nan = std::numeric_limits<float>::quiet_NaN();
    out_mask.feret_minor_angle_deg[0] = angle_nan;
    out_mask.feret_minor_angle_deg[1] = angle_nan;
    out_mask.feret_angle_valid = {0, 0};
    out_mask.has_eye_angles = false;
    out_mask.subject_mask_components.clear();
    out_mask.has_subject_mask_components = false;
    out_mask.roi_index = static_cast<int32_t>(roi_index);

    std::lock_guard<std::mutex> cache_lock(*data_.mask_chunk_cache_mutex);
    const auto* entry = findEyeMaskChunk(chunk_id);
    if (entry == nullptr) {
        return false;
    }
    size_t local_index = roi_index - entry->chunk_start;
    if (local_index >= entry->pixel_indices.size()) {
        return false;
    }

    if (local_index < entry->component_pixel_indices.size()) {
        const auto& component_pixels =
            entry->component_pixel_indices[local_index];
        const size_t component_count = std::min(
            component_pixels.size(),
            data_.refined_subject_mask_overlay_components.size());
        out_mask.subject_mask_components.reserve(component_count);
        for (size_t component = 0; component < component_count; ++component) {
            FrameDetections::EyeMask::SubjectMaskComponent component_entry;
            component_entry.label =
                data_.refined_subject_mask_overlay_components[component].label;
            component_entry.channel_index =
                data_.refined_subject_mask_overlay_components[component]
                    .channel_index;
            component_entry.pixel_indices = component_pixels[component];
            component_entry.valid = !component_entry.pixel_indices.empty();
            if (local_index < entry->component_contours_xy.size() &&
                component < entry->component_contours_xy[local_index].size()) {
                component_entry.contour_xy =
                    entry->component_contours_xy[local_index][component];
                component_entry.has_contour =
                    component_entry.contour_xy.size() > 1;
            }
            if (component_entry.valid) {
                out_mask.valid = true;
                out_mask.has_subject_mask_components = true;
            }
            out_mask.subject_mask_components.push_back(
                std::move(component_entry));
        }
    }

    for (size_t channel = 0; channel < 2; ++channel) {
        out_mask.pixel_indices[channel] =
            entry->pixel_indices[local_index][channel];
        if (!out_mask.pixel_indices[channel].empty()) {
            out_mask.valid = true;
        }

        if (roi_index < data_.eye_mask_feret_axes_major.size()) {
            const auto& axis_vals =
                data_.eye_mask_feret_axes_major[roi_index][channel];
            bool axis_valid = true;
            for (float value : axis_vals) {
                if (!std::isfinite(value)) {
                    axis_valid = false;
                    break;
                }
            }
            if (axis_valid) {
                float dx = axis_vals[0] - axis_vals[2];
                float dy = axis_vals[1] - axis_vals[3];
                if (std::fabs(dx) > 1e-5f || std::fabs(dy) > 1e-5f) {
                    auto& segment = out_mask.feret_major[channel];
                    segment.valid = true;
                    segment.x0 = axis_vals[0];
                    segment.y0 = axis_vals[1];
                    segment.x1 = axis_vals[2];
                    segment.y1 = axis_vals[3];
                    out_mask.has_feret_axes = true;
                }
            }
        }
        if (roi_index < data_.eye_mask_feret_axes_minor.size()) {
            const auto& axis_vals =
                data_.eye_mask_feret_axes_minor[roi_index][channel];
            bool axis_valid = true;
            for (float value : axis_vals) {
                if (!std::isfinite(value)) {
                    axis_valid = false;
                    break;
                }
            }
            if (axis_valid) {
                float dx = axis_vals[0] - axis_vals[2];
                float dy = axis_vals[1] - axis_vals[3];
                if (std::fabs(dx) > 1e-5f || std::fabs(dy) > 1e-5f) {
                    auto& segment = out_mask.feret_minor[channel];
                    segment.valid = true;
                    segment.x0 = axis_vals[0];
                    segment.y0 = axis_vals[1];
                    segment.x1 = axis_vals[2];
                    segment.y1 = axis_vals[3];
                    out_mask.has_feret_axes = true;
                }
            }
        }
    }
    return out_mask.valid;
}
