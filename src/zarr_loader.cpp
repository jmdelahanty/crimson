#include "zarr_loader.h"
#include <iostream>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstring>
#include <tensorstore/kvstore/operations.h>  // For kvstore::Read
#include <tensorstore/cast.h>  // For Cast operation
#include <tensorstore/driver/zarr/dtype.h> // For GetZarrDType

using json = nlohmann::json;

ZarrDetectionLoader::ZarrDetectionLoader() {
    // Initialize TensorStore context with default settings
    context_ = ts::Context::Default();
}

ZarrDetectionLoader::~ZarrDetectionLoader() {
    // TensorStore handles cleanup automatically
}

bool ZarrDetectionLoader::loadZarrFile(const std::string& filepath,
                                       std::string& error_message) {
    try {
        std::cout << "Opening Zarr store: " << filepath << std::endl;

        // Open the zarr store
        auto store_result = ts::kvstore::Open({
            {"driver", "file"},
            {"path", filepath}
        }, context_).result();

        if (!store_result.ok()) {
            error_message = "Failed to open zarr store: " +
                          store_result.status().ToString();
            return false;
        }

        auto store = std::move(store_result.value());

        // Check for standard format first (bboxes at root level)
        auto check_standard = ts::kvstore::Read(store, "bboxes/.zarray").result();
        if (check_standard.ok()) {
            std::cout << "Detected standard Zarr format" << std::endl;
            return loadStandardFormat(store);
        }

        // Check for detect_runs format
        auto check_runs = ts::kvstore::Read(store, "detect_runs/.zattrs").result();
        if (check_runs.ok()) {
            std::cout << "Detected detect_runs Zarr format" << std::endl;
            return loadDetectRunsFormat(store);
        }

        error_message = "Unrecognized zarr format - no bboxes or detect_runs found";
        return false;

    } catch (const std::exception& e) {
        error_message = std::string("Exception loading zarr: ") + e.what();
        return false;
    }
}

bool ZarrDetectionLoader::loadStandardFormat(const ts::kvstore::KvStore& store) {
    try {
        // Load metadata first
        if (!loadMetadata(store)) {
            std::cerr << "Warning: Could not load metadata from .zattrs" << std::endl;
        }

        // Load n_detections to get frame count
        if (!loadNDetections(store, "n_detections")) {
            std::cerr << "Error: Could not load n_detections array" << std::endl;
            return false;
        }

        // Load bounding boxes (required)
        if (!loadBoundingBoxes(store, "bboxes")) {
            std::cerr << "Error: Could not load bboxes array" << std::endl;
            return false;
        }

        // Load scores (optional)
        if (loadScores(store, "scores")) {
            std::cout << "  Loaded scores array" << std::endl;
            data_.has_scores = true;
        }

        // Load class IDs (optional)
        if (loadClassIDs(store, "class_ids")) {
            std::cout << "  Loaded class_ids array" << std::endl;
            data_.has_class_ids = true;
        }

        std::cout << "Successfully loaded Zarr detections:" << std::endl;
        std::cout << "  Total frames: " << data_.total_frames << std::endl;
        std::cout << "  Max detections per frame: " << data_.max_detections << std::endl;
        std::cout << "  Has scores: " << data_.has_scores << std::endl;
        std::cout << "  Has class IDs: " << data_.has_class_ids << std::endl;

        return true;

    } catch (const std::exception& e) {
        std::cerr << "Error in loadStandardFormat: " << e.what() << std::endl;
        return false;
    }
}

bool ZarrDetectionLoader::loadDetectRunsFormat(const ts::kvstore::KvStore& store) {
    try {
        // Read the detect_runs attributes to get the latest run
        auto attrs_result = ts::kvstore::Read(store, "detect_runs/.zattrs").result();
        if (!attrs_result.ok()) {
            std::cerr << "Could not read detect_runs/.zattrs" << std::endl;
            return false;
        }

        auto attrs = json::parse(attrs_result.value().value.Flatten());
        if (!attrs.contains("latest")) {
            std::cerr << "No 'latest' field in detect_runs/.zattrs" << std::endl;
            return false;
        }

        std::string latest_run = attrs["latest"];
        std::string run_prefix = "detect_runs/" + latest_run + "/";
        std::cout << "Loading run: " << latest_run << std::endl;

        // Load arrays from the run subdirectory
        if (!loadNDetections(store, run_prefix + "n_detections")) {
            std::cerr << "Error: Could not load n_detections from run" << std::endl;
            return false;
        }

        // Try both bbox_norm_coords and bboxes
        if (!loadBoundingBoxes(store, run_prefix + "bbox_norm_coords")) {
            if (!loadBoundingBoxes(store, run_prefix + "bboxes")) {
                std::cerr << "Error: Could not load bbox data from run" << std::endl;
                return false;
            }
        } else {
            data_.coordinates_normalized = true;  // bbox_norm_coords implies normalized
        }

        // Try to load optional arrays
        loadScores(store, run_prefix + "scores");
        loadClassIDs(store, run_prefix + "class_ids");

        // Load run metadata
        auto run_attrs_result = ts::kvstore::Read(store, run_prefix + ".zattrs").result();
        if (run_attrs_result.ok()) {
            auto run_attrs = json::parse(run_attrs_result.value().value.Flatten());
            if (run_attrs.contains("fps")) {
                data_.fps = run_attrs["fps"];
            }
            if (run_attrs.contains("video_path")) {
                data_.video_path = run_attrs["video_path"];
            }
        }

        return true;

    } catch (const std::exception& e) {
        std::cerr << "Error in loadDetectRunsFormat: " << e.what() << std::endl;
        return false;
    }
}

bool ZarrDetectionLoader::loadBoundingBoxes(const ts::kvstore::KvStore& store,
                                           const std::string& path) {
    try {
        json spec;
        spec["driver"] = "zarr";
        spec["kvstore"] = store.spec().value().ToJson().value();
        spec["path"] = path;

        auto open_result = ts::Open<float, 3>(
            spec,
            ts::OpenMode::open,
            ts::ReadWriteMode::read,
            context_
        ).result();

        if (!open_result.ok()) {
            return false;
        }
        data_.bboxes_store = open_result.value();

        // Get dimensions
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
        json spec;
        spec["driver"] = "zarr";
        spec["kvstore"] = store.spec().value().ToJson().value();
        spec["path"] = path;

        auto open_result = ts::Open<float, 2>(
            spec,
            ts::OpenMode::open,
            ts::ReadWriteMode::read,
            context_
        ).result();

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
        json spec;
        spec["driver"] = "zarr";
        spec["kvstore"] = store.spec().value().ToJson().value();
        spec["path"] = path;

        auto open_result = ts::Open<int32_t, 2>(
            spec,
            ts::OpenMode::open,
            ts::ReadWriteMode::read,
            context_
        ).result();

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
        json spec;
        spec["driver"] = "zarr";
        spec["kvstore"] = store.spec().value().ToJson().value();
        spec["path"] = path;

        auto open_result = ts::Open<int32_t, 1>(
            spec,
            ts::OpenMode::open,
            ts::ReadWriteMode::read,
            context_
        ).result();

        if (!open_result.ok()) {
            return false;
        }

        auto n_det_store = open_result.value();

        // Read all n_detections into memory (usually small array)
        auto read_result = ts::Read(n_det_store).result();
        if (!read_result.ok()) {
            return false;
        }

        auto n_det_array = read_result.value();
        data_.total_frames = n_det_array.shape()[0];

        // Convert to vector
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
        auto attrs_result = ts::kvstore::Read(store, ".zattrs").result();
        if (!attrs_result.ok()) {
            return false;
        }

        auto attrs = json::parse(attrs_result.value().value.Flatten());

        if (attrs.contains("video_path")) {
            data_.video_path = attrs["video_path"];
        }
        if (attrs.contains("fps")) {
            data_.fps = attrs["fps"];
        }
        if (attrs.contains("model_path")) {
            data_.model_path = attrs["model_path"];
        }

        return true;

    } catch (...) {
        return false;
    }
}

std::vector<LoggedBoundingBox> ZarrDetectionLoader::getBoundingBoxesForFrame(size_t frame_id) const {
    std::vector<LoggedBoundingBox> boxes;

    if (frame_id >= data_.total_frames) {
        return boxes;
    }

    int32_t n_dets = data_.n_detections[frame_id];
    if (n_dets <= 0) {
        return boxes;
    }

    try {
        // Read bbox slice for this frame
        auto bbox_slice = data_.bboxes_store |
                         ts::Dims(0).SizedInterval(ts::Index(frame_id), ts::Index(1)) |
                         ts::Dims(1).SizedInterval(ts::Index(0), ts::Index(n_dets));

        auto bbox_result = ts::Read(bbox_slice).result();
        if (!bbox_result.ok()) {
            std::cerr << "Failed to read bboxes for frame " << frame_id << std::endl;
            return boxes;
        }
        auto bbox_array = bbox_result.value();

        // Read scores if available
        ts::SharedArray<float, 2> scores_array;
        if (data_.has_scores) {
            auto scores_slice = data_.scores_store |
                              ts::Dims(0).SizedInterval(ts::Index(frame_id), ts::Index(1)) |
                              ts::Dims(1).SizedInterval(ts::Index(0), ts::Index(n_dets));
            ts::Read(scores_slice, scores_array).result().value();
        }

        // Read class IDs if available
        ts::SharedArray<int32_t, 2> class_array;
        if (data_.has_class_ids) {
            auto class_slice = data_.class_ids_store |
                              ts::Dims(0).SizedInterval(ts::Index(frame_id), ts::Index(1)) |
                              ts::Dims(1).SizedInterval(ts::Index(0), ts::Index(n_dets));
            ts::Read(class_slice, class_array).result().value();
        }

        // Convert to LoggedBoundingBox format
        boxes.reserve(n_dets);
        for (int32_t i = 0; i < n_dets; ++i) {
            std::array<float, 4> box = {
                bbox_array.data()[i * 4],
                bbox_array.data()[i * 4 + 1],
                bbox_array.data()[i * 4 + 2],
                bbox_array.data()[i * 4 + 3]
            };

            float score = data_.has_scores && scores_array.data() != nullptr ? scores_array.data()[i] : 1.0f;
            int32_t class_id = data_.has_class_ids && class_array.data() != nullptr ? class_array.data()[i] : 0;

            boxes.push_back(convertToLoggedBox(box, score, class_id, frame_id, i));
        }

    } catch (const std::exception& e) {
        std::cerr << "Error reading frame " << frame_id << ": " << e.what() << std::endl;
    }

    return boxes;
}

ZarrDetectionLoader::FrameDetections ZarrDetectionLoader::getRawDetections(size_t frame_id) const {
    FrameDetections result;
    result.frame_id = frame_id;

    if (frame_id >= data_.total_frames) {
        return result;
    }

    int32_t n_dets = data_.n_detections[frame_id];
    if (n_dets <= 0) {
        return result;
    }

    try {
        // Read bboxes
        auto bbox_slice = data_.bboxes_store |
                         ts::Dims(0).SizedInterval(ts::Index(frame_id), ts::Index(1)) |
                         ts::Dims(1).SizedInterval(ts::Index(0), ts::Index(n_dets));

        auto bbox_array = ts::Read(bbox_slice).result().value();

        result.boxes.reserve(n_dets);
        for (int32_t i = 0; i < n_dets; ++i) {
            std::array<float, 4> box = {
                bbox_array.data()[i * 4],
                bbox_array.data()[i * 4 + 1],
                bbox_array.data()[i * 4 + 2],
                bbox_array.data()[i * 4 + 3]
            };
            result.boxes.push_back(box);
        }

        // Read scores if available
        if (data_.has_scores) {
            auto scores_slice = data_.scores_store |
                              ts::Dims(0).SizedInterval(ts::Index(frame_id), ts::Index(1)) |
                              ts::Dims(1).SizedInterval(ts::Index(0), ts::Index(n_dets));
            auto scores_array = ts::Read(scores_slice).result().value();

            result.scores.reserve(n_dets);
            for (int32_t i = 0; i < n_dets; ++i) {
                result.scores.push_back(scores_array.data()[i]);
            }
        }

        // Read class IDs if available
        if (data_.has_class_ids) {
            auto class_slice = data_.class_ids_store |
                              ts::Dims(0).SizedInterval(ts::Index(frame_id), ts::Index(1)) |
                              ts::Dims(1).SizedInterval(ts::Index(0), ts::Index(n_dets));
            auto class_array = ts::Read(class_slice).result().value();

            result.class_ids.reserve(n_dets);
            for (int32_t i = 0; i < n_dets; ++i) {
                result.class_ids.push_back(class_array.data()[i]);
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "Error reading raw detections for frame " << frame_id
                  << ": " << e.what() << std::endl;
    }

    return result;
}

int32_t ZarrDetectionLoader::getDetectionsForFrame(size_t frame_id) const {
    if (frame_id >= data_.total_frames) {
        return 0;
    }
    return data_.n_detections[frame_id];
}

LoggedBoundingBox ZarrDetectionLoader::convertToLoggedBox(
    const std::array<float, 4>& box,
    float score,
    int32_t class_id,
    size_t frame_id,
    size_t box_index
) const {
    LoggedBoundingBox result;

    // Detect format: if box[2] and box[3] are large, likely x1,y1,x2,y2
    // Otherwise likely x,y,w,h
    bool is_xyxy = (box[2] > 10 && box[3] > 10);  // Simple heuristic

    if (is_xyxy) {
        // Format: [x_min, y_min, x_max, y_max]
        result.x_min = box[0];
        result.y_min = box[1];
        result.width = box[2] - box[0];
        result.height = box[3] - box[1];
    } else {
        // Format: [x, y, width, height]
        result.x_min = box[0];
        result.y_min = box[1];
        result.width = box[2];
        result.height = box[3];
    }

    // Fill in other fields
    result.confidence = score;
    result.class_id = static_cast<uint16_t>(class_id);
    result.payload_frame_id = frame_id;
    result.payload_camera_id = 0;  // Assuming single camera
    result.box_index_in_payload = static_cast<uint8_t>(box_index);

    // Calculate timestamp from frame number and FPS
    int64_t timestamp_ns = static_cast<int64_t>(frame_id / data_.fps * 1e9);
    result.payload_timestamp_ns_epoch = timestamp_ns;
    result.received_timestamp_ns_epoch = timestamp_ns;

    return result;
}

std::optional<std::string> ZarrDetectionLoader::findZarrDetectionFile(const std::string& directory) {
    namespace fs = std::filesystem;

    try {
        for (const auto& entry : fs::directory_iterator(directory)) {
            if (entry.is_directory()) {
                std::string filename = entry.path().filename().string();

                // Check if it has .zarr extension
                if (filename.find(".zarr") != std::string::npos ||
                    filename.find(".zr3") != std::string::npos) {

                    // Check if it contains "detection" or "chaser" in the name
                    if (filename.find("detection") != std::string::npos ||
                        filename.find("chaser") != std::string::npos) {
                        return entry.path().string();
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error searching for zarr files: " << e.what() << std::endl;
    }

    return std::nullopt;
}

// Standalone helper function
bool loadZarrDetectionFromDirectory(
    const std::string& dir_path,
    ZarrDetectionLoader& loader,
    std::string& error_message
) {
    auto zarr_file = ZarrDetectionLoader::findZarrDetectionFile(dir_path);

    if (!zarr_file.has_value()) {
        error_message = "No zarr detection file found in directory";
        return false;
    }

    std::cout << "Found zarr file: " << zarr_file.value() << std::endl;
    return loader.loadZarrFile(zarr_file.value(), error_message);
}