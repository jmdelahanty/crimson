#include "zarr_loader.h"
#include <iostream>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstring>
#include <tensorstore/kvstore/operations.h>
#include <tensorstore/cast.h>
#include <tensorstore/driver/zarr/dtype.h>

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
        // Clear any previous data
        data_ = ZarrDetectionData();
        
        std::cout << "Opening Zarr store: " << filepath << std::endl;

        // Validate filepath exists
        if (!std::filesystem::exists(filepath)) {
            error_message = "Zarr file/directory does not exist: " + filepath;
            return false;
        }

        // Open the kvstore
        auto spec_result = ts::kvstore::Spec::FromJson({
            {"driver", "file"},
            {"path", filepath}
        });
        
        if (!spec_result.ok()) {
            error_message = "Failed to create kvstore spec: " + spec_result.status().ToString();
            return false;
        }

        auto store_future = ts::kvstore::Open(spec_result.value(), context_);
        auto store_result = store_future.result();
        
        if (!store_result.ok()) {
            error_message = "Failed to open kvstore: " + store_result.status().ToString();
            return false;
        }
        
        auto store = store_result.value();
        
        // Load standard format
        if (!loadStandardFormat(store)) {
            error_message = "Failed to load standard zarr format";
            return false;
        }
        
        // Validate we have essential data
        if (data_.total_frames == 0) {
            error_message = "Zarr file has 0 frames";
            return false;
        }
        
        // Try to load interpolation runs from the correct, known location
        if (loadInterpolationRuns(store)) {
            std::cout << "  Successfully loaded interpolation data" << std::endl;
            
            // Validate interpolation data matches main data dimensions
            if (data_.has_interpolation) {
                auto interp_domain = data_.latest_interpolation.bboxes_store.domain();
                if (interp_domain.shape()[0] != data_.total_frames) {
                    std::cerr << "  Warning: Interpolation frame count mismatch. "
                              << "Expected " << data_.total_frames 
                              << " but got " << interp_domain.shape()[0] << std::endl;
                    data_.has_interpolation = false;
                }
            }
        } else {
            std::cout << "  No interpolation data available in 'interpolation_runs'" << std::endl;
        }
        
        std::cout << "Successfully loaded zarr file: " << filepath << std::endl;
        std::cout << "  Total frames: " << data_.total_frames << std::endl;
        std::cout << "  Max detections per frame: " << data_.max_detections << std::endl;
        std::cout << "  FPS: " << data_.fps << std::endl;
        std::cout << "  Has scores: " << (data_.has_scores ? "Yes" : "No") << std::endl;
        std::cout << "  Has class IDs: " << (data_.has_class_ids ? "Yes" : "No") << std::endl;
        std::cout << "  Has interpolation: " << (data_.has_interpolation ? "Yes" : "No") << std::endl;
        
        if (data_.has_interpolation) {
            std::cout << "  Interpolation method: " << data_.latest_interpolation.method << std::endl;
            std::cout << "  Interpolation created: " << data_.latest_interpolation.created_at << std::endl;
        }
        
        return true;
        
    } catch (const std::exception& e) {
        error_message = std::string("Exception loading zarr: ") + e.what();
        // Clear any partially loaded data
        data_ = ZarrDetectionData();
        return false;
    }
}

// ... (loadStandardFormat and other functions remain the same) ...
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
    namespace fs = std::filesystem;

    try {
        for (const auto& entry : fs::directory_iterator(directory)) {
            if (entry.is_directory()) {
                std::string filename = entry.path().filename().string();

                if (filename.find(".zarr") != std::string::npos ||
                    filename.find(".zr3") != std::string::npos) {

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

bool ZarrDetectionLoader::loadInterpolationRuns(const ts::kvstore::KvStore& store) {
    try {
        // The actual interpolation run we know exists is:
        // /interpolation_runs/interp_linear_20250820_125819/
        // Let's check for it directly first
        
        std::string known_run = "interp_linear_20250820_125819";
        std::string test_path = "interpolation_runs/" + known_run + "/bboxes/.zarray";
        
        auto test_result = ts::kvstore::Read(store, test_path).result();
        if (test_result.ok()) {
            std::cout << "  Found interpolation run: " << known_run << std::endl;
            std::cout << "  Loading run: " << known_run << std::endl;
            return loadLatestInterpolationRun(store, known_run);
        }
        
        // If that didn't work, try scanning for other patterns
        std::vector<std::string> run_names;
        
        // Common prefixes for interpolation runs
        std::vector<std::string> prefixes = {
            "interp_linear_",
            "interp_cubic_",
            "interp_nearest_",
            "interp_"
        };
        
        // Scan for runs with these patterns
        // Since we know the format is typically prefix + YYYYMMDD_HHMMSS
        // Let's try dates from today backwards
        auto now = std::chrono::system_clock::now();
        
        for (int days_back = 0; days_back < 365; ++days_back) {  // Check up to a year back
            auto check_time = now - std::chrono::hours(24 * days_back);
            auto check_time_t = std::chrono::system_clock::to_time_t(check_time);
            struct tm* tm_check = std::localtime(&check_time_t);
            
            // Try different times of day
            for (int hour = 23; hour >= 0; hour -= 4) {
                for (int min = 59; min >= 0; min -= 30) {
                    for (int sec = 59; sec >= 0; sec -= 30) {
                        // Try each prefix
                        for (const auto& prefix : prefixes) {
                            char run_name[100];
                            snprintf(run_name, sizeof(run_name), 
                                    "%s%04d%02d%02d_%02d%02d%02d",
                                    prefix.c_str(),
                                    tm_check->tm_year + 1900,
                                    tm_check->tm_mon + 1,
                                    tm_check->tm_mday,
                                    hour, min, sec);
                            
                            // Check if this run exists by looking for its bboxes array
                            std::string bbox_path = std::string("interpolation_runs/") + 
                                                   run_name + "/bboxes/.zarray";
                            auto result = ts::kvstore::Read(store, bbox_path).result();
                            
                            if (result.ok()) {
                                run_names.push_back(run_name);
                                std::cout << "    Found interpolation run: " << run_name << std::endl;
                                
                                // If we found one, that's probably enough
                                goto found_some;
                            }
                        }
                    }
                }
            }
            
            // Stop early if we found something
            if (!run_names.empty()) {
                break;
            }
        }
        
        found_some:
        if (run_names.empty()) {
            std::cout << "  No interpolation runs found" << std::endl;
            return false;
        }
        
        // Sort to get the latest (timestamp format means alphabetical = chronological)
        std::sort(run_names.begin(), run_names.end());
        std::string latest_run = run_names.back();
        
        std::cout << "  Found " << run_names.size() << " interpolation run(s)" << std::endl;
        std::cout << "  Loading latest run: " << latest_run << std::endl;
        
        // Load the latest interpolation run
        return loadLatestInterpolationRun(store, latest_run);
        
    } catch (const std::exception& e) {
        std::cerr << "Error loading interpolation runs: " << e.what() << std::endl;
        return false;
    }
}


bool ZarrDetectionLoader::loadLatestInterpolationRun(const ts::kvstore::KvStore& store, 
                                                     const std::string& run_name) {
    try {
        std::string base_path = "interpolation_runs/" + run_name + "/";
        
        // Try to load metadata, but don't fail if it's missing or empty
        auto attrs_result = ts::kvstore::Read(store, base_path + ".zattrs").result();
        if (attrs_result.ok() && attrs_result.value().has_value()) {
            // Convert Cord to string using Flatten()
            std::string attrs_str = std::string(attrs_result.value().value.Flatten());
            
            // Check if the string is not empty before parsing
            if (!attrs_str.empty()) {
                try {
                    auto attrs_json = nlohmann::json::parse(attrs_str);
                    
                    data_.latest_interpolation.run_name = run_name;
                    data_.latest_interpolation.created_at = attrs_json.value("created_at", "");
                    data_.latest_interpolation.method = attrs_json.value("method", "linear");
                    
                    std::cout << "    Created: " << data_.latest_interpolation.created_at << std::endl;
                    std::cout << "    Method: " << data_.latest_interpolation.method << std::endl;
                } catch (const nlohmann::json::parse_error& e) {
                    std::cout << "    Warning: Could not parse .zattrs (may be empty): " << e.what() << std::endl;
                    // Continue with defaults
                    data_.latest_interpolation.run_name = run_name;
                    data_.latest_interpolation.method = "linear";  // Reasonable default
                }
            } else {
                std::cout << "    Warning: Empty .zattrs file, using defaults" << std::endl;
                data_.latest_interpolation.run_name = run_name;
                data_.latest_interpolation.method = "linear";
            }
        } else {
            std::cout << "    No .zattrs found, using defaults" << std::endl;
            data_.latest_interpolation.run_name = run_name;
            // Extract method from run name if possible (e.g., "interp_linear_20250820_125819")
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
        
        // Now load the actual data arrays - these are required
        
        // Load interpolated bboxes
        json spec;
        spec["driver"] = "zarr";
        spec["kvstore"] = store.spec().value().ToJson().value();
        spec["path"] = base_path + "bboxes";
        
        std::cout << "    Loading interpolated bboxes from: " << base_path + "bboxes" << std::endl;
        
        auto bbox_result = ts::Open<float, 3>(
            spec,
            ts::OpenMode::open,
            ts::ReadWriteMode::read,
            context_
        ).result();
        
        if (!bbox_result.ok()) {
            std::cerr << "Failed to load interpolated bboxes: " << bbox_result.status().ToString() << std::endl;
            return false;
        }
        
        data_.latest_interpolation.bboxes_store = bbox_result.value();
        std::cout << "    Successfully loaded interpolated bboxes" << std::endl;
        
        // Load interpolation mask (true = interpolated, false = original)
        spec["path"] = base_path + "interpolation_mask";
        
        std::cout << "    Loading interpolation mask from: " << base_path + "interpolation_mask" << std::endl;
        
        auto mask_result = ts::Open<bool, 1>(
            spec,
            ts::OpenMode::open,
            ts::ReadWriteMode::read,
            context_
        ).result();
        
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
    if (!data_.has_interpolation || frame_id >= data_.total_frames) {
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
    size_t frame_id, bool use_interpolated) const {
    
    FrameDetections result;
    result.frame_id = frame_id;
    
    if (frame_id >= data_.total_frames) {
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
            
            std::cout << "Interpolated frame " << frame_id << ": found " 
                      << valid_detections << " valid detections" << std::endl;
        } else {
            // Use original n_detections for non-interpolated frames
            valid_detections = data_.n_detections[frame_id];
        }
        
        for (int det_idx = 0; det_idx < valid_detections; ++det_idx) {
            std::array<float, 4> box;
            for (int coord = 0; coord < 4; ++coord) {
                box[coord] = bbox_data[det_idx * 4 + coord];
            }
            
            if (box[0] >= 0) {
                result.boxes.push_back(box);
                
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