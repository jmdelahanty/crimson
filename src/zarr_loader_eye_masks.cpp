#include "zarr_loader_internal.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

void ZarrDetectionLoader::clearEyeMaskData() {
    data_.mask_chunk_cache.clear();
    data_.eye_masks_run_name.clear();
    data_.eye_masks_loaded = false;
    data_.has_eye_masks = false;
    data_.eye_masks_store = ts::TensorStore<uint8_t, 4>();
    data_.eye_mask_roi_count = 0;
    data_.eye_mask_height = 0;
    data_.eye_mask_width = 0;
    data_.eye_mask_chunk_rows = 0;
    data_.eye_mask_frame_indices.clear();
    data_.eye_mask_detection_indices.clear();
    data_.eye_masks_source_crop_run.clear();
    data_.eye_mask_roi_index_by_detection.clear();
    data_.eye_mask_offset_x.clear();
    data_.eye_mask_offset_y.clear();
    data_.eye_mask_roi_width_px.clear();
    data_.eye_mask_roi_height_px.clear();
    data_.eye_mask_ellipse_params.clear();
    data_.eye_mask_ellipse_success.clear();
    data_.eye_mask_feret_axes_major.clear();
    data_.eye_mask_feret_axes_minor.clear();
    data_.eye_masks_have_ellipse_fits = false;
    data_.eye_masks_have_feret_axes = false;
}

bool ZarrDetectionLoader::loadEyeMaskData(const ts::kvstore::KvStore& store) {
    clearEyeMaskData();

    if (data_.layout != ZarrLayoutType::kPaletteRuns) {
        return false;
    }

    const size_t expected_roi_count = data_.keypoint_roi_frame_indices.size();
    return loadRefinedEyeMaskData(store, expected_roi_count);
}

bool ZarrDetectionLoader::loadRefinedEyeMaskData(const ts::kvstore::KvStore& store,
                                                 size_t expected_roi_count) {
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

    const std::string run_base = "refined_eye_masks_runs/" + latest_run + "/";

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

    const size_t roi_dim = static_cast<size_t>(shape[0]);
    const size_t channel_dim = static_cast<size_t>(shape[1]);
    const size_t mask_rows = static_cast<size_t>(shape[2]);
    const size_t mask_cols = static_cast<size_t>(shape[3]);

    if (roi_dim == 0 || channel_dim == 0 || mask_rows == 0 || mask_cols == 0) {
        std::cout << "[EYE_MASK_WARNING] masks_roi dataset '" << latest_run
                  << "' has empty dimensions; skipping." << std::endl;
        return false;
    }

    data_.eye_masks_store = masks_store_result.value();
    data_.eye_masks_run_name = latest_run;
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
                data_.eye_mask_chunk_rows = static_cast<size_t>(chunk_size);
            }
        }
    }
    if (data_.eye_mask_chunk_rows == 0) {
        data_.eye_mask_chunk_rows = std::min<size_t>(roi_dim, 512);
    }

    std::vector<int32_t> mask_frame_indices;
    if (readInt32Array(store, run_base + "frame_indices", mask_frame_indices)) {
        data_.eye_mask_frame_indices = mask_frame_indices;
        if (mask_frame_indices.size() != roi_dim) {
            std::cout << "[EYE_MASK_WARNING] refined_eye_masks run '" << latest_run
                      << "' frame_indices size (" << mask_frame_indices.size()
                      << ") does not match masks_roi rows (" << roi_dim << ")." << std::endl;
        }
        if (expected_roi_count > 0 && mask_frame_indices.size() != expected_roi_count) {
            std::cout << "[EYE_MASK_WARNING] refined_eye_masks run '" << latest_run
                      << "' frame_indices size (" << mask_frame_indices.size()
                      << ") does not match keypoint ROI count (" << expected_roi_count
                      << ")." << std::endl;
        }
    } else {
        std::cout << "[EYE_MASK_WARNING] refined_eye_masks run '" << latest_run
                  << "' missing frame_indices; overlay alignment will rely on other ROI metadata."
                  << std::endl;
    }

    std::vector<int32_t> mask_detection_indices;
    if (readInt32Array(store, run_base + "detection_indices", mask_detection_indices)) {
        data_.eye_mask_detection_indices = mask_detection_indices;
        if (mask_detection_indices.size() != roi_dim) {
            std::cout << "[EYE_MASK_WARNING] refined_eye_masks run '" << latest_run
                      << "' detection_indices size (" << mask_detection_indices.size()
                      << ") does not match masks_roi rows (" << roi_dim << ")." << std::endl;
        }
    }

    if (expected_roi_count > 0 && roi_dim != expected_roi_count) {
        std::cout << "[EYE_MASK_WARNING] refined_eye_masks run '" << latest_run
                  << "' masks_roi row count (" << roi_dim
                  << ") does not match keypoint ROI count (" << expected_roi_count << ")."
                  << std::endl;
    }

    const float nan_value = std::numeric_limits<float>::quiet_NaN();
    const size_t total_detections = data_.bbox_norm_coords.size();
    data_.eye_mask_roi_index_by_detection.assign(total_detections, -1);
    data_.eye_mask_offset_x.assign(total_detections, nan_value);
    data_.eye_mask_offset_y.assign(total_detections, nan_value);
    data_.eye_mask_roi_width_px.assign(total_detections, 0.0f);
    data_.eye_mask_roi_height_px.assign(total_detections, 0.0f);

    auto run_attrs = readAttrsAny(store, run_base);
    std::vector<std::string> crop_candidates;
    auto addCropCandidate = [&](const std::string& candidate) {
        if (candidate.empty()) {
            return;
        }
        std::string normalized = NormalizeCropRunName(candidate);
        if (std::find(crop_candidates.begin(), crop_candidates.end(), normalized) ==
            crop_candidates.end()) {
            crop_candidates.push_back(normalized);
        }
    };

    if (run_attrs.has_value()) {
        addCropCandidate(ExtractCropRunFromObject(*run_attrs));
        if (run_attrs->contains("inputs") && (*run_attrs)["inputs"].is_object()) {
            addCropCandidate(ExtractCropRunFromObject((*run_attrs)["inputs"]));
        }
    }

    if (auto crop_group_attrs = readAttrsAny(store, "crop_runs")) {
        addCropCandidate(extractLatestRunName(*crop_group_attrs));
    }

    if (!root_path_.empty()) {
        auto fs_candidates = collect_runs_fs(
            root_path_,
            "crop_runs",
            {"roi_coordinates_full"});
        for (const auto& name : fs_candidates) {
            addCropCandidate(name);
        }
    }

    std::vector<float> roi_offsets;
    std::string crop_run;
    float roi_height_px = 0.0f;
    float roi_width_px = 0.0f;
    bool roi_offsets_loaded = false;
    bool roi_size_available = false;

    auto updateRoiSizeFromAttrs = [&](const std::string& crop_base) {
        if (roi_size_available) {
            return;
        }
        if (auto crop_attrs = readAttrsAny(store, crop_base)) {
            const auto& attrs = *crop_attrs;
            if (attrs.contains("roi_size") && attrs["roi_size"].is_array()) {
                const auto& roi_size_attr = attrs["roi_size"];
                if (roi_size_attr.size() >= 2 &&
                    roi_size_attr[0].is_number() &&
                    roi_size_attr[1].is_number()) {
                    roi_height_px = static_cast<float>(roi_size_attr[0].get<double>());
                    roi_width_px = static_cast<float>(roi_size_attr[1].get<double>());
                    if (roi_height_px > 0.0f && roi_width_px > 0.0f) {
                        roi_size_available = true;
                    }
                }
            }
        }
        if (!roi_size_available) {
            auto roi_image_store =
                openArrayAny<uint8_t, 3>(store, crop_base + "roi_images", context_);
            if (roi_image_store.ok()) {
                auto roi_shape = roi_image_store.value().domain().shape();
                if (roi_shape.size() == 3) {
                    float inferred_height = static_cast<float>(roi_shape[1]);
                    float inferred_width = static_cast<float>(roi_shape[2]);
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

        auto loadTypedOffsets = [&](auto type_tag) -> bool {
            using T = decltype(type_tag);
            auto roi_store =
                openArrayAny<T, 2>(store, crop_base + "roi_coordinates_full", context_);
            if (!roi_store.ok()) {
                return false;
            }
            auto roi_array_result = ts::Read(roi_store.value()).result();
            if (!roi_array_result.ok()) {
                return false;
            }
            auto roi_array = roi_array_result.value();
            auto roi_shape = roi_array.shape();
            if (roi_shape.size() != 2 ||
                roi_shape[0] != static_cast<tensorstore::Index>(roi_dim) ||
                roi_shape[1] < 2) {
                return false;
            }

            const size_t cols = static_cast<size_t>(roi_shape[1]);
            roi_offsets.assign(roi_dim * 2, nan_value);
            const T* roi_ptr = static_cast<const T*>(roi_array.data());
            for (size_t i = 0; i < roi_dim; ++i) {
                roi_offsets[i * 2 + 0] = static_cast<float>(roi_ptr[i * cols + 0]);
                roi_offsets[i * 2 + 1] = static_cast<float>(roi_ptr[i * cols + 1]);
            }
            updateRoiSizeFromAttrs(crop_base);
            return true;
        };

        if (loadTypedOffsets(float{})) {
            return true;
        }
        return loadTypedOffsets(int32_t{});
    };

    for (const auto& candidate : crop_candidates) {
        if (tryLoadCropRun(candidate)) {
            crop_run = candidate;
            roi_offsets_loaded = true;
            break;
        }
    }

    data_.eye_masks_source_crop_run = crop_run;
    if (!crop_run.empty() && !data_.keypoints_source_crop_run.empty() &&
        crop_run != data_.keypoints_source_crop_run) {
        std::cout << "  Refined eye masks use crop run '" << crop_run
                  << "' (keypoints use '" << data_.keypoints_source_crop_run << "')"
                  << std::endl;
    }

    auto assignDetectionPlacement = [&](size_t roi_index, int32_t det_index) {
        if (det_index < 0 || static_cast<size_t>(det_index) >= total_detections) {
            return;
        }
        if (data_.eye_mask_roi_index_by_detection[det_index] >= 0) {
            return;
        }

        data_.eye_mask_roi_index_by_detection[det_index] = static_cast<int32_t>(roi_index);

        std::array<float, 4> pixel_box = {nan_value, nan_value, nan_value, nan_value};
        bool have_pixel_box = false;
        if (static_cast<size_t>(det_index) < data_.bbox_norm_coords.size() &&
            data_.image_width > 0 && data_.image_height > 0) {
            pixel_box = normalizedBoxToPixels(
                data_.bbox_norm_coords[det_index],
                data_.image_width,
                data_.image_height);
            have_pixel_box = true;
        }

        float assigned_offset_x = nan_value;
        float assigned_offset_y = nan_value;
        if (roi_offsets_loaded && roi_offsets.size() >= (roi_index * 2 + 2)) {
            assigned_offset_x = roi_offsets[roi_index * 2 + 0];
            assigned_offset_y = roi_offsets[roi_index * 2 + 1];
        } else if (have_pixel_box) {
            assigned_offset_x = pixel_box[0];
            assigned_offset_y = pixel_box[1];
        }
        if (std::isfinite(assigned_offset_x) && std::isfinite(assigned_offset_y)) {
            data_.eye_mask_offset_x[det_index] = assigned_offset_x;
            data_.eye_mask_offset_y[det_index] = assigned_offset_y;
        }

        float assigned_width = 0.0f;
        float assigned_height = 0.0f;
        if (roi_size_available) {
            assigned_width = roi_width_px;
            assigned_height = roi_height_px;
        } else if (have_pixel_box) {
            assigned_width = std::max(0.0f, pixel_box[2] - pixel_box[0]);
            assigned_height = std::max(0.0f, pixel_box[3] - pixel_box[1]);
        }
        if (assigned_width > 0.0f && assigned_height > 0.0f) {
            data_.eye_mask_roi_width_px[det_index] = assigned_width;
            data_.eye_mask_roi_height_px[det_index] = assigned_height;
        }
    };

    const bool has_detection_index_map = mask_detection_indices.size() == roi_dim;
    if (has_detection_index_map) {
        for (size_t roi_index = 0; roi_index < roi_dim; ++roi_index) {
            assignDetectionPlacement(roi_index, mask_detection_indices[roi_index]);
        }
    } else if (mask_detection_indices.size() != 0) {
        std::cout << "[EYE_MASK_WARNING] refined_eye_masks run '" << latest_run
                  << "' detection_indices size (" << mask_detection_indices.size()
                  << ") does not match masks_roi rows; falling back to frame-order alignment."
                  << std::endl;
    }

    if (!has_detection_index_map &&
        data_.frame_offsets.size() >= 2 &&
        mask_frame_indices.size() == roi_dim) {
        std::vector<size_t> frame_cursor(data_.frame_offsets.size() - 1, 0);
        for (size_t roi_index = 0; roi_index < roi_dim; ++roi_index) {
            int32_t frame = mask_frame_indices[roi_index];
            if (frame < 0 || static_cast<size_t>(frame + 1) >= data_.frame_offsets.size()) {
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
            assignDetectionPlacement(roi_index, static_cast<int32_t>(start + offset));
            frame_cursor[frame]++;
        }
    }

    auto loadEllipseParams =
        [&](const std::string& dataset_name,
            std::vector<std::array<std::array<float, 5>, 2>>& target) -> bool {
            target.clear();
            auto params_store =
                openArrayAny<float, 3>(store, run_base + dataset_name, context_);
            if (!params_store.ok()) {
                return false;
            }
            auto params_result = ts::Read(params_store.value()).result();
            if (!params_result.ok()) {
                std::cout << "[EYE_MASK_WARNING] Failed to read " << dataset_name
                          << " for run '" << latest_run << "': "
                          << params_result.status().ToString() << std::endl;
                return false;
            }

            auto params_array = params_result.value();
            auto params_shape = params_array.shape();
            if (params_shape.size() != 3 ||
                params_shape[0] != static_cast<ts::Index>(roi_dim) ||
                params_shape[1] < 1 || params_shape[2] < 5) {
                std::cout << "[EYE_MASK_WARNING] Unexpected shape for " << dataset_name
                          << " in run '" << latest_run << "' (expected "
                          << roi_dim << "x2x5, got ";
                for (size_t i = 0; i < params_shape.size(); ++i) {
                    std::cout << params_shape[i] << (i + 1 < params_shape.size() ? "x" : "");
                }
                std::cout << ")." << std::endl;
                return false;
            }

            target.resize(roi_dim);
            for (auto& roi_entry : target) {
                roi_entry = {
                    std::array<float, 5>{nan_value, nan_value, nan_value, nan_value, nan_value},
                    std::array<float, 5>{nan_value, nan_value, nan_value, nan_value, nan_value},
                };
            }

            const size_t eye_dim = std::min<size_t>(2, static_cast<size_t>(params_shape[1]));
            for (size_t roi = 0; roi < roi_dim; ++roi) {
                for (size_t eye = 0; eye < eye_dim; ++eye) {
                    std::array<float, 5> values = {
                        nan_value, nan_value, nan_value, nan_value, nan_value};
                    for (size_t idx = 0; idx < 5; ++idx) {
                        values[idx] = params_array(static_cast<ts::Index>(roi),
                                                   static_cast<ts::Index>(eye),
                                                   static_cast<ts::Index>(idx));
                    }
                    target[roi][eye] = values;
                }
            }
            return true;
        };

    auto loadEllipseSuccess =
        [&](const std::string& dataset_name,
            std::vector<std::array<uint8_t, 2>>& target) -> bool {
            target.clear();

            auto parseArray = [&](const auto& success_array) -> bool {
                auto success_shape = success_array.shape();
                if (success_shape.size() != 2 ||
                    success_shape[0] != static_cast<ts::Index>(roi_dim) ||
                    success_shape[1] < 1) {
                    std::cout << "[EYE_MASK_WARNING] Unexpected shape for " << dataset_name
                              << " in run '" << latest_run << "' (expected "
                              << roi_dim << "x2, got ";
                    for (size_t i = 0; i < success_shape.size(); ++i) {
                        std::cout << success_shape[i]
                                  << (i + 1 < success_shape.size() ? "x" : "");
                    }
                    std::cout << ")." << std::endl;
                    return false;
                }

                target.assign(roi_dim, std::array<uint8_t, 2>{0, 0});
                const size_t eye_dim =
                    std::min<size_t>(2, static_cast<size_t>(success_shape[1]));
                for (size_t roi = 0; roi < roi_dim; ++roi) {
                    for (size_t eye = 0; eye < eye_dim; ++eye) {
                        target[roi][eye] =
                            success_array(static_cast<ts::Index>(roi),
                                          static_cast<ts::Index>(eye))
                                ? 1
                                : 0;
                    }
                }
                return true;
            };

            auto success_store_bool =
                openArrayAny<bool, 2>(store, run_base + dataset_name, context_);
            if (success_store_bool.ok()) {
                auto success_result = ts::Read(success_store_bool.value()).result();
                if (!success_result.ok()) {
                    std::cout << "[EYE_MASK_WARNING] Failed to read " << dataset_name
                              << " for run '" << latest_run << "': "
                              << success_result.status().ToString() << std::endl;
                    return false;
                }
                return parseArray(success_result.value());
            }

            auto success_store_u8 =
                openArrayAny<uint8_t, 2>(store, run_base + dataset_name, context_);
            if (!success_store_u8.ok()) {
                return false;
            }
            auto success_result = ts::Read(success_store_u8.value()).result();
            if (!success_result.ok()) {
                std::cout << "[EYE_MASK_WARNING] Failed to read " << dataset_name
                          << " for run '" << latest_run << "': "
                          << success_result.status().ToString() << std::endl;
                return false;
            }
            return parseArray(success_result.value());
        };

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
                    float value = axes_array(static_cast<ts::Index>(roi),
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
                const float dx = values[0] - values[2];
                const float dy = values[1] - values[3];
                if (std::fabs(dx) < 1e-5f && std::fabs(dy) < 1e-5f) {
                    continue;
                }
                target[roi][eye] = values;
            }
        }
        return true;
    };

    const bool ellipse_params_ok =
        loadEllipseParams("ellipse_params", data_.eye_mask_ellipse_params);
    const bool ellipse_success_ok =
        loadEllipseSuccess("ellipse_success", data_.eye_mask_ellipse_success);
    data_.eye_masks_have_ellipse_fits = ellipse_params_ok && ellipse_success_ok;

    const bool feret_major_ok =
        loadFeretAxes("feret_axes_major", data_.eye_mask_feret_axes_major);
    const bool feret_minor_ok =
        loadFeretAxes("feret_axes_minor", data_.eye_mask_feret_axes_minor);
    data_.eye_masks_have_feret_axes = feret_major_ok && feret_minor_ok;

    std::cout << "  Refined eye mask run '" << latest_run
              << "' loaded (" << channel_dim << " channels, "
              << mask_cols << "x" << mask_rows << " masks)" << std::endl;
    return true;
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

    auto& cache = data_.mask_chunk_cache;
    auto it = std::find_if(cache.begin(), cache.end(),
                           [&](const ZarrDetectionData::EyeMaskChunkCacheEntry& entry) {
                               return entry.chunk_id == chunk_id;
                           });
    if (it != cache.end()) {
        if (std::next(it) != cache.end()) {
            ZarrDetectionData::EyeMaskChunkCacheEntry entry = std::move(*it);
            cache.erase(it);
            cache.push_back(std::move(entry));
        }
        return true;
    }

    const size_t chunk_rows =
        data_.eye_mask_chunk_rows > 0 ? data_.eye_mask_chunk_rows : 512;
    const size_t chunk_start = chunk_id * chunk_rows;
    if (chunk_start >= data_.eye_mask_roi_count) {
        return false;
    }
    const size_t chunk_end =
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

    const size_t chunk_len = static_cast<size_t>(shape[0]);
    const size_t channels = static_cast<size_t>(shape[1]);
    const size_t rows = static_cast<size_t>(shape[2]);
    const size_t cols = static_cast<size_t>(shape[3]);

    ZarrDetectionData::EyeMaskChunkCacheEntry entry;
    entry.chunk_id = chunk_id;
    entry.chunk_start = chunk_start;
    entry.chunk_length = chunk_len;
    entry.pixel_indices.resize(chunk_len);

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

    for (size_t roi = 0; roi < chunk_len; ++roi) {
        const auto roi_offset =
            stride_roi * static_cast<ts::Index>(roi);
        const uint8_t* roi_ptr = base_ptr + roi_offset;
        for (size_t channel = 0; channel < std::min<size_t>(channels, 2); ++channel) {
            auto& indices_vec = entry.pixel_indices[roi][channel];
            indices_vec.clear();
            indices_vec.reserve(256);

            const auto channel_offset =
                stride_channel * static_cast<ts::Index>(channel);
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
        }
    }

    if (cache.size() >= kEyeMaskChunkCacheCapacity) {
        cache.erase(cache.begin());
    }
    cache.push_back(std::move(entry));

    if (allow_prefetch) {
        prefetchAdjacentEyeMaskChunks(chunk_id);
    }
    return true;
}

void ZarrDetectionLoader::prefetchAdjacentEyeMaskChunks(size_t chunk_id) const {
    const size_t chunk_rows =
        data_.eye_mask_chunk_rows > 0 ? data_.eye_mask_chunk_rows : 512;
    if (chunk_id > 0) {
        ensureEyeMaskChunk(chunk_id - 1, /*allow_prefetch=*/false);
    }
    if ((chunk_id + 1) * chunk_rows < data_.eye_mask_roi_count) {
        ensureEyeMaskChunk(chunk_id + 1, /*allow_prefetch=*/false);
    }
}

bool ZarrDetectionLoader::populateEyeMaskEntry(
    size_t roi_index,
    FrameDetections::EyeMask& out_mask,
    bool include_mask_pixels) const {
    if (!data_.eye_masks_loaded || roi_index >= data_.eye_mask_roi_count) {
        return false;
    }

    const ZarrDetectionData::EyeMaskChunkCacheEntry* entry = nullptr;
    size_t local_index = 0;
    if (include_mask_pixels) {
        const size_t chunk_rows =
            data_.eye_mask_chunk_rows > 0 ? data_.eye_mask_chunk_rows : 512;
        const size_t chunk_id = roi_index / chunk_rows;
        if (ensureEyeMaskChunk(chunk_id)) {
            entry = findEyeMaskChunk(chunk_id);
            if (entry != nullptr) {
                local_index = roi_index - entry->chunk_start;
                if (local_index >= entry->pixel_indices.size()) {
                    entry = nullptr;
                }
            }
        }
    }

    out_mask.rows = static_cast<int>(data_.eye_mask_height);
    out_mask.cols = static_cast<int>(data_.eye_mask_width);
    out_mask.valid = false;
    out_mask.has_fitted_ellipses = false;
    out_mask.has_feret_axes = false;
    const float angle_nan = std::numeric_limits<float>::quiet_NaN();
    out_mask.feret_minor_angle_deg[0] = angle_nan;
    out_mask.feret_minor_angle_deg[1] = angle_nan;
    out_mask.feret_angle_valid = {0, 0};
    out_mask.has_eye_angles = false;
    out_mask.roi_index = static_cast<int32_t>(roi_index);
    for (size_t channel = 0; channel < 2; ++channel) {
        out_mask.pixel_indices[channel].clear();
        if (entry != nullptr) {
            out_mask.pixel_indices[channel] =
                entry->pixel_indices[local_index][channel];
            if (!out_mask.pixel_indices[channel].empty()) {
                out_mask.valid = true;
            }
        }

        if (roi_index < data_.eye_mask_ellipse_params.size() &&
            roi_index < data_.eye_mask_ellipse_success.size() &&
            data_.eye_mask_ellipse_success[roi_index][channel] != 0) {
            const auto& params = data_.eye_mask_ellipse_params[roi_index][channel];
            const bool params_valid =
                std::isfinite(params[0]) && std::isfinite(params[1]) &&
                std::isfinite(params[2]) && std::isfinite(params[3]) &&
                std::isfinite(params[4]) && params[2] > 0.0f && params[3] > 0.0f;
            if (params_valid) {
                auto& ellipse = out_mask.fitted_ellipses[channel];
                ellipse.valid = true;
                ellipse.center_x = params[0];
                ellipse.center_y = params[1];
                ellipse.major_axis = params[2];
                ellipse.minor_axis = params[3];
                ellipse.angle_deg = params[4];
                out_mask.has_fitted_ellipses = true;
                out_mask.valid = true;
            }
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
                const float dx = axis_vals[0] - axis_vals[2];
                const float dy = axis_vals[1] - axis_vals[3];
                if (std::fabs(dx) > 1e-5f || std::fabs(dy) > 1e-5f) {
                    auto& segment = out_mask.feret_major[channel];
                    segment.valid = true;
                    segment.x0 = axis_vals[0];
                    segment.y0 = axis_vals[1];
                    segment.x1 = axis_vals[2];
                    segment.y1 = axis_vals[3];
                    out_mask.has_feret_axes = true;
                    out_mask.valid = true;
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
                const float dx = axis_vals[0] - axis_vals[2];
                const float dy = axis_vals[1] - axis_vals[3];
                if (std::fabs(dx) > 1e-5f || std::fabs(dy) > 1e-5f) {
                    auto& segment = out_mask.feret_minor[channel];
                    segment.valid = true;
                    segment.x0 = axis_vals[0];
                    segment.y0 = axis_vals[1];
                    segment.x1 = axis_vals[2];
                    segment.y1 = axis_vals[3];
                    out_mask.has_feret_axes = true;
                    out_mask.valid = true;
                }
            }
        }
    }
    return out_mask.valid;
}
