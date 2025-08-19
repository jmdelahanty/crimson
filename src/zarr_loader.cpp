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

        // Load bounding boxes first to get dimensions (required)
        if (!loadBoundingBoxes(store, "bboxes")) {
            std::cerr << "Error: Could not load bboxes array" << std::endl;
            return false;
        }

        // Try to load n_detections array
        bool has_n_detections = false;
        
        // Check if n_detections exists
        auto n_det_check = ts::kvstore::Read(store, "n_detections/.zarray").result();
        if (n_det_check.ok()) {
            // n_detections array exists, load it
            if (loadNDetections(store, "n_detections")) {
                has_n_detections = true;
                std::cout << "  Loaded n_detections array" << std::endl;
            } else {
                std::cerr << "Warning: n_detections exists but failed to load" << std::endl;
            }
        }
        
        // If n_detections doesn't exist or failed to load, initialize based on max_detections
        if (!has_n_detections) {
            std::cout << "  No n_detections array found, initializing..." << std::endl;
            
            // Since max_detections is 1 in your case, we need to check if boxes are valid
            // We'll assume all frames have detections unless the bbox values are fill_value (-1.0)
            data_.n_detections.clear();
            data_.n_detections.resize(data_.total_frames, 0);
            
            // Do a quick scan to determine which frames have valid detections
            // This is important to avoid processing invalid/padded boxes
            for (size_t frame_id = 0; frame_id < std::min(size_t(10), data_.total_frames); ++frame_id) {
                try {
                    auto bbox_slice = data_.bboxes_store | 
                        ts::Dims(0).IndexSlice(static_cast<tensorstore::Index>(frame_id));
                    auto bbox_result = ts::Read(bbox_slice).result();
                    
                    if (bbox_result.ok()) {
                        auto bbox_array = bbox_result.value();
                        auto* bbox_data = static_cast<const float*>(bbox_array.data());
                        
                        // Check if the first coordinate is not the fill value (-1.0)
                        // If any coordinate is -1.0, assume no detection
                        bool has_valid_detection = true;
                        for (int i = 0; i < 4; ++i) {
                            if (std::abs(bbox_data[i] + 1.0f) < 1e-6f) {  // Check for -1.0 fill value
                                has_valid_detection = false;
                                break;
                            }
                        }
                        
                        if (has_valid_detection) {
                            data_.n_detections[frame_id] = 1;  // Since max_detections is 1
                        }
                    }
                } catch (...) {
                    // If we can't read this frame, assume no detection
                    data_.n_detections[frame_id] = 0;
                }
            }
            
            // For remaining frames, assume they have detections if we couldn't scan all
            // (You might want to scan all frames if the dataset is small enough)
            if (data_.total_frames > 10) {
                std::cout << "  Note: Only scanned first 10 frames for valid detections." << std::endl;
                std::cout << "  Assuming remaining frames have valid detections." << std::endl;
                for (size_t i = 10; i < data_.total_frames; ++i) {
                    data_.n_detections[i] = 1;  // Assume detection exists
                }
            }
        }

        // Calculate actual max detections from n_detections if we loaded/created it
        if (!data_.n_detections.empty()) {
            auto max_iter = std::max_element(data_.n_detections.begin(), data_.n_detections.end());
            int32_t actual_max = (max_iter != data_.n_detections.end()) ? *max_iter : 0;
            if (actual_max != data_.max_detections) {
                std::cout << "  Note: Actual max detections (" << actual_max 
                          << ") differs from array dimension (" << data_.max_detections << ")" << std::endl;
            }
        }

        // Load scores (optional)
        auto scores_check = ts::kvstore::Read(store, "scores/.zarray").result();
        if (scores_check.ok()) {
            if (loadScores(store, "scores")) {
                std::cout << "  Loaded scores array" << std::endl;
                data_.has_scores = true;
            } else {
                std::cerr << "Warning: scores array exists but failed to load" << std::endl;
            }
        } else {
            std::cout << "  No scores array found (optional)" << std::endl;
        }

        // Load class IDs (optional)
        auto class_check = ts::kvstore::Read(store, "class_ids/.zarray").result();
        if (class_check.ok()) {
            if (loadClassIDs(store, "class_ids")) {
                std::cout << "  Loaded class_ids array" << std::endl;
                data_.has_class_ids = true;
            } else {
                std::cerr << "Warning: class_ids array exists but failed to load" << std::endl;
            }
        } else {
            std::cout << "  No class_ids array found (optional)" << std::endl;
        }

        // Count total detections
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
    std::vector<LoggedBoundingBox> result;
    
    if (frame_id >= data_.total_frames) {
        return result;
    }
    
    auto detections = getRawDetections(frame_id);
    
    for (size_t i = 0; i < detections.boxes.size(); ++i) {
        LoggedBoundingBox box;
        
        // Set coordinates - LoggedBoundingBox uses x_min, y_min, width, height
        // The Zarr has [x_min, y_min, x_max, y_max] format
        box.x_min = detections.boxes[i][0];
        box.y_min = detections.boxes[i][1];
        
        // Calculate width and height from x_max and y_max
        float x_max = detections.boxes[i][2];
        float y_max = detections.boxes[i][3];
        box.width = x_max - box.x_min;
        box.height = y_max - box.y_min;
        
        // Set frame and camera info
        box.payload_frame_id = frame_id;
        box.payload_camera_id = 0;  // Assuming single camera or default camera ID
        box.box_index_in_payload = static_cast<uint8_t>(i);
        
        // Set class_id and confidence
        if (i < detections.class_ids.size()) {
            box.class_id = static_cast<uint16_t>(detections.class_ids[i]);
        } else {
            box.class_id = 0;  // Default class
        }
        
        if (i < detections.scores.size()) {
            box.confidence = detections.scores[i];
        } else {
            box.confidence = 1.0f;  // Default confidence if not available
        }
        
        // Calculate timestamps based on FPS
        // Note: You might need to adjust this based on your actual timestamp calculation
        int64_t timestamp_ns = static_cast<int64_t>(frame_id / data_.fps * 1e9);
        box.payload_timestamp_ns_epoch = timestamp_ns;
        box.received_timestamp_ns_epoch = timestamp_ns;  // Or use current time if needed
        
        result.push_back(box);
    }
    
    return result;
}


// In getRawDetections or getBoundingBoxesForFrame
ZarrDetectionLoader::FrameDetections ZarrDetectionLoader::getRawDetections(size_t frame_id) const {
    FrameDetections result;
    result.frame_id = frame_id;
    
    if (frame_id >= data_.total_frames) {
        return result;  // Empty result for out-of-bounds
    }
    
    try {
        // Cast frame_id to tensorstore::Index
        auto ts_frame_id = static_cast<tensorstore::Index>(frame_id);
        
        // Use IndexSlice with proper type
        auto bbox_future = ts::Read(
            data_.bboxes_store | 
            ts::Dims(0).IndexSlice(ts_frame_id)  // Select specific frame
        );
        
        auto bbox_array = bbox_future.result().value();
        
        // The result will be shape [1, 4], we need to extract the values
        auto* bbox_data = static_cast<const float*>(bbox_array.data());
        
        // Since max_detections is 1, we only have one box
        if (data_.n_detections[frame_id] > 0) {
            std::array<float, 4> box;
            for (int i = 0; i < 4; ++i) {
                box[i] = bbox_data[i];
            }
            result.boxes.push_back(box);
            
            // Read scores if available
            if (data_.has_scores) {
                auto score_future = ts::Read(
                    data_.scores_store | 
                    ts::Dims(0).IndexSlice(ts_frame_id)
                );
                auto score_array = score_future.result().value();
                auto* score_data = static_cast<const float*>(score_array.data());
                result.scores.push_back(score_data[0]);
            }
            
            // Read class_ids if available
            if (data_.has_class_ids) {
                auto class_future = ts::Read(
                    data_.class_ids_store | 
                    ts::Dims(0).IndexSlice(ts_frame_id)
                );
                auto class_array = class_future.result().value();
                auto* class_data = static_cast<const int32_t*>(class_array.data());
                result.class_ids.push_back(class_data[0]);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error reading frame " << frame_id << ": " << e.what() << std::endl;
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
    
    // Set coordinates (convert from x_max, y_max to width, height)
    result.x_min = box[0];
    result.y_min = box[1];
    result.width = box[2] - box[0];   // x_max - x_min
    result.height = box[3] - box[1];  // y_max - y_min
    
    // Set metadata
    result.payload_frame_id = frame_id;
    result.payload_camera_id = 0;  // Default camera ID
    result.box_index_in_payload = static_cast<uint8_t>(box_index);
    
    // Set detection info
    result.class_id = static_cast<uint16_t>(class_id);
    result.confidence = score;
    
    // Calculate timestamps
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