#include "zarr_loader_internal.h"
#include <iostream>

using json = nlohmann::json;

std::optional<json> ZarrDetectionLoader::readGroupAttrs(
    const ts::kvstore::KvStore& store,
    const std::string& path) const {
    if (auto attrs = readAttrsAny(store, path)) {
        return attrs;
    }
    if (root_path_.empty()) {
        return std::nullopt;
    }

    namespace fs = std::filesystem;
    fs::path json_path = fs::path(root_path_);
    if (!path.empty()) {
        fs::path sub(path);
        json_path /= sub;
    }
    if (fs::is_directory(json_path)) {
        json_path /= "zarr.json";
    } else if (json_path.filename() != "zarr.json") {
        json_path /= "zarr.json";
    }

    if (!fs::exists(json_path)) {
        return std::nullopt;
    }

    try {
        std::ifstream in(json_path);
        if (!in) {
            return std::nullopt;
        }
        json meta;
        in >> meta;
        if (meta.contains("attributes") && meta["attributes"].is_object()) {
            return meta["attributes"];
        }
        return meta;
    } catch (const std::exception& e) {
        std::cout << "  [AttrProbe] Failed to parse " << json_path << ": "
                  << e.what() << std::endl;
        return std::nullopt;
    }
}

void ZarrDetectionLoader::cacheDetectionStage(InterpolationRunData stage,
                                              DetectionDataset dataset_type) {
    switch (dataset_type) {
        case DetectionDataset::RawDetect:
            data_.raw_detection_dataset = std::move(stage);
            data_.has_raw_detection_dataset = true;
            break;
        case DetectionDataset::RefinedManual:
            data_.refined_manual_dataset = std::move(stage);
            data_.has_refined_manual_dataset = true;
            break;
        case DetectionDataset::RefinedFiltered:
            data_.refined_filtered_dataset = std::move(stage);
            data_.has_refined_filtered_dataset = true;
            break;
        case DetectionDataset::RefinedInterpolated:
            data_.refined_interpolated_dataset = std::move(stage);
            data_.has_refined_interpolated_dataset = true;
            break;
        case DetectionDataset::RefinedRoot:
            data_.refined_root_dataset = std::move(stage);
            data_.has_refined_root_dataset = true;
            break;
    }
}

void ZarrDetectionLoader::computeDetectionsFromOffsets(
    const std::vector<size_t>& offsets,
    std::vector<int32_t>& n_detections_out) const {
    size_t frame_count = offsets.size() > 0 ? offsets.size() - 1 : 0;
    n_detections_out.assign(frame_count, 0);
    for (size_t frame = 0; frame < frame_count; ++frame) {
        size_t start = offsets[frame];
        size_t end = offsets[frame + 1];
        size_t count = (end >= start) ? (end - start) : 0;
        n_detections_out[frame] = static_cast<int32_t>(count);
    }
}

void ZarrDetectionLoader::computeActiveDatasetInterpolationFlags() {
    size_t frame_count = 0;
    if (!data_.frame_offsets.empty()) {
        frame_count = data_.frame_offsets.size() > 0 ? data_.frame_offsets.size() - 1 : 0;
    }
    if (frame_count == 0 && !data_.n_detections.empty()) {
        frame_count = data_.n_detections.size();
    }

    size_t total_detections = 0;
    if (!data_.frame_offsets.empty()) {
        total_detections = data_.frame_offsets.back();
    } else if (!data_.frame_indices.empty()) {
        total_detections = data_.frame_indices.size();
    }

    data_.frame_interpolated_flags.clear();

    if (!data_.detection_source_flags.empty() &&
        total_detections == data_.detection_source_flags.size() &&
        !data_.frame_offsets.empty()) {
        data_.frame_interpolated_flags.assign(frame_count, 0);
        for (size_t frame = 0; frame < frame_count; ++frame) {
            size_t start = data_.frame_offsets[frame];
            size_t end = data_.frame_offsets[frame + 1];
            bool any_interp = false;
            for (size_t idx = start; idx < end && idx < data_.detection_source_flags.size(); ++idx) {
                if (data_.detection_source_flags[idx] != 0) {
                    any_interp = true;
                    break;
                }
            }
            data_.frame_interpolated_flags[frame] = any_interp ? 1 : 0;
        }
    } else {
        data_.frame_interpolated_flags.clear();
    }
}

bool ZarrDetectionLoader::applyDetectionDataset(const InterpolationRunData& stage,
                                                DetectionDataset dataset_type) {
    if (stage.frame_indices.empty() && stage.n_detections.empty() &&
        stage.frame_offsets.empty()) {
        return false;
    }

    data_.frame_indices = stage.frame_indices;
    data_.bbox_norm_coords = stage.bbox_norm_coords;
    data_.flat_scores = stage.flat_scores;
    data_.flat_class_ids = stage.flat_class_ids;
    data_.frame_offsets = stage.frame_offsets;
    data_.detection_source_flags = stage.detection_source;
    data_.detection_reason_flags = stage.detection_reason;
    data_.has_scores = stage.has_scores;
    data_.has_class_ids = stage.has_class_ids;
    data_.coordinates_normalized = stage.uses_palette_layout;
    data_.layout = ZarrLayoutType::kPaletteRuns;

    data_.n_detections = stage.n_detections;
    if (data_.n_detections.empty()) {
        computeDetectionsFromOffsets(data_.frame_offsets, data_.n_detections);
    }

    if (data_.frame_offsets.empty() && !data_.n_detections.empty()) {
        data_.frame_offsets.assign(data_.n_detections.size() + 1, 0);
        size_t running = 0;
        for (size_t frame = 0; frame < data_.n_detections.size(); ++frame) {
            running += static_cast<size_t>(std::max(data_.n_detections[frame], 0));
            data_.frame_offsets[frame + 1] = running;
        }
    }

    if (data_.frame_offsets.empty() && !data_.frame_indices.empty()) {
        int32_t max_index = -1;
        for (int32_t idx : data_.frame_indices) {
            max_index = std::max(max_index, idx);
        }
        size_t count = max_index >= 0 ? static_cast<size_t>(max_index + 1) : 0;
        data_.frame_offsets.assign(count + 1, 0);
        std::vector<int32_t> counts(count, 0);
        for (int32_t idx : data_.frame_indices) {
            if (idx >= 0 && static_cast<size_t>(idx) < counts.size()) {
                counts[idx]++;
            }
        }
        size_t running = 0;
        for (size_t i = 0; i < counts.size(); ++i) {
            running += static_cast<size_t>(counts[i]);
            data_.frame_offsets[i + 1] = running;
        }
        if (data_.n_detections.empty()) {
            data_.n_detections = counts;
        }
    }

    if (data_.n_detections.empty()) {
        computeDetectionsFromOffsets(data_.frame_offsets, data_.n_detections);
    }

    data_.total_frames = data_.n_detections.size();
    if (data_.total_frames == 0 && data_.frame_offsets.size() > 1) {
        data_.total_frames = data_.frame_offsets.size() - 1;
    }

    data_.max_detections = 0;
    if (!data_.frame_offsets.empty()) {
        for (size_t frame = 0; frame + 1 < data_.frame_offsets.size(); ++frame) {
            size_t count = data_.frame_offsets[frame + 1] - data_.frame_offsets[frame];
            data_.max_detections = std::max(data_.max_detections, count);
        }
    }
    for (const auto count : data_.n_detections) {
        if (count >= 0) {
            data_.max_detections = std::max(
                data_.max_detections,
                static_cast<size_t>(count));
        }
    }

    data_.detect_run_name = stage.run_name;
    if (!stage.stage_label.empty()) {
        if (!data_.detect_run_name.empty()) {
            data_.detect_run_name += "/";
        }
        data_.detect_run_name += stage.stage_label;
    }
    data_.detect_run_method = stage.method.empty() ? stage.stage_label : stage.method;
    data_.detect_run_created_at = stage.created_at;
    data_.detect_run_source = stage.source_detection_run;
    data_.detect_run_provenance_json = stage.provenance_json;

    computeActiveDatasetInterpolationFlags();
    active_dataset_ = dataset_type;
    return true;
}

std::vector<std::pair<ZarrDetectionLoader::DetectionDataset, std::string>>
ZarrDetectionLoader::getAvailableDetectionDatasets() const {
    std::vector<std::pair<DetectionDataset, std::string>> result;
    auto make_label = [](const InterpolationRunData& stage,
                         const std::string& fallback) -> std::string {
        if (!stage.run_name.empty()) {
            if (!stage.stage_label.empty()) {
                return stage.run_name + "/" + stage.stage_label;
            }
            return stage.run_name;
        }
        return fallback;
    };

    if (data_.has_raw_detection_dataset) {
        std::string label = make_label(data_.raw_detection_dataset, "Detect run");
        result.emplace_back(DetectionDataset::RawDetect, std::move(label));
    }
    if (data_.has_refined_manual_dataset) {
        std::string label = make_label(data_.refined_manual_dataset, "Refined manual");
        result.emplace_back(DetectionDataset::RefinedManual, std::move(label));
    }
    if (data_.has_refined_filtered_dataset) {
        std::string label = make_label(data_.refined_filtered_dataset, "Refined filtered");
        result.emplace_back(DetectionDataset::RefinedFiltered, std::move(label));
    }
    if (data_.has_refined_interpolated_dataset) {
        std::string label = make_label(data_.refined_interpolated_dataset, "Refined interpolated");
        result.emplace_back(DetectionDataset::RefinedInterpolated, std::move(label));
    }
    if (data_.has_refined_root_dataset) {
        std::string label = make_label(data_.refined_root_dataset, "Refined (root)");
        result.emplace_back(DetectionDataset::RefinedRoot, std::move(label));
    }
    return result;
}

bool ZarrDetectionLoader::isDatasetAvailable(DetectionDataset dataset) const {
    switch (dataset) {
        case DetectionDataset::RawDetect:
            return data_.has_raw_detection_dataset;
        case DetectionDataset::RefinedManual:
            return data_.has_refined_manual_dataset;
        case DetectionDataset::RefinedFiltered:
            return data_.has_refined_filtered_dataset;
        case DetectionDataset::RefinedInterpolated:
            return data_.has_refined_interpolated_dataset;
        case DetectionDataset::RefinedRoot:
            return data_.has_refined_root_dataset;
    }
    return false;
}

bool ZarrDetectionLoader::setActiveDetectionDataset(DetectionDataset dataset) {
    if (dataset == active_dataset_) {
        return true;
    }
    if (!isDatasetAvailable(dataset)) {
        return false;
    }

    switch (dataset) {
        case DetectionDataset::RawDetect:
            return applyDetectionDataset(data_.raw_detection_dataset, dataset);
        case DetectionDataset::RefinedManual:
            return applyDetectionDataset(data_.refined_manual_dataset, dataset);
        case DetectionDataset::RefinedFiltered:
            return applyDetectionDataset(data_.refined_filtered_dataset, dataset);
        case DetectionDataset::RefinedInterpolated:
            return applyDetectionDataset(data_.refined_interpolated_dataset, dataset);
        case DetectionDataset::RefinedRoot:
            return applyDetectionDataset(data_.refined_root_dataset, dataset);
    }
    return false;
}

bool ZarrDetectionLoader::activeDatasetHasSyntheticDetections() const {
    return std::any_of(data_.detection_source_flags.begin(),
                       data_.detection_source_flags.end(),
                       [](uint8_t value) { return value != 0; });
}

bool ZarrDetectionLoader::loadStandardFormat(const ts::kvstore::KvStore& store) {
    try {
        // Load metadata first
        if (!loadMetadata(store)) {
            std::cerr << "Warning: Could not load metadata from zarr.json/.zattrs" << std::endl;
        }

        data_.layout = ZarrLayoutType::kLegacyGrid;
        data_.coordinates_normalized = false;
        data_.detect_run_name.clear();
        data_.frame_indices.clear();
        data_.frame_offsets.clear();
        data_.flat_scores.clear();
        data_.flat_class_ids.clear();
        data_.has_scores = false;
        data_.has_class_ids = false;

        // Load bounding boxes first to get dimensions (required)
        if (!loadBoundingBoxes(store, "bboxes")) {
            std::cerr << "Error: Could not load bboxes array" << std::endl;
            return false;
        }

        // Try to load n_detections array
        bool has_n_detections = false;

        if (arrayExists(store, "n_detections")) {
            if (loadNDetections(store, "n_detections")) {
                has_n_detections = true;
                std::cout << "  Loaded n_detections array" << std::endl;
            } else {
                std::cerr << "Warning: n_detections exists but failed to load" << std::endl;
            }
        }
        
        if (!has_n_detections) {
            std::cout << "  No n_detections array found, initializing..." << std::endl;
            
            data_.n_detections.clear();
            data_.n_detections.resize(data_.total_frames, 0);
            
            for (size_t frame_id = 0; frame_id < std::min(size_t(10), data_.total_frames); ++frame_id) {
                try {
                    auto bbox_slice = data_.bboxes_store | 
                        ts::Dims(0).IndexSlice(static_cast<tensorstore::Index>(frame_id));
                    auto bbox_result = ts::Read(bbox_slice).result();
                    
                    if (bbox_result.ok()) {
                        auto bbox_array = bbox_result.value();
                        auto* bbox_data = static_cast<const float*>(bbox_array.data());
                        
                        bool has_valid_detection = true;
                        for (int i = 0; i < 4; ++i) {
                            if (std::abs(bbox_data[i] + 1.0f) < 1e-6f) {
                                has_valid_detection = false;
                                break;
                            }
                        }
                        
                        if (has_valid_detection) {
                            data_.n_detections[frame_id] = 1;
                        }
                    }
                } catch (...) {
                    data_.n_detections[frame_id] = 0;
                }
            }
            
            if (data_.total_frames > 10) {
                std::cout << "  Note: Only scanned first 10 frames for valid detections." << std::endl;
                std::cout << "  Assuming remaining frames have valid detections." << std::endl;
                for (size_t i = 10; i < data_.total_frames; ++i) {
                    data_.n_detections[i] = 1;
                }
            }
        }

        if (!data_.n_detections.empty()) {
            auto max_iter = std::max_element(data_.n_detections.begin(), data_.n_detections.end());
            int32_t actual_max = (max_iter != data_.n_detections.end()) ? *max_iter : 0;
            if (actual_max != data_.max_detections) {
                std::cout << "  Note: Actual max detections (" << actual_max 
                          << ") differs from array dimension (" << data_.max_detections << ")" << std::endl;
            }
        }

        if (arrayExists(store, "scores")) {
            if (loadScores(store, "scores")) {
                std::cout << "  Loaded scores array" << std::endl;
                data_.has_scores = true;
            } else {
                std::cerr << "Warning: scores array exists but failed to load" << std::endl;
            }
        } else {
            std::cout << "  No scores array found (optional)" << std::endl;
        }

        if (arrayExists(store, "class_ids")) {
            if (loadClassIDs(store, "class_ids")) {
                std::cout << "  Loaded class_ids array" << std::endl;
                data_.has_class_ids = true;
            } else {
                std::cerr << "Warning: class_ids array exists but failed to load" << std::endl;
            }
        } else {
            std::cout << "  No class_ids array found (optional)" << std::endl;
        }

        int64_t total_detections = 0;
        for (const auto& n : data_.n_detections) {
            total_detections += n;
        }

        std::cout << "Successfully loaded Zarr detections:" << std::endl;
        std::cout << "  Total frames: " << data_.total_frames << std::endl;
        std::cout << "  Max detections per frame: " << data_.max_detections << std::endl;
        std::cout << "  Total detections: " << total_detections << std::endl;
        std::cout << "  Has scores: " << data_.has_scores << std::endl;
        std::cout << "  Has class IDs: " << data_.has_class_ids << std::endl;

        return true;

    } catch (const std::exception& e) {
        std::cerr << "Error in loadStandardFormat: " << e.what() << std::endl;
        return false;
    }
}

bool ZarrDetectionLoader::loadBoundingBoxes(const ts::kvstore::KvStore& store,
                                           const std::string& path) {
    try {
        auto open_result = openArrayAny<float, 3>(store, path, context_);

        if (!open_result.ok()) {
            return false;
        }
        data_.bboxes_store = open_result.value();

        auto domain = data_.bboxes_store.domain();
        data_.total_frames = domain.shape()[0];
        data_.max_detections = domain.shape()[1];

        return true;

    } catch (const std::exception& e) {
        std::cerr << "Error loading bboxes from " << path << ": " << e.what() << std::endl;
        return false;
    }
}

bool ZarrDetectionLoader::loadScores(const ts::kvstore::KvStore& store,
                                    const std::string& path) {
    try {
        auto open_result = openArrayAny<float, 2>(store, path, context_);

        if (!open_result.ok()) {
            return false;
        }

        data_.scores_store = open_result.value();
        data_.has_scores = true;
        return true;

    } catch (...) {
        return false;
    }
}

bool ZarrDetectionLoader::loadClassIDs(const ts::kvstore::KvStore& store,
                                      const std::string& path) {
    try {
        auto open_result = openArrayAny<int32_t, 2>(store, path, context_);

        if (!open_result.ok()) {
            return false;
        }

        data_.class_ids_store = open_result.value();
        data_.has_class_ids = true;
        return true;

    } catch (...) {
        return false;
    }
}

bool ZarrDetectionLoader::loadNDetections(const ts::kvstore::KvStore& store,
                                         const std::string& path) {
    try {
        auto open_result = openArrayAny<int32_t, 1>(store, path, context_);

        if (!open_result.ok()) {
            return false;
        }

        auto n_det_store = open_result.value();
        auto read_result = ts::Read(n_det_store).result();
        if (!read_result.ok()) {
            return false;
        }

        auto n_det_array = read_result.value();
        data_.total_frames = n_det_array.shape()[0];

        data_.n_detections.clear();
        data_.n_detections.reserve(data_.total_frames);
        for (int i = 0; i < data_.total_frames; ++i) {
            data_.n_detections.push_back(n_det_array.data()[i]);
        }

        return true;

    } catch (const std::exception& e) {
        std::cerr << "Error loading n_detections: " << e.what() << std::endl;
        return false;
    }
}

bool ZarrDetectionLoader::loadMetadata(const ts::kvstore::KvStore& store) {
    try {
        bool metadata_found = false;

        bool fps_from_metadata = false;
        double duration_seconds = 0.0;
        double root_fps = 0.0;
        std::string root_video_path_hint;

        auto readAttrsWithFsFallback = [&](const std::string& path) -> std::optional<json> {
            if (auto attrs = readAttrsAny(store, path)) {
                return attrs;
            }
            if (root_path_.empty()) {
                return std::nullopt;
            }
            std::filesystem::path json_path = std::filesystem::path(root_path_);
            if (!path.empty()) {
                json_path /= std::filesystem::path(path);
            }
            json_path /= "zarr.json";
            return readAttrsFromZarrJsonFile(json_path);
        };

        if (auto root_attrs = readAttrsWithFsFallback("")) {
            auto assignIfString = [&](const json& object, const char* key, std::string& dest) {
                if (object.contains(key) && object[key].is_string()) {
                    dest = object[key].get<std::string>();
                    return true;
                }
                return false;
            };

            auto assignIfNumber = [&](const json& object, const char* key, double& dest) {
                if (object.contains(key) && object[key].is_number()) {
                    dest = object[key].get<double>();
                    return true;
                }
                return false;
            };

            if (root_attrs->contains("fps") && (*root_attrs)["fps"].is_number()) {
                root_fps = (*root_attrs)["fps"].get<double>();
            }

            assignIfString(*root_attrs, "source_path", root_video_path_hint) ||
                assignIfString(*root_attrs, "source_video_path", root_video_path_hint) ||
                assignIfString(*root_attrs, "source_video", root_video_path_hint) ||
                assignIfString(*root_attrs, "path", root_video_path_hint);

            if (root_video_path_hint.empty() &&
                root_attrs->contains("source_video_metadata") &&
                (*root_attrs)["source_video_metadata"].is_object()) {
                const auto& source_video_metadata = (*root_attrs)["source_video_metadata"];
                assignIfString(source_video_metadata, "source_path", root_video_path_hint) ||
                    assignIfString(source_video_metadata, "source_video_path", root_video_path_hint) ||
                    assignIfString(source_video_metadata, "source_video", root_video_path_hint) ||
                    assignIfString(source_video_metadata, "path", root_video_path_hint);
            }

            if (data_.total_frames == 0) {
                double frames = 0.0;
                if (assignIfNumber(*root_attrs, "total_frames", frames) ||
                    assignIfNumber(*root_attrs, "n_frames", frames) ||
                    assignIfNumber(*root_attrs, "source_video_total_frames", frames)) {
                    if (frames > 0.0) {
                        data_.total_frames = static_cast<size_t>(frames);
                        metadata_found = true;
                    }
                }
            }

            if (data_.image_width <= 0) {
                double width = 0.0;
                if (assignIfNumber(*root_attrs, "video_width", width) ||
                    assignIfNumber(*root_attrs, "width", width) ||
                    assignIfNumber(*root_attrs, "source_video_width", width)) {
                    if (width > 0.0) {
                        data_.image_width = static_cast<int>(width);
                        metadata_found = true;
                    }
                }
            }

            if (data_.image_height <= 0) {
                double height = 0.0;
                if (assignIfNumber(*root_attrs, "video_height", height) ||
                    assignIfNumber(*root_attrs, "height", height) ||
                    assignIfNumber(*root_attrs, "source_video_height", height)) {
                    if (height > 0.0) {
                        data_.image_height = static_cast<int>(height);
                        metadata_found = true;
                    }
                }
            }

            if (duration_seconds <= 0.0) {
                assignIfNumber(*root_attrs, "duration_seconds", duration_seconds);
            }
        }

        if (auto attrs = readAttrsWithFsFallback("raw_video")) {
            metadata_found = true;

            auto assignIfString = [&](const char* key, std::string& dest) {
                if (attrs->contains(key) && (*attrs)[key].is_string()) {
                    dest = (*attrs)[key].get<std::string>();
                    return true;
                }
                return false;
            };

            if (!assignIfString("source_path", data_.video_path)) {
                assignIfString("source_video", data_.video_path);
            }

            if (attrs->contains("total_frames") && (*attrs)["total_frames"].is_number()) {
                data_.total_frames = static_cast<size_t>((*attrs)["total_frames"].get<double>());
            }
            if (attrs->contains("fps") && (*attrs)["fps"].is_number()) {
                data_.fps = (*attrs)["fps"].get<double>();
                fps_from_metadata = true;
            } else if (attrs->contains("frame_rate") && (*attrs)["frame_rate"].is_number()) {
                data_.fps = (*attrs)["frame_rate"].get<double>();
                fps_from_metadata = true;
            }
            if (attrs->contains("video_width") && (*attrs)["video_width"].is_number()) {
                data_.image_width = static_cast<int>((*attrs)["video_width"].get<double>());
            }
            if (attrs->contains("video_height") && (*attrs)["video_height"].is_number()) {
                data_.image_height = static_cast<int>((*attrs)["video_height"].get<double>());
            }
            if ((data_.image_width <= 0 || data_.image_height <= 0) &&
                attrs->contains("original_resolution") &&
                (*attrs)["original_resolution"].is_array() &&
                (*attrs)["original_resolution"].size() >= 2) {
                const auto& res = (*attrs)["original_resolution"];
                int dim0 = res[0].is_number() ? static_cast<int>(res[0].get<double>()) : 0;
                int dim1 = res[1].is_number() ? static_cast<int>(res[1].get<double>()) : 0;
                if (data_.image_height <= 0 && dim0 > 0) {
                    data_.image_height = dim0;
                }
                if (data_.image_width <= 0 && dim1 > 0) {
                    data_.image_width = dim1;
                }
            }
            if (attrs->contains("video_duration_seconds") && (*attrs)["video_duration_seconds"].is_number()) {
                duration_seconds = (*attrs)["video_duration_seconds"].get<double>();
            }
        }

        if (data_.video_path.empty() && !root_video_path_hint.empty()) {
            data_.video_path = root_video_path_hint;
        }

        if (!fps_from_metadata && root_fps > 0.0) {
            data_.fps = root_fps;
            fps_from_metadata = true;
        }

        if (!fps_from_metadata && duration_seconds > 0.0 && data_.total_frames > 0) {
            double computed_fps = static_cast<double>(data_.total_frames) / duration_seconds;
            if (computed_fps > 0.0) {
                data_.fps = computed_fps;
                fps_from_metadata = true;
            }
        }

        if (!fps_from_metadata && data_.fps <= 0.0) {
            data_.fps = 30.0;
        }

        if (data_.image_width <= 0 || data_.image_height <= 0 || data_.total_frames == 0) {
            auto video_result = openArrayAny<uint8_t, 3>(
                store,
                "raw_video/images_full",
                context_);

            if (video_result.ok()) {
                auto domain = video_result.value().domain();
                if (domain.rank() >= 3) {
                    if (data_.total_frames == 0) {
                        data_.total_frames = static_cast<size_t>(domain.shape()[0]);
                    }
                    if (data_.image_height <= 0) {
                        data_.image_height = static_cast<int>(domain.shape()[1]);
                    }
                    if (data_.image_width <= 0) {
                        data_.image_width = static_cast<int>(domain.shape()[2]);
                    }
                    metadata_found = true;
                }
            } else {
                // Try downsampled images as a fallback
                video_result = openArrayAny<uint8_t, 3>(
                    store,
                    "raw_video/images_ds",
                    context_);

                if (video_result.ok()) {
                    auto domain = video_result.value().domain();
                    if (domain.rank() >= 3) {
                        if (data_.total_frames == 0) {
                            data_.total_frames = static_cast<size_t>(domain.shape()[0]);
                        }
                        if (data_.image_height <= 0) {
                            data_.image_height = static_cast<int>(domain.shape()[1]);
                        }
                        if (data_.image_width <= 0) {
                            data_.image_width = static_cast<int>(domain.shape()[2]);
                        }
                        metadata_found = true;
                    }
                }
            }
        }

        return metadata_found;

    } catch (...) {
        return false;
    }
}

std::vector<LoggedBoundingBox> ZarrDetectionLoader::getBoundingBoxesForFrame(size_t frame_id) const {
    std::vector<LoggedBoundingBox> result;
    
    if (frame_id >= data_.total_frames) {
        return result;
    }
    
    auto detections = getRawDetections(frame_id, false);
    
    for (size_t i = 0; i < detections.boxes.size(); ++i) {
        LoggedBoundingBox box;
        
        box.x_min = detections.boxes[i][0];
        box.y_min = detections.boxes[i][1];
        
        float x_max = detections.boxes[i][2];
        float y_max = detections.boxes[i][3];
        box.width = x_max - box.x_min;
        box.height = y_max - box.y_min;
        
        box.payload_frame_id = frame_id;
        box.payload_camera_id = 0;
        box.box_index_in_payload = static_cast<uint8_t>(i);
        
        if (i < detections.class_ids.size()) {
            box.class_id = static_cast<uint16_t>(detections.class_ids[i]);
        } else {
            box.class_id = 0;
        }
        
        if (i < detections.scores.size()) {
            box.confidence = detections.scores[i];
        } else {
            box.confidence = 1.0f;
        }
        
        int64_t timestamp_ns = static_cast<int64_t>(frame_id / data_.fps * 1e9);
        box.payload_timestamp_ns_epoch = timestamp_ns;
        box.received_timestamp_ns_epoch = timestamp_ns;
        
        result.push_back(box);
    }
    
    return result;
}

int32_t ZarrDetectionLoader::getDetectionsForFrame(size_t frame_id) const {
    if (frame_id >= data_.total_frames) {
        return 0;
    }
    if (frame_id >= data_.n_detections.size()) {
        return 0;
    }
    return data_.n_detections[frame_id];
}

LoggedBoundingBox ZarrDetectionLoader::convertToLoggedBox(
    const std::array<float, 4>& box,
    float score,
    int32_t class_id,
    size_t frame_id,
    size_t box_index,
    bool is_interpolated
) const {
    LoggedBoundingBox result;
    
    result.x_min = box[0];
    result.y_min = box[1];
    result.width = box[2] - box[0];
    result.height = box[3] - box[1];
    
    result.payload_frame_id = frame_id;
    result.payload_camera_id = 0;
    result.box_index_in_payload = static_cast<uint8_t>(box_index);
    
    result.class_id = static_cast<uint16_t>(class_id);
    result.confidence = score;
    
    int64_t timestamp_ns = static_cast<int64_t>(frame_id / data_.fps * 1e9);
    result.payload_timestamp_ns_epoch = timestamp_ns;
    result.received_timestamp_ns_epoch = timestamp_ns;
    
    return result;
}

std::optional<std::string> ZarrDetectionLoader::findZarrDetectionFile(const std::string& directory) {
    auto discovery = discoverZarrArchiveInDirectory(directory);
    if (!discovery.selected_archive.has_value()) {
        if (!discovery.error_message.empty()) {
            std::cerr << "Zarr discovery failed: " << discovery.error_message << std::endl;
        }
        return std::nullopt;
    }
    return discovery.selected_archive->string();
}


bool loadZarrDetectionFromPath(
    const std::string& zarr_path,
    ZarrDetectionLoader& loader,
    std::string& error_message
) {
    namespace fs = std::filesystem;

    auto archive_path = resolveExplicitZarrArchivePath(zarr_path);
    if (!archive_path.has_value()) {
        std::error_code ec;
        if (fs::is_directory(fs::path(zarr_path), ec)) {
            auto discovery = discoverZarrArchiveInDirectory(zarr_path);
            if (discovery.selected_archive.has_value()) {
                archive_path = discovery.selected_archive;
            } else if (!discovery.error_message.empty()) {
                error_message = discovery.error_message;
                return false;
            }
        }
    }
    if (!archive_path.has_value()) {
        fs::path probe_dir(zarr_path);
        std::error_code ec;

        while (!probe_dir.empty() && !fs::exists(probe_dir, ec)) {
            probe_dir = probe_dir.parent_path();
        }
        if (!probe_dir.empty() && fs::is_regular_file(probe_dir, ec)) {
            probe_dir = probe_dir.parent_path();
        }

        if (!probe_dir.empty() && fs::is_directory(probe_dir, ec)) {
            auto discovery = discoverZarrArchiveInDirectory(probe_dir.string());
            if (discovery.selected_archive.has_value()) {
                archive_path = discovery.selected_archive;
            }
        }
    }
    if (!archive_path.has_value()) {
        error_message = "Provided path is not a valid .zarr/.zr3 archive: " + zarr_path;
        return false;
    }

    std::cout << "Using zarr archive: " << archive_path->string() << std::endl;
    return loader.loadZarrFile(archive_path->string(), error_message);
}

bool loadZarrDetectionFromDirectory(
    const std::string& dir_path,
    ZarrDetectionLoader& loader,
    std::string& error_message
) {
    auto discovery = discoverZarrArchiveInDirectory(dir_path);
    if (!discovery.selected_archive.has_value()) {
        error_message = discovery.error_message.empty()
                            ? "No compatible zarr archive found in directory"
                            : discovery.error_message;
        return false;
    }

    std::cout << "Found zarr file: " << discovery.selected_archive->string() << std::endl;
    return loader.loadZarrFile(discovery.selected_archive->string(), error_message);
}

bool ZarrDetectionLoader::loadInterpolationRuns(const ts::kvstore::KvStore& store) {
    try {
        data_.latest_interpolation = InterpolationRunData();
        data_.has_interpolation = false;

        bool refined_loaded = false;
        bool stimulus_loaded = false;

        if (data_.layout == ZarrLayoutType::kPaletteRuns) {
            refined_loaded = loadRefinedDetectRuns(store);
        }

        if (loadStimulusAlignment(store)) {
            stimulus_loaded = true;
        }

        if (refined_loaded || stimulus_loaded) {
            data_.has_interpolation = true;
            return true;
        }

        // Fallback to legacy refined/interpolation runs
        data_.latest_interpolation = InterpolationRunData();

        if (data_.layout == ZarrLayoutType::kPaletteRuns) {
            if (auto refined_attrs = readAttrsAny(store, "refined_runs")) {
                std::string latest_run = extractLatestRunName(*refined_attrs);

                if (!latest_run.empty()) {
                    std::string base_path = "refined_runs/" + latest_run + "/";
                    std::string subgroup;

                    if (arrayExists(store, base_path + "interpolated/frame_indices")) {
                        subgroup = "interpolated";
                    } else if (arrayExists(store, base_path + "filtered/frame_indices")) {
                        subgroup = "filtered";
                    }

                    if (!subgroup.empty()) {
                        std::cout << "  Found refined run: " << latest_run
                                  << " (" << subgroup << ")" << std::endl;
                        if (loadPaletteInterpolationRun(store, latest_run, subgroup)) {
                            return true;
                        }
                    } else {
                        std::cout << "  Refined run " << latest_run
                                  << " does not contain 'interpolated' or 'filtered' outputs" << std::endl;
                    }
                }
            }
            // Fall through to legacy loaders if refined data not available
        }

        std::vector<std::string> run_names;
        if (!root_path_.empty()) {
            run_names = collect_runs_fs(
                root_path_,
                "interpolation_runs",
                {"bboxes", "interpolation_mask"});
        }

        run_names.erase(
            std::remove_if(
                run_names.begin(),
                run_names.end(),
                [](const std::string& name) {
                    return name.rfind("interp_", 0) != 0;
                }),
            run_names.end());

        if (run_names.empty()) {
            std::cout << "  No interpolation runs found" << std::endl;
            return false;
        }

        std::cout << "  Found " << run_names.size() << " interpolation run(s)" << std::endl;
        std::string latest_run = run_names.back();
        std::cout << "  Loading latest run: " << latest_run << std::endl;

        return loadLatestInterpolationRun(store, latest_run);
        
    } catch (const std::exception& e) {
        std::cerr << "Error loading interpolation runs: " << e.what() << std::endl;
        return false;
    }
}


bool ZarrDetectionLoader::loadLatestInterpolationRun(const ts::kvstore::KvStore& store, 
                                                     const std::string& run_name) {
    try {
        data_.latest_interpolation.uses_palette_layout = false;
        std::string base_path = "interpolation_runs/" + run_name + "/";
        
        // Try to load metadata, but don't fail if it's missing or empty
        data_.latest_interpolation.run_name = run_name;
        data_.latest_interpolation.method.clear();
        data_.latest_interpolation.created_at.clear();

        bool metadata_loaded = false;
        if (auto attrs = readAttrsAny(store, base_path)) {
            metadata_loaded = true;
            if (attrs->contains("created_at") && (*attrs)["created_at"].is_string()) {
                data_.latest_interpolation.created_at = (*attrs)["created_at"].get<std::string>();
            }
            if (data_.latest_interpolation.created_at.empty() &&
                attrs->contains("created_at_utc") && (*attrs)["created_at_utc"].is_string()) {
                data_.latest_interpolation.created_at =
                    (*attrs)["created_at_utc"].get<std::string>();
            }
            if (attrs->contains("method") && (*attrs)["method"].is_string()) {
                data_.latest_interpolation.method = (*attrs)["method"].get<std::string>();
            }
            std::cout << "    Loaded interpolation metadata from zarr.json/.zattrs" << std::endl;
        } else {
            std::cout << "    No zarr.json/.zattrs metadata found, using defaults" << std::endl;
        }

        if (metadata_loaded) {
            if (data_.latest_interpolation.method.empty()) {
                data_.latest_interpolation.method = "linear";
            }
        } else {
            if (run_name.find("_linear_") != std::string::npos) {
                data_.latest_interpolation.method = "linear";
            } else if (run_name.find("_cubic_") != std::string::npos) {
                data_.latest_interpolation.method = "cubic";
            } else if (run_name.find("_nearest_") != std::string::npos) {
                data_.latest_interpolation.method = "nearest";
            } else {
                data_.latest_interpolation.method = "unknown";
            }
        }

        if (!data_.latest_interpolation.created_at.empty()) {
            std::cout << "    Created: " << data_.latest_interpolation.created_at << std::endl;
        }
        std::cout << "    Method: " << data_.latest_interpolation.method << std::endl;
        
        // Now load the actual data arrays - these are required
        
        std::cout << "    Loading interpolated bboxes from: " << base_path + "bboxes" << std::endl;
        
        auto bbox_result = openArrayAny<float, 3>(
            store,
            base_path + "bboxes",
            context_);
        
        if (!bbox_result.ok()) {
            std::cerr << "Failed to load interpolated bboxes: " << bbox_result.status().ToString() << std::endl;
            return false;
        }
        
        data_.latest_interpolation.bboxes_store = bbox_result.value();
        std::cout << "    Successfully loaded interpolated bboxes" << std::endl;
        
        // Load interpolation mask (true = interpolated, false = original)
        std::cout << "    Loading interpolation mask from: " << base_path + "interpolation_mask" << std::endl;
        
        auto mask_result = openArrayAny<bool, 1>(
            store,
            base_path + "interpolation_mask",
            context_);
        
        if (!mask_result.ok()) {
            std::cerr << "Failed to load interpolation mask: " << mask_result.status().ToString() << std::endl;
            return false;
        }
        
        data_.latest_interpolation.interpolation_mask = mask_result.value();
        data_.latest_interpolation.is_loaded = true;
        data_.has_interpolation = true;
        
        std::cout << "    Successfully loaded interpolation mask" << std::endl;
        
        // Get dimensions for validation
        auto bbox_domain = data_.latest_interpolation.bboxes_store.domain();
        auto mask_domain = data_.latest_interpolation.interpolation_mask.domain();
        
        size_t bbox_frames = bbox_domain.shape()[0];
        size_t mask_frames = mask_domain.shape()[0];
        
        std::cout << "    Bbox frames: " << bbox_frames << std::endl;
        std::cout << "    Mask frames: " << mask_frames << std::endl;
        
        if (bbox_frames != mask_frames) {
            std::cerr << "    Warning: Frame count mismatch between bboxes and mask!" << std::endl;
        }
        
        // Try to count interpolated frames (but don't fail if it doesn't work)
        try {
            auto mask_array = ts::Read(data_.latest_interpolation.interpolation_mask).result().value();
            auto* mask_data = static_cast<const bool*>(mask_array.data());
            
            size_t interpolated_count = 0;
            for (size_t i = 0; i < mask_frames; ++i) {
                if (mask_data[i]) {
                    interpolated_count++;
                }
            }
            
            std::cout << "    Interpolated frames: " << interpolated_count 
                      << " / " << mask_frames << std::endl;
        } catch (const std::exception& e) {
            std::cout << "    Could not count interpolated frames: " << e.what() << std::endl;
        }
        
        std::cout << "    Successfully loaded interpolation run: " << run_name << std::endl;
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "Error loading interpolation run " << run_name 
                  << ": " << e.what() << std::endl;
        return false;
    }
}

bool ZarrDetectionLoader::isFrameInterpolated(size_t frame_id) const {
    if (frame_id < data_.frame_interpolated_flags.size()) {
        return data_.frame_interpolated_flags[frame_id] != 0;
    }

    if (active_dataset_ != DetectionDataset::RefinedInterpolated) {
        return false;
    }

    if (!data_.has_interpolation) {
        return false;
    }

    if (!data_.latest_interpolation.frame_mask.empty() &&
        frame_id < data_.latest_interpolation.frame_mask.size()) {
        return data_.latest_interpolation.frame_mask[frame_id] != 0;
    }

    if (frame_id >= data_.total_frames) {
        return false;
    }

    if (data_.layout == ZarrLayoutType::kPaletteRuns &&
        data_.latest_interpolation.uses_palette_layout) {
        return false;
    }
    
    try {
        auto ts_frame_id = static_cast<tensorstore::Index>(frame_id);
        
        auto mask_future = ts::Read(
            data_.latest_interpolation.interpolation_mask | 
            ts::Dims(0).IndexSlice(ts_frame_id)
        );
        
        auto mask_array = mask_future.result().value();
        auto* mask_data = static_cast<const bool*>(mask_array.data());
        
        return mask_data[0];
        
    } catch (const std::exception& e) {
        std::cerr << "Error checking interpolation status for frame " 
                  << frame_id << ": " << e.what() << std::endl;
        return false;
    }
}

std::vector<LoggedBoundingBox> ZarrDetectionLoader::getBoundingBoxesForFrame(
    size_t frame_id, bool use_interpolated) const {
    
    if (use_interpolated && data_.has_interpolation) {
        auto detections = getRawDetections(frame_id, true);
        return convertDetectionsToLoggedBoxes(detections, frame_id);
    } else {
        return getBoundingBoxesForFrame(frame_id);
    }
}

ZarrDetectionLoader::FrameDetections ZarrDetectionLoader::getRawDetections(
    size_t frame_id, bool use_interpolated, bool include_eye_masks) const {
    
    FrameDetections result;
    result.frame_id = frame_id;
    result.includes_eye_masks = false;
    const float nan_value = std::numeric_limits<float>::quiet_NaN();
    
    if (frame_id >= data_.total_frames) {
        return result;
    }
    if (!hasDetectionData()) {
        return result;
    }

    if (data_.layout == ZarrLayoutType::kPaletteRuns) {
        const bool has_palette_interp =
            data_.latest_interpolation.uses_palette_layout &&
            data_.latest_interpolation.is_loaded;
        const bool want_interpolated = use_interpolated && has_palette_interp;

        const auto& offsets = want_interpolated
            ? data_.latest_interpolation.frame_offsets
            : data_.frame_offsets;
        const auto& boxes_source = want_interpolated
            ? data_.latest_interpolation.bbox_norm_coords
            : data_.bbox_norm_coords;
        const auto& scores_source = want_interpolated
            ? data_.latest_interpolation.flat_scores
            : data_.flat_scores;
        const auto& class_source = want_interpolated
            ? data_.latest_interpolation.flat_class_ids
            : data_.flat_class_ids;

        const bool has_scores = want_interpolated
            ? !data_.latest_interpolation.flat_scores.empty()
            : data_.has_scores;
        const bool has_class_ids = want_interpolated
            ? !data_.latest_interpolation.flat_class_ids.empty()
            : data_.has_class_ids;
        const bool can_use_headings =
            data_.has_heading_data && !want_interpolated;
        const bool has_roi_metadata =
            !data_.roi_offset_x.empty() && !data_.mask_roi_indices.empty();
        const bool can_use_eye_masks =
            include_eye_masks && !want_interpolated &&
            ((data_.has_eye_masks && data_.eye_masks_loaded) || has_roi_metadata);
        const bool can_use_keypoints =
            data_.has_keypoints && !want_interpolated &&
            data_.keypoints_per_detection > 0 &&
            !data_.flat_keypoints_px.empty();

        if (offsets.empty() || frame_id + 1 >= offsets.size()) {
            return result;
        }

        size_t start = offsets[frame_id];
        size_t end = offsets[frame_id + 1];

        if (can_use_headings) {
            result.headings_deg.reserve(end - start);
            result.swim_bladder_pixels.reserve(end - start);
            result.heading_valid.reserve(end - start);
        }
        if (can_use_eye_masks) {
            result.eye_masks.reserve(end - start);
            result.includes_eye_masks = true;
        }
        if (can_use_keypoints) {
            result.keypoints_pixels.reserve(end - start);
            result.keypoint_labels = data_.keypoint_labels;
            result.skeleton_edges = data_.skeleton_edges;
            result.keypoints_per_detection = data_.keypoints_per_detection;
            result.has_keypoints = false;
            result.is_refined_keypoints = data_.is_refined_keypoints;
            if (data_.is_refined_keypoints) {
                result.keypoint_quality_labels.reserve(end - start);
                result.keypoint_reason.reserve(end - start);
                result.keypoint_flip_corrected.reserve(end - start);
                result.keypoint_usable.reserve(end - start);
                result.keypoint_refined_success.reserve(end - start);
                result.keypoint_detection_source.reserve(end - start);
            }
        }
        if (!data_.detection_source_flags.empty()) {
            result.detection_source.reserve(end - start);
        }
        if (!data_.detection_reason_flags.empty()) {
            result.detection_reason.reserve(end - start);
        }

        for (size_t idx = start; idx < end && idx < boxes_source.size(); ++idx) {
            auto pixel_box = normalizedBoxToPixels(
                boxes_source[idx],
                data_.image_width,
                data_.image_height
            );
            result.boxes.push_back(pixel_box);

            if (has_scores && idx < scores_source.size()) {
                result.scores.push_back(scores_source[idx]);
            } else {
                result.scores.push_back(1.0f);
            }

            if (has_class_ids && idx < class_source.size()) {
                result.class_ids.push_back(class_source[idx]);
            } else {
                result.class_ids.push_back(0);
            }

            if (!data_.detection_source_flags.empty()) {
                if (idx < data_.detection_source_flags.size()) {
                    result.detection_source.push_back(data_.detection_source_flags[idx]);
                } else {
                    result.detection_source.push_back(0);
                }
            }
            if (!data_.detection_reason_flags.empty()) {
                if (idx < data_.detection_reason_flags.size()) {
                    result.detection_reason.push_back(data_.detection_reason_flags[idx]);
                } else {
                    result.detection_reason.emplace_back();
                }
            }

            if (can_use_keypoints) {
                std::vector<std::array<float, 2>> kp_set(
                    data_.keypoints_per_detection,
                    std::array<float, 2>{nan_value, nan_value});
                size_t stride = data_.keypoints_per_detection * 2;
                size_t base = idx * stride;
                if (base + stride <= data_.flat_keypoints_px.size()) {
                    bool detection_has_points = false;
                    for (size_t kp_idx = 0; kp_idx < data_.keypoints_per_detection; ++kp_idx) {
                        float px = data_.flat_keypoints_px[base + kp_idx * 2 + 0];
                        float py = data_.flat_keypoints_px[base + kp_idx * 2 + 1];
                        kp_set[kp_idx][0] = px;
                        kp_set[kp_idx][1] = py;
                        if (std::isfinite(px) && std::isfinite(py)) {
                            detection_has_points = true;
                        }
                    }
                    if (detection_has_points) {
                        result.has_keypoints = true;
                    }
                }
                result.keypoints_pixels.push_back(std::move(kp_set));

                if (data_.is_refined_keypoints) {
                    result.keypoint_quality_labels.push_back(
                        idx < data_.flat_keypoint_quality_labels.size()
                            ? data_.flat_keypoint_quality_labels[idx] : -1);
                    result.keypoint_reason.push_back(
                        idx < data_.flat_keypoint_reason.size()
                            ? data_.flat_keypoint_reason[idx] : std::string());
                    result.keypoint_flip_corrected.push_back(
                        idx < data_.flat_keypoint_flip_corrected.size()
                            ? data_.flat_keypoint_flip_corrected[idx] : 0);
                    result.keypoint_usable.push_back(
                        idx < data_.flat_keypoint_usable.size()
                            ? data_.flat_keypoint_usable[idx] : 0);
                    result.keypoint_refined_success.push_back(
                        idx < data_.flat_keypoint_refined_success.size()
                            ? data_.flat_keypoint_refined_success[idx] : 0);
                    result.keypoint_detection_source.push_back(
                        idx < data_.flat_keypoint_detection_source.size()
                            ? data_.flat_keypoint_detection_source[idx] : 0);
                }
            }

            if (can_use_headings) {
                float heading = (idx < data_.flat_headings_deg.size())
                    ? data_.flat_headings_deg[idx]
                    : 0.0f;
                uint8_t valid = (idx < data_.flat_heading_valid.size())
                    ? data_.flat_heading_valid[idx]
                    : 0;
                std::array<float, 2> anchor = {nan_value, nan_value};
                if (idx < data_.flat_swim_bladder_px.size()) {
                    anchor = data_.flat_swim_bladder_px[idx];
                }
                if (std::isnan(anchor[0]) || std::isnan(anchor[1])) {
                    float cx = pixel_box[0] + 0.5f * (pixel_box[2] - pixel_box[0]);
                    float cy = pixel_box[1] + 0.5f * (pixel_box[3] - pixel_box[1]);
                    anchor = {cx, cy};
                }
                result.headings_deg.push_back(heading);
                result.swim_bladder_pixels.push_back(anchor);
                result.heading_valid.push_back(valid);
            }

            if (can_use_eye_masks) {
                FrameDetections::EyeMask mask_entry;
                mask_entry.offset_x =
                    (idx < data_.roi_offset_x.size()) ? data_.roi_offset_x[idx] : nan_value;
                mask_entry.offset_y =
                    (idx < data_.roi_offset_y.size()) ? data_.roi_offset_y[idx] : nan_value;
                mask_entry.roi_width =
                    (idx < data_.roi_width_px.size()) ? data_.roi_width_px[idx] : 0.0f;
                mask_entry.roi_height =
                    (idx < data_.roi_height_px.size()) ? data_.roi_height_px[idx] : 0.0f;
                mask_entry.rows = static_cast<int>(data_.eye_mask_height);
                mask_entry.cols = static_cast<int>(data_.eye_mask_width);

                int32_t roi_lookup =
                    (idx < data_.mask_roi_indices.size()) ? data_.mask_roi_indices[idx] : -1;
                mask_entry.roi_index = roi_lookup;
                bool offsets_valid = std::isfinite(mask_entry.offset_x) && std::isfinite(mask_entry.offset_y);
                bool dims_valid = (mask_entry.roi_width > 0.0f && mask_entry.roi_height > 0.0f);

                if (data_.has_eye_masks && data_.eye_masks_loaded &&
                    roi_lookup >= 0 &&
                    static_cast<size_t>(roi_lookup) < data_.eye_mask_roi_count &&
                    offsets_valid && dims_valid) {
                    populateEyeMaskEntry(static_cast<size_t>(roi_lookup), mask_entry);
                }
                if (data_.has_eye_angles && roi_lookup >= 0) {
                    size_t roi_idx = static_cast<size_t>(roi_lookup);
                    bool roi_valid = data_.eye_angle_valid_mask.empty() ||
                                     (roi_idx < data_.eye_angle_valid_mask.size() &&
                                      data_.eye_angle_valid_mask[roi_idx] != 0);
                    if (roi_idx < data_.eye_angle_left_deg.size()) {
                        float left_angle = data_.eye_angle_left_deg[roi_idx];
                        mask_entry.feret_minor_angle_deg[0] = left_angle;
                        mask_entry.feret_angle_valid[0] =
                            (roi_valid && std::isfinite(left_angle)) ? 1 : 0;
                    }
                    if (roi_idx < data_.eye_angle_right_deg.size()) {
                        float right_angle = data_.eye_angle_right_deg[roi_idx];
                        mask_entry.feret_minor_angle_deg[1] = right_angle;
                        mask_entry.feret_angle_valid[1] =
                            (roi_valid && std::isfinite(right_angle)) ? 1 : 0;
                    }
                    mask_entry.has_eye_angles =
                        (mask_entry.feret_angle_valid[0] != 0) ||
                        (mask_entry.feret_angle_valid[1] != 0);
                }

                result.eye_masks.push_back(std::move(mask_entry));
            }
        }

        result.is_interpolated = isFrameInterpolated(frame_id);
        return result;
    }
    
    try {
        auto ts_frame_id = static_cast<tensorstore::Index>(frame_id);
        
        bool use_interp = use_interpolated && data_.has_interpolation;
        ts::TensorStore<float, 3>* bbox_source = nullptr;
        
        if (use_interp) {
            bbox_source = const_cast<ts::TensorStore<float, 3>*>(
                &data_.latest_interpolation.bboxes_store);
            result.is_interpolated = isFrameInterpolated(frame_id);
        } else {
            bbox_source = const_cast<ts::TensorStore<float, 3>*>(&data_.bboxes_store);
            result.is_interpolated = false;
        }
        
        auto bbox_future = ts::Read(
            *bbox_source | 
            ts::Dims(0).IndexSlice(ts_frame_id)
        );
        
        auto bbox_array = bbox_future.result().value();
        auto* bbox_data = static_cast<const float*>(bbox_array.data());
        
        // FIX: For interpolated data, we need to check the actual array dimensions
        // or scan for valid boxes since n_detections might not apply
        int valid_detections;
        if (use_interp) {
            // For interpolated data, check the shape of the array
            // The second dimension should be max_detections
            auto shape = bbox_array.shape();
            int max_dets = shape[0];  // This is the max detections dimension
            
            // Count valid detections by checking for non-negative values
            valid_detections = 0;
            for (int det_idx = 0; det_idx < max_dets; ++det_idx) {
                float first_coord = bbox_data[det_idx * 4];
                if (first_coord >= 0) {
                    valid_detections++;
                } else {
                    break;  // Assuming detections are packed at the beginning
                }
            }
        } else {
            // Use original n_detections for non-interpolated frames
            if (frame_id >= data_.n_detections.size()) {
                valid_detections = 0;
            } else {
                valid_detections = std::max(data_.n_detections[frame_id], 0);
            }
        }
        
        for (int det_idx = 0; det_idx < valid_detections; ++det_idx) {
            std::array<float, 4> box;
            for (int coord = 0; coord < 4; ++coord) {
                box[coord] = bbox_data[det_idx * 4 + coord];
            }
            
            if (box[0] >= 0) {
                result.boxes.push_back(box);
                if (!data_.detection_source_flags.empty()) {
                    size_t idx = static_cast<size_t>(det_idx);
                    if (idx < data_.detection_source_flags.size()) {
                        result.detection_source.push_back(data_.detection_source_flags[idx]);
                    } else {
                        result.detection_source.push_back(0);
                    }
                }
                if (!data_.detection_reason_flags.empty()) {
                    size_t idx = static_cast<size_t>(det_idx);
                    if (idx < data_.detection_reason_flags.size()) {
                        result.detection_reason.push_back(data_.detection_reason_flags[idx]);
                    } else {
                        result.detection_reason.emplace_back();
                    }
                }
                
                if (!use_interp && data_.has_scores) {
                    auto score_future = ts::Read(
                        data_.scores_store | 
                        ts::Dims(0).IndexSlice(ts_frame_id)
                    );
                    auto score_array = score_future.result().value();
                    auto* score_data = static_cast<const float*>(score_array.data());
                    result.scores.push_back(score_data[det_idx]);
                } else {
                    result.scores.push_back(1.0f);
                }
                
                if (!use_interp && data_.has_class_ids) {
                    auto class_future = ts::Read(
                        data_.class_ids_store | 
                        ts::Dims(0).IndexSlice(ts_frame_id)
                    );
                    auto class_array = class_future.result().value();
                    auto* class_data = static_cast<const int32_t*>(class_array.data());
                    result.class_ids.push_back(class_data[det_idx]);
                } else {
                    result.class_ids.push_back(0);
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error reading frame " << frame_id << ": " << e.what() << std::endl;
    }

    result.is_interpolated = isFrameInterpolated(frame_id);
    
    return result;
}

std::vector<LoggedBoundingBox> ZarrDetectionLoader::convertDetectionsToLoggedBoxes(
    const FrameDetections& detections, size_t frame_id) const {
    
    std::vector<LoggedBoundingBox> result;
    
    for (size_t i = 0; i < detections.boxes.size(); ++i) {
        LoggedBoundingBox box = convertToLoggedBox(
            detections.boxes[i],
            i < detections.scores.size() ? detections.scores[i] : 1.0f,
            i < detections.class_ids.size() ? detections.class_ids[i] : 0,
            frame_id,
            i,
            detections.is_interpolated
        );
        result.push_back(box);
    }
    
    return result;
}

ZarrDetectionLoader::KeypointRoiMetadata
ZarrDetectionLoader::getCropRoiMetadataForRoiIndex(int32_t roi_index) const {
    KeypointRoiMetadata metadata;
    metadata.roi_index = roi_index;
    if (roi_index < 0) {
        return metadata;
    }

    auto fill_from_detection_row = [&](size_t det_row) {
        metadata.valid = true;
        if (det_row < data_.roi_offset_x.size()) {
            metadata.offset_x = data_.roi_offset_x[det_row];
        }
        if (det_row < data_.roi_offset_y.size()) {
            metadata.offset_y = data_.roi_offset_y[det_row];
        }
        if (det_row < data_.roi_width_px.size()) {
            metadata.roi_width = data_.roi_width_px[det_row];
        }
        if (det_row < data_.roi_height_px.size()) {
            metadata.roi_height = data_.roi_height_px[det_row];
        }
        metadata.has_crop_metadata =
            std::isfinite(metadata.offset_x) &&
            std::isfinite(metadata.offset_y) &&
            metadata.roi_width > 0.0f &&
            metadata.roi_height > 0.0f;
    };

    for (size_t det_row = 0; det_row < data_.keypoint_roi_indices.size(); ++det_row) {
        if (data_.keypoint_roi_indices[det_row] == roi_index) {
            fill_from_detection_row(det_row);
            return metadata;
        }
    }
    for (size_t det_row = 0; det_row < data_.mask_roi_indices.size(); ++det_row) {
        if (data_.mask_roi_indices[det_row] == roi_index) {
            fill_from_detection_row(det_row);
            return metadata;
        }
    }
    return metadata;
}

ZarrDetectionLoader::KeypointRoiMetadata
ZarrDetectionLoader::getKeypointRoiMetadataForFrameDetection(
    size_t frame_id,
    size_t detection_idx,
    bool use_interpolated) const {
    KeypointRoiMetadata metadata;

    // Refined/raw keypoint rows are aligned to the non-interpolated detection order.
    // Synthetic interpolated rows do not have a stable keypoint ROI identity.
    if (use_interpolated) {
        return metadata;
    }
    if (!data_.has_keypoints || data_.keypoint_roi_indices.empty()) {
        return metadata;
    }
    if (frame_id >= data_.total_frames) {
        return metadata;
    }
    if (data_.frame_offsets.empty() || frame_id + 1 >= data_.frame_offsets.size()) {
        return metadata;
    }

    size_t start = data_.frame_offsets[frame_id];
    size_t end = data_.frame_offsets[frame_id + 1];
    size_t frame_detection_count = (end >= start) ? (end - start) : 0;
    if (detection_idx >= frame_detection_count) {
        return metadata;
    }

    size_t det_row = start + detection_idx;
    if (det_row >= data_.keypoint_roi_indices.size()) {
        return metadata;
    }
    metadata.roi_index = data_.keypoint_roi_indices[det_row];
    metadata.valid = metadata.roi_index >= 0;
    if (!metadata.valid) {
        return metadata;
    }

    if (det_row < data_.roi_offset_x.size()) {
        metadata.offset_x = data_.roi_offset_x[det_row];
    }
    if (det_row < data_.roi_offset_y.size()) {
        metadata.offset_y = data_.roi_offset_y[det_row];
    }
    if (det_row < data_.roi_width_px.size()) {
        metadata.roi_width = data_.roi_width_px[det_row];
    }
    if (det_row < data_.roi_height_px.size()) {
        metadata.roi_height = data_.roi_height_px[det_row];
    }

    metadata.has_crop_metadata =
        std::isfinite(metadata.offset_x) &&
        std::isfinite(metadata.offset_y) &&
        metadata.roi_width > 0.0f &&
        metadata.roi_height > 0.0f;
    return metadata;
}

int32_t ZarrDetectionLoader::getKeypointRoiIndexForFrameDetection(
    size_t frame_id,
    size_t detection_idx,
    bool use_interpolated) const {
    return getKeypointRoiMetadataForFrameDetection(
               frame_id, detection_idx, use_interpolated)
        .roi_index;
}

bool ZarrDetectionLoader::getCropImageForIndex(int32_t roi_index,
                                               CropImageView& out_view) const {
    if (!data_.crop_data.loaded || roi_index < 0) {
        return false;
    }
    size_t index = static_cast<size_t>(roi_index);
    if (index >= data_.crop_data.roi_count) {
        return false;
    }
    size_t stride = data_.crop_data.width * data_.crop_data.channels;
    size_t plane = data_.crop_data.height * stride;
    size_t offset = index * plane;
    if (offset + plane > data_.crop_data.images.size()) {
        return false;
    }
    out_view.roi_index = roi_index;
    out_view.data = data_.crop_data.images.data() + offset;
    out_view.width = data_.crop_data.width;
    out_view.height = data_.crop_data.height;
    out_view.channels = data_.crop_data.channels;
    out_view.stride = stride;
    return true;
}
