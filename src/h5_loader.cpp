#include "h5_loader.h"
#include <iostream>
#include <algorithm>
#include <cstring>

using namespace H5;

namespace {
constexpr bool kH5DebugLoggingEnabled = false;
}

H5SessionLoader::H5SessionLoader() {
    // Turn off HDF5 error printing by default
    H5::Exception::dontPrint();
}

H5SessionLoader::~H5SessionLoader() {
}

bool H5SessionLoader::loadSessionInfo(H5::H5File& file, SessionInfo& info) {
    try {
        Group root = file.openGroup("/");

        // Helper lambda to read string attributes
        auto readStrAttr = [&](const H5::H5Object& obj, const std::string& name, std::string& value) -> bool {
            try {
                if (obj.attrExists(name)) {
                    Attribute attr = obj.openAttribute(name);
                    H5std_string str_val;
                    DataType dtype = attr.getDataType();
                    attr.read(dtype, str_val);
                    value = str_val;
                    return true;
                }
            } catch (...) {}
            return false;
        };

        // Helper lambda to read int64 attributes
        auto readInt64Attr = [&](const H5::H5Object& obj, const std::string& name, int64_t& value) -> bool {
            try {
                if (obj.attrExists(name)) {
                    Attribute attr = obj.openAttribute(name);
                    attr.read(PredType::NATIVE_INT64, &value);
                    return true;
                }
            } catch (...) {}
            return false;
        };

        // Helper lambda to read int attributes
        auto readIntAttr = [&](const H5::H5Object& obj, const std::string& name, int& value) -> bool {
            try {
                if (obj.attrExists(name)) {
                    Attribute attr = obj.openAttribute(name);
                    attr.read(PredType::NATIVE_INT, &value);
                    return true;
                }
            } catch (...) {}
            return false;
        };

        // Read all session attributes
        readStrAttr(root, "session_uuid", info.session_uuid);
        readStrAttr(root, "session_start_iso8601_utc", info.session_start_iso8601_utc);
        readInt64Attr(root, "session_start_ns_epoch", info.session_start_ns_epoch);
        readStrAttr(root, "loaded_protocol_filepath", info.loaded_protocol_filepath);
        readStrAttr(root, "protocol_name_from_definition", info.protocol_name_from_definition);
        readIntAttr(root, "stimulus_output_width", info.stimulus_output_width);
        readIntAttr(root, "stimulus_output_height", info.stimulus_output_height);
        readStrAttr(root, "hostname", info.hostname);
        readStrAttr(root, "rig_id", info.rig_id);
        readStrAttr(root, "arena_id", info.arena_id);
        readStrAttr(root, "active_ipc_source", info.active_ipc_source);
        readStrAttr(root, "ipc_source_name", info.ipc_source_name);
        readStrAttr(root, "software_version", info.software_version);
        readStrAttr(root, "git_commit_hash", info.git_commit_hash);
        readStrAttr(root, "operator_notes", info.operator_notes);

        // Read subject metadata if it exists
        if (groupExists(file, "/subject_metadata")) {
            Group subjectGroup = file.openGroup("/subject_metadata");
            hsize_t num_attrs = subjectGroup.getNumAttrs();
            for (hsize_t i = 0; i < num_attrs; i++) {
                Attribute attr = subjectGroup.openAttribute(i);
                std::string attr_name = attr.getName();
                H5std_string attr_value;
                DataType dtype = attr.getDataType();
                attr.read(dtype, attr_value);
                info.subject_metadata[attr_name] = attr_value;
            }
        }

        // Read associated camera IDs if they exist
        if (groupExists(file, "/associated_cameras")) {
            Group camerasGroup = file.openGroup("/associated_cameras");
            if (datasetExists(camerasGroup, "camera_ids")) {
                DataSet camDataset = camerasGroup.openDataSet("camera_ids");
                DataSpace dataspace = camDataset.getSpace();
                hsize_t dims[1];
                dataspace.getSimpleExtentDims(dims);

                std::vector<char*> c_strs(dims[0]);
                StrType strType(PredType::C_S1, H5T_VARIABLE);
                camDataset.read(c_strs.data(), strType);

                for (size_t i = 0; i < dims[0]; i++) {
                    if (c_strs[i]) {
                        info.associated_camera_ids.push_back(std::string(c_strs[i]));
                        free(c_strs[i]); // HDF5 allocates these
                    }
                }
            }
        }

        return true;
    } catch (const H5::Exception& e) {
        std::cerr << "Error loading session info: " << e.getCDetailMsg() << std::endl;
        return false;
    }
}

bool H5SessionLoader::loadEvents(H5::H5File& file, std::vector<EventLogEntry>& events) {
    try {
        if (!file.nameExists("/events")) {
            return false;
        }

        DataSet dataset = file.openDataSet("/events");
        DataSpace dataspace = dataset.getSpace();

        // Get dimensions
        hsize_t dims[1];
        dataspace.getSimpleExtentDims(dims);
        size_t num_events = dims[0];

        if (num_events == 0) {
            return true; // Empty but valid
        }

        // Create compound type for reading
        CompType memtype(sizeof(EventLogEntry));
        memtype.insertMember("timestamp_ns_epoch", HOFFSET(EventLogEntry, timestamp_ns_epoch), PredType::NATIVE_INT64);
        memtype.insertMember("timestamp_ns_session", HOFFSET(EventLogEntry, timestamp_ns_session), PredType::NATIVE_INT64);
        memtype.insertMember("event_type_id", HOFFSET(EventLogEntry, event_type_id), PredType::NATIVE_INT32);
        memtype.insertMember("current_step_index", HOFFSET(EventLogEntry, current_step_index), PredType::NATIVE_INT32);

        StrType name_str_type(PredType::C_S1, sizeof(EventLogEntry::name_or_context));
        name_str_type.setCset(H5T_CSET_UTF8);
        name_str_type.setStrpad(H5T_STR_NULLTERM);
        memtype.insertMember("name_or_context", HOFFSET(EventLogEntry, name_or_context), name_str_type);

        memtype.insertMember("stimulus_mode_id", HOFFSET(EventLogEntry, stimulus_mode_id), PredType::NATIVE_INT32);

        StrType details_str_type(PredType::C_S1, sizeof(EventLogEntry::details_json));
        details_str_type.setCset(H5T_CSET_UTF8);
        details_str_type.setStrpad(H5T_STR_NULLTERM);
        memtype.insertMember("details_json", HOFFSET(EventLogEntry, details_json), details_str_type);

        // Read data
        events.resize(num_events);
        dataset.read(events.data(), memtype);

        return true;
    } catch (const H5::Exception& e) {
        std::cerr << "Error loading events: " << e.getCDetailMsg() << std::endl;
        return false;
    }
}

bool H5SessionLoader::loadBoundingBoxes(H5::H5File& file, std::vector<LoggedBoundingBox>& boxes) {
    try {
        Group tracking_group = file.openGroup("/tracking_data");

        if (!datasetExists(tracking_group, "bounding_boxes")) {
            return false;
        }

        DataSet dataset = tracking_group.openDataSet("bounding_boxes");
        DataSpace dataspace = dataset.getSpace();

        // Get dimensions
        hsize_t dims[1];
        dataspace.getSimpleExtentDims(dims);
        size_t num_boxes = dims[0];

        if (num_boxes == 0) {
            return true; // Empty but valid
        }

        // Create compound type for reading
        CompType memtype(sizeof(LoggedBoundingBox));
        memtype.insertMember("payload_timestamp_ns_epoch", HOFFSET(LoggedBoundingBox, payload_timestamp_ns_epoch), PredType::NATIVE_INT64);
        memtype.insertMember("received_timestamp_ns_epoch", HOFFSET(LoggedBoundingBox, received_timestamp_ns_epoch), PredType::NATIVE_INT64);
        memtype.insertMember("payload_frame_id", HOFFSET(LoggedBoundingBox, payload_frame_id), PredType::NATIVE_UINT64);
        memtype.insertMember("payload_camera_id", HOFFSET(LoggedBoundingBox, payload_camera_id), PredType::NATIVE_UINT32);
        memtype.insertMember("box_index_in_payload", HOFFSET(LoggedBoundingBox, box_index_in_payload), PredType::NATIVE_UINT8);
        memtype.insertMember("x_min", HOFFSET(LoggedBoundingBox, x_min), PredType::NATIVE_FLOAT);
        memtype.insertMember("y_min", HOFFSET(LoggedBoundingBox, y_min), PredType::NATIVE_FLOAT);
        memtype.insertMember("width", HOFFSET(LoggedBoundingBox, width), PredType::NATIVE_FLOAT);
        memtype.insertMember("height", HOFFSET(LoggedBoundingBox, height), PredType::NATIVE_FLOAT);
        memtype.insertMember("class_id", HOFFSET(LoggedBoundingBox, class_id), PredType::NATIVE_UINT16);
        memtype.insertMember("confidence", HOFFSET(LoggedBoundingBox, confidence), PredType::NATIVE_FLOAT);

        // Read data
        boxes.resize(num_boxes);
        dataset.read(boxes.data(), memtype);

        return true;
    } catch (const H5::Exception& e) {
        std::cerr << "Error loading bounding boxes: " << e.getCDetailMsg() << std::endl;
        return false;
    }
}

bool H5SessionLoader::loadChaserStates(H5::H5File& file, std::vector<LoggedChaserState>& states) {
    try {
        Group tracking_group = file.openGroup("/tracking_data");

        if (!datasetExists(tracking_group, "chaser_states")) {
            return false;
        }

        DataSet dataset = tracking_group.openDataSet("chaser_states");
        DataSpace dataspace = dataset.getSpace();

        // Get dimensions
        hsize_t dims[1];
        dataspace.getSimpleExtentDims(dims);
        size_t num_states = dims[0];

        if (num_states == 0) {
            return true; // Empty but valid
        }

        // Create compound type for reading
        CompType memtype(sizeof(LoggedChaserState));
        memtype.insertMember("stimulus_frame_num", HOFFSET(LoggedChaserState, stimulus_frame_num), PredType::NATIVE_UINT64);
        memtype.insertMember("timestamp_ns_session", HOFFSET(LoggedChaserState, timestamp_ns_session), PredType::NATIVE_INT64);
        memtype.insertMember("chaser_index", HOFFSET(LoggedChaserState, chaser_index), PredType::NATIVE_UINT8);
        memtype.insertMember("is_chasing", HOFFSET(LoggedChaserState, is_chasing), PredType::NATIVE_HBOOL);
        memtype.insertMember("chaser_pos_x", HOFFSET(LoggedChaserState, chaser_pos_x), PredType::NATIVE_FLOAT);
        memtype.insertMember("chaser_pos_y", HOFFSET(LoggedChaserState, chaser_pos_y), PredType::NATIVE_FLOAT);
        memtype.insertMember("target_pos_x", HOFFSET(LoggedChaserState, target_pos_x), PredType::NATIVE_FLOAT);
        memtype.insertMember("target_pos_y", HOFFSET(LoggedChaserState, target_pos_y), PredType::NATIVE_FLOAT);

        // Read data
        states.resize(num_states);
        dataset.read(states.data(), memtype);

        return true;
    } catch (const H5::Exception& e) {
        std::cerr << "Error loading chaser states: " << e.getCDetailMsg() << std::endl;
        return false;
    }
}

bool H5SessionLoader::loadFrameMetadata(H5::H5File& file, std::vector<FrameMetadataRecord>& metadata) {
    try {
        Group video_group = file.openGroup("/video_metadata");

        if (!datasetExists(video_group, "frame_metadata")) {
            return false;
        }

        DataSet dataset = video_group.openDataSet("frame_metadata");
        DataSpace dataspace = dataset.getSpace();

        // Get dimensions
        hsize_t dims[1];
        dataspace.getSimpleExtentDims(dims);
        size_t num_records = dims[0];

        if (num_records == 0) {
            return true; // Empty but valid
        }

        // Create compound type for reading
        CompType memtype(sizeof(FrameMetadataRecord));
        memtype.insertMember("stimulus_frame_num", HOFFSET(FrameMetadataRecord, stimulus_frame_num), PredType::NATIVE_UINT64);
        memtype.insertMember("triggering_camera_frame_id", HOFFSET(FrameMetadataRecord, triggering_camera_frame_id), PredType::NATIVE_UINT64);
        memtype.insertMember("timestamp_ns", HOFFSET(FrameMetadataRecord, timestamp_ns), PredType::NATIVE_INT64);

        // Read data
        metadata.resize(num_records);
        dataset.read(metadata.data(), memtype);

        return true;
    } catch (const H5::Exception& e) {
        std::cerr << "Error loading frame metadata: " << e.getCDetailMsg() << std::endl;
        return false;
    }
}

bool H5SessionLoader::loadProtocolSnapshot(H5::H5File& file, std::string& protocol_json) {
    try {
        if (!groupExists(file, "/protocol_snapshot")) {
            return false;
        }

        Group protocolGroup = file.openGroup("/protocol_snapshot");

        if (datasetExists(protocolGroup, "protocol_definition_json")) {
            DataSet dataset = protocolGroup.openDataSet("protocol_definition_json");
            StrType strType(PredType::C_S1, H5T_VARIABLE);
            H5std_string h5_str;
            dataset.read(h5_str, strType);
            protocol_json = h5_str;
        }

        return true;
    } catch (const H5::Exception& e) {
        std::cerr << "Error loading protocol snapshot: " << e.getCDetailMsg() << std::endl;
        return false;
    }
}

bool H5SessionLoader::loadCalibrationSnapshot(H5::H5File& file, std::string& arena_config_json) {
    try {
        if (!groupExists(file, "/calibration_snapshot")) {
            std::cout << "[DEBUG] /calibration_snapshot group does not exist" << std::endl;
            return false;
        }
        
        std::cout << "[DEBUG] Found /calibration_snapshot group" << std::endl;
        
        Group calibGroup = file.openGroup("/calibration_snapshot");
        
        if (datasetExists(calibGroup, "arena_config_json")) {
            std::cout << "[DEBUG] Found arena_config_json dataset" << std::endl;
            DataSet dataset = calibGroup.openDataSet("arena_config_json");
            StrType strType(PredType::C_S1, H5T_VARIABLE);
            H5std_string h5_str;
            dataset.read(h5_str, strType);
            arena_config_json = h5_str;
            std::cout << "[DEBUG] Loaded arena_config_json, length: " << arena_config_json.length() << std::endl;
        } else {
            std::cout << "[DEBUG] arena_config_json dataset not found" << std::endl;
        }
        
        return true;
    } catch (const H5::Exception& e) {
        std::cerr << "Error loading calibration snapshot: " << e.getCDetailMsg() << std::endl;
        return false;
    }
}
// Helper functions
bool H5SessionLoader::groupExists(H5::H5File& file, const std::string& group_name) {
    try {
        htri_t exists = H5Lexists(file.getId(), group_name.c_str(), H5P_DEFAULT);
        return exists > 0;
    } catch (...) {
        return false;
    }
}

bool H5SessionLoader::datasetExists(H5::Group& group, const std::string& dataset_name) {
    try {
        return group.nameExists(dataset_name);
    } catch (...) {
        return false;
    }
}

void H5SessionLoader::setError(std::string& error_message, const std::string& msg) {
    error_message = msg;
    std::cerr << "H5SessionLoader Error: " << msg << std::endl;
}

// Static helper functions
std::string H5SessionLoader::findH5FileInDirectory(const std::string& directory) {
    try {
        std::vector<std::string> analysis_files;
        std::vector<std::string> regular_files;
        
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (entry.is_regular_file()) {
                std::string filepath = entry.path().string();
                if (isH5File(filepath)) {
                    // Check if it's an analysis file
                    if (filepath.find("_analysis.h5") != std::string::npos ||
                        filepath.find("_analysis.hdf5") != std::string::npos) {
                        analysis_files.push_back(filepath);
                    } else {
                        regular_files.push_back(filepath);
                    }
                }
            }
        }
        
        // Priority: 
        // 1. Analysis files (prefer these)
        // 2. Regular H5 files (fallback)
        if (!analysis_files.empty()) {
            if (analysis_files.size() > 1) {
                std::cout << "Warning: Multiple analysis files found, using: " 
                          << analysis_files[0] << std::endl;
            }
            return analysis_files[0];
        } else if (!regular_files.empty()) {
            std::cout << "No analysis file found, using regular H5 file: " 
                      << regular_files[0] << std::endl;
            return regular_files[0];
        }
        
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "Filesystem error: " << e.what() << std::endl;
    }

    return "";  // No H5 file found
}

bool H5SessionLoader::isH5File(const std::string& filepath) {
    std::filesystem::path path(filepath);
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return (ext == ".h5" || ext == ".hdf5" || ext == ".hdf");
}

FrameMetadataRecord* H5SessionLoader::getFrameMetadata(H5SessionData& data, uint64_t frame_num) {
    auto it = std::find_if(data.frame_metadata.begin(), data.frame_metadata.end(),
                          [frame_num](const FrameMetadataRecord& record) {
                              return record.stimulus_frame_num == frame_num;
                          });
    return (it != data.frame_metadata.end()) ? &(*it) : nullptr;
}

FrameMetadataRecord* H5SessionLoader::getFrameMetadataByCameraID(H5SessionData& data, uint64_t camera_frame_id) {
    static int call_count = 0;
    static uint64_t last_camera_frame = 0;
    static int consecutive_same_frame = 0;
    
    // Track if we're repeatedly looking up the same frame (sign of blinking)
    if (camera_frame_id == last_camera_frame) {
        consecutive_same_frame++;
    } else {
        if (kH5DebugLoggingEnabled && consecutive_same_frame > 1) {
            std::cout << "[H5_DEBUG] Frame " << last_camera_frame 
                      << " was looked up " << consecutive_same_frame + 1 
                      << " times in a row" << std::endl;
        }
        consecutive_same_frame = 0;
        last_camera_frame = camera_frame_id;
    }
    
    call_count++;
    
    // Log every 100th call or first 10 calls for debugging
    bool should_log = kH5DebugLoggingEnabled && ((call_count <= 10) || (call_count % 100 == 0));
    
    if (should_log) {
        std::cout << "\n[H5_LOOKUP] Call #" << call_count 
                  << " - Looking up camera frame: " << camera_frame_id << std::endl;
    }
    
    // First try the optimized lookup for continuous frames (analysis files)
    if (data.has_continuous_frames && !data.frame_metadata.empty()) {
        uint64_t min_frame_id = data.frame_metadata.front().triggering_camera_frame_id;
        uint64_t max_frame_id = data.frame_metadata.back().triggering_camera_frame_id;
        
        if (should_log) {
            std::cout << "  [H5_DEBUG] Using CONTINUOUS mode (analysis file)" << std::endl;
            std::cout << "  [H5_DEBUG] Available range: " << min_frame_id << " - " << max_frame_id << std::endl;
        }

        // Check bounds
        if (camera_frame_id < min_frame_id || camera_frame_id > max_frame_id) {
            if (should_log) {
                std::cout << "  [H5_DEBUG] ❌ Frame " << camera_frame_id << " is OUT OF RANGE" << std::endl;
            }
            return nullptr;
        }
        
        if (camera_frame_id >= min_frame_id && camera_frame_id <= max_frame_id) {
            // In analysis files with continuous frames, the index should map directly
            size_t index = camera_frame_id - min_frame_id;
            
            if (should_log) {
                std::cout << "  [H5_DEBUG] Calculated index: " << index 
                          << " (frame " << camera_frame_id << " - min " << min_frame_id << ")" << std::endl;
            }
            
            if (index < data.frame_metadata.size()) {
                // Verify this is the correct frame (in case of any indexing issues)
                uint64_t found_camera_id = data.frame_metadata[index].triggering_camera_frame_id;
                uint64_t found_stim_id = data.frame_metadata[index].stimulus_frame_num;
                
                if (found_camera_id == camera_frame_id) {
                    if (should_log) {
                        std::cout << "  [H5_DEBUG] ✅ Direct index SUCCESS!" << std::endl;
                        std::cout << "  [H5_DEBUG] Found: camera=" << found_camera_id 
                                  << " -> stimulus=" << found_stim_id << std::endl;
                    }
                    return &data.frame_metadata[index];
                } else {
                    if (should_log) {
                        std::cout << "  [H5_DEBUG] ⚠️  Direct index MISMATCH!" << std::endl;
                        std::cout << "  [H5_DEBUG] Expected camera=" << camera_frame_id 
                                  << " but found camera=" << found_camera_id << std::endl;
                        std::cout << "  [H5_DEBUG] Falling back to linear search..." << std::endl;
                    }
                }
            } else {
                if (should_log) {
                    std::cout << "  [H5_DEBUG] ⚠️  Index " << index << " >= size " 
                              << data.frame_metadata.size() << std::endl;
                }
            }
        }
    } else {
        if (should_log) {
            std::cout << "  [H5_DEBUG] Using LINEAR SEARCH mode (non-continuous or regular file)" << std::endl;
            std::cout << "  [H5_DEBUG] has_continuous_frames=" << data.has_continuous_frames 
                      << ", metadata_size=" << data.frame_metadata.size() << std::endl;
        }
    }
    
    // Fall back to linear search for non-continuous frames or if direct lookup failed
    if (should_log) {
        std::cout << "  [H5_DEBUG] Starting linear search through " 
                  << data.frame_metadata.size() << " records..." << std::endl;
    }
    
    // Count ALL matches to detect multiple stimulus frames per camera frame
    std::vector<size_t> matching_indices;
    for (size_t i = 0; i < data.frame_metadata.size(); ++i) {
        if (data.frame_metadata[i].triggering_camera_frame_id == camera_frame_id) {
            matching_indices.push_back(i);
        }
    }
    
    if (matching_indices.empty()) {
        static int not_found_counter = 0;
        if (not_found_counter++ < 10) {
            std::cerr << "[H5_WARNING] Frame metadata not found for camera_frame_id: " << camera_frame_id << std::endl;
            if (!data.frame_metadata.empty()) {
                std::cerr << "  Available range: " 
                          << data.frame_metadata.front().triggering_camera_frame_id 
                          << " - " 
                          << data.frame_metadata.back().triggering_camera_frame_id << std::endl;
            }
        }
        return nullptr;
    }
    
    // Log if we found multiple matches (this causes blinking!)
    if (matching_indices.size() > 1) {
        if (kH5DebugLoggingEnabled && (should_log || matching_indices.size() > 2)) {  // Always log if more than 2
            std::cout << "  [H5_DEBUG] ⚠️  Found " << matching_indices.size() 
                      << " MULTIPLE matches for camera frame " << camera_frame_id << "!" << std::endl;
            
            for (size_t i = 0; i < std::min(matching_indices.size(), size_t(3)); ++i) {
                size_t idx = matching_indices[i];
                std::cout << "    Match " << i+1 << ": index=" << idx 
                          << ", stimulus=" << data.frame_metadata[idx].stimulus_frame_num 
                          << ", camera=" << data.frame_metadata[idx].triggering_camera_frame_id << std::endl;
            }
            
            std::cout << "  [H5_DEBUG] 🎲 Returning FIRST match (index=" << matching_indices[0] 
                      << ", stimulus=" << data.frame_metadata[matching_indices[0]].stimulus_frame_num << ")" << std::endl;
        }
    } else if (should_log) {
        size_t idx = matching_indices[0];
        std::cout << "  [H5_DEBUG] ✅ Found single match: index=" << idx 
                  << ", stimulus=" << data.frame_metadata[idx].stimulus_frame_num << std::endl;
    }
    
    // Return the first match (this may not be consistent if order changes!)
    return &data.frame_metadata[matching_indices[0]];
}

std::vector<LoggedBoundingBox> H5SessionLoader::getBoundingBoxesForFrame(const H5SessionData& data, uint64_t frame_id) {
    std::vector<LoggedBoundingBox> result;
    for (const auto& box : data.bounding_boxes) {
        if (box.payload_frame_id == frame_id) {
            result.push_back(box);
        }
    }
    return result;
}

std::vector<LoggedChaserState> H5SessionLoader::getChaserStatesForFrame(const H5SessionData& data, uint64_t frame_num) {
    static int chaser_call_count = 0;
    chaser_call_count++;
    
    bool should_log = kH5DebugLoggingEnabled && ((chaser_call_count <= 10) || (chaser_call_count % 100 == 0));
    
    std::vector<LoggedChaserState> result;
    for (const auto& state : data.chaser_states) {
        if (state.stimulus_frame_num == frame_num) {
            result.push_back(state);
        }
    }
    
    if (should_log) {
        std::cout << "[H5_CHASER] Call #" << chaser_call_count 
                  << " - Stimulus frame " << frame_num 
                  << " -> Found " << result.size() << " chaser state(s)";
        
        if (!result.empty()) {
            const auto& first = result[0];
            std::cout << " [Chaser at (" << first.chaser_pos_x << ", " << first.chaser_pos_y 
                      << "), Target at (" << first.target_pos_x << ", " << first.target_pos_y << ")]";
        }
        std::cout << std::endl;
    }
    
    // Warn if multiple chaser states for same stimulus frame
    if (kH5DebugLoggingEnabled && result.size() > 1) {
        std::cout << "  [H5_CHASER] ⚠️  WARNING: Multiple chaser states (" << result.size() 
                  << ") for stimulus frame " << frame_num << "!" << std::endl;
    }
    
    return result;
}

// Integration function
bool loadH5SessionFromDirectory(const std::string& video_directory,
                                H5SessionData& h5_data,
                                std::string& error_message) {
    // Find H5 file in the directory
    std::string h5_filepath = H5SessionLoader::findH5FileInDirectory(video_directory);

    if (h5_filepath.empty()) {
        // Not an error - H5 files are optional
        return false;
    }

    std::cout << "Found H5 session file: " << h5_filepath << std::endl;

    // Load the H5 file
    H5SessionLoader loader;
    return loader.loadH5File(h5_filepath, h5_data, error_message);
}

bool H5SessionLoader::loadCalibrationSnapshotEnhanced(H5::H5File& file, 
                                                      std::string& arena_config_json,
                                                      std::map<std::string, CameraCalibrationData>& camera_calibrations) {
    try {
        if (!groupExists(file, "/calibration_snapshot")) {
            return false;
        }

        Group calibGroup = file.openGroup("/calibration_snapshot");

        // Load the arena config JSON (existing functionality)
        if (datasetExists(calibGroup, "arena_config_json")) {
            DataSet dataset = calibGroup.openDataSet("arena_config_json");
            StrType strType(PredType::C_S1, H5T_VARIABLE);
            H5std_string h5_str;
            dataset.read(h5_str, strType);
            arena_config_json = h5_str;
        }

        // Now iterate through all camera groups
        hsize_t num_objs = calibGroup.getNumObjs();
        for (hsize_t i = 0; i < num_objs; i++) {
            H5G_obj_t obj_type = calibGroup.getObjTypeByIdx(i);
            
            if (obj_type == H5G_GROUP) {
                std::string obj_name = calibGroup.getObjnameByIdx(i);
                
                // Skip non-camera groups (like if there's metadata group)
                if (obj_name.find("Camera") != std::string::npos || 
                    obj_name.find("camera") != std::string::npos ||
                    std::isdigit(obj_name[0])) {  // Handle numeric camera IDs
                    
                    std::cout << "Loading calibration for camera: " << obj_name << std::endl;
                    
                    Group camera_group = calibGroup.openGroup(obj_name);
                    CameraCalibrationData calib_data;
                    calib_data.camera_id = obj_name;
                    
                    // Load homography YAML
                    if (loadCameraHomographyYAML(camera_group, calib_data)) {
                        std::cout << "  - Loaded homography matrix from YAML" << std::endl;
                    }
                    
                    // Load calibration attributes
                    if (loadCameraCalibrationAttributes(camera_group, calib_data)) {
                        std::cout << "  - Loaded calibration attributes" << std::endl;
                    }
                    
                    // Load calibration images (optional, can be memory intensive)
                    // Uncomment if needed:
                    // if (loadCameraCalibrationImages(camera_group, calib_data)) {
                    //     std::cout << "  - Loaded calibration images" << std::endl;
                    // }
                    
                    camera_calibrations[obj_name] = calib_data;
                    camera_group.close();
                }
            }
        }

        calibGroup.close();
        return true;
        
    } catch (const H5::Exception& e) {
        std::cerr << "Error loading enhanced calibration snapshot: " << e.getCDetailMsg() << std::endl;
        return false;
    }
}

bool H5SessionLoader::loadCameraHomographyYAML(H5::Group& camera_group,
                                               CameraCalibrationData& calib_data) {
    try {
        if (!datasetExists(camera_group, "homography_matrix_yml")) {
            return false;
        }
        
        DataSet dataset = camera_group.openDataSet("homography_matrix_yml");
        
        // Read the YAML string
        StrType strType = dataset.getStrType();
        H5std_string yaml_content;
        dataset.read(yaml_content, strType);
        
        // Parse the YAML to extract homography matrix
        if (parseHomographyYAML(yaml_content, 
                               calib_data.homography_matrix,
                               calib_data.calibration_timestamp_utc)) {
            calib_data.has_homography = true;
            dataset.close();
            return true;
        }
        
        dataset.close();
        
    } catch (const H5::Exception& e) {
        std::cerr << "Error loading homography YAML: " << e.getCDetailMsg() << std::endl;
    }
    return false;
}

bool H5SessionLoader::loadCameraCalibrationAttributes(H5::Group& camera_group,
                                                      CameraCalibrationData& calib_data) {
    try {
        bool found_any = false;
        
        // Read float attributes
        if (camera_group.attrExists("pixels_per_mm_projector")) {
            Attribute attr = camera_group.openAttribute("pixels_per_mm_projector");
            attr.read(PredType::NATIVE_FLOAT, &calib_data.pixels_per_mm_projector);
            found_any = true;
        }
        
        if (camera_group.attrExists("pixels_per_mm_camera")) {
            Attribute attr = camera_group.openAttribute("pixels_per_mm_camera");
            attr.read(PredType::NATIVE_FLOAT, &calib_data.pixels_per_mm_camera);
            found_any = true;
        }
        
        if (camera_group.attrExists("real_world_ref_mm")) {
            Attribute attr = camera_group.openAttribute("real_world_ref_mm");
            attr.read(PredType::NATIVE_FLOAT, &calib_data.real_world_ref_mm);
            found_any = true;
        }
        
        calib_data.has_attributes = found_any;
        return found_any;
        
    } catch (const H5::Exception& e) {
        std::cerr << "Error loading calibration attributes: " << e.getCDetailMsg() << std::endl;
    }
    return false;
}

bool H5SessionLoader::loadCameraCalibrationImages(H5::Group& camera_group,
                                                  CameraCalibrationData& calib_data) {
    try {
        bool loaded_any = false;
        
        // Load homography image
        if (datasetExists(camera_group, "homography_image_png_buffer")) {
            DataSet dataset = camera_group.openDataSet("homography_image_png_buffer");
            DataSpace dataspace = dataset.getSpace();
            
            hsize_t dims[1];
            dataspace.getSimpleExtentDims(dims);
            
            calib_data.homography_image_png.resize(dims[0]);
            dataset.read(calib_data.homography_image_png.data(), PredType::NATIVE_UINT8);
            dataset.close();
            loaded_any = true;
        }
        
        // Load scale image
        if (datasetExists(camera_group, "scale_image_png_buffer")) {
            DataSet dataset = camera_group.openDataSet("scale_image_png_buffer");
            DataSpace dataspace = dataset.getSpace();
            
            hsize_t dims[1];
            dataspace.getSimpleExtentDims(dims);
            
            calib_data.scale_image_png.resize(dims[0]);
            dataset.read(calib_data.scale_image_png.data(), PredType::NATIVE_UINT8);
            dataset.close();
            loaded_any = true;
        }
        
        return loaded_any;
        
    } catch (const H5::Exception& e) {
        std::cerr << "Error loading calibration images: " << e.getCDetailMsg() << std::endl;
    }
    return false;
}

bool H5SessionLoader::parseHomographyYAML(const std::string& yaml_content,
                                          cv::Mat& homography_matrix,
                                          std::string& timestamp) {
    try {
        // Use OpenCV's FileStorage with MEMORY flag to parse YAML string
        cv::FileStorage fs(yaml_content, cv::FileStorage::READ | cv::FileStorage::MEMORY);
        
        if (!fs.isOpened()) {
            std::cerr << "Failed to parse YAML content" << std::endl;
            return false;
        }
        
        // Extract timestamp if present
        if (!fs["calibration_timestamp_utc"].empty()) {
            fs["calibration_timestamp_utc"] >> timestamp;
        }
        
        // Extract homography matrix
        if (!fs["homography_matrix"].empty()) {
            fs["homography_matrix"] >> homography_matrix;
            
            // Verify it's a 3x3 matrix
            if (homography_matrix.rows == 3 && homography_matrix.cols == 3) {
                fs.release();
                return true;
            } else {
                std::cerr << "Invalid homography matrix dimensions: " 
                         << homography_matrix.rows << "x" << homography_matrix.cols << std::endl;
            }
        }
        
        fs.release();
        
    } catch (const cv::Exception& e) {
        std::cerr << "OpenCV error parsing YAML: " << e.what() << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error parsing homography YAML: " << e.what() << std::endl;
    }
    
    return false;
}

bool H5SessionLoader::isAnalysisFile(const std::string& filepath) {
    // Check if filename contains "analysis" or if the file has /analysis group
    std::filesystem::path path(filepath);
    std::string filename = path.filename().string();
    std::transform(filename.begin(), filename.end(), filename.begin(), ::tolower);
    
    return (filename.find("analysis") != std::string::npos) ||
           (filename.find("out_analysis") != std::string::npos);
}

bool H5SessionLoader::loadAnalysisData(H5::H5File& file, H5SessionData& data) {
    try {
        if (!groupExists(file, "/analysis")) {
            return false;
        }
        
        Group analysisGroup = file.openGroup("/analysis");
        
        // Load interpolation mask
        if (!loadInterpolationMask(file, data.interpolation_mask)) {
            std::cerr << "  Warning: Failed to load interpolation mask" << std::endl;
        } else {
            std::cout << "  Loaded interpolation mask: " << data.interpolation_mask.size() << " entries" << std::endl;
        }
        
        // Load gap info JSON
        if (!loadGapInfo(file, data.gap_info_json)) {
            std::cerr << "  Warning: Failed to load gap info" << std::endl;
        } else {
            std::cout << "  Loaded gap info JSON" << std::endl;
        }
        
        if (!data.interpolation_mask.empty()) {
            std::cout << "  Analysis statistics:" << std::endl;
            std::cout << "    Original frames: " << data.getOriginalFrameCount() << std::endl;
            std::cout << "    Interpolated frames: " << data.getInterpolatedFrameCount() << std::endl;
        }
        
        return true;
    } catch (const H5::Exception& e) {
        std::cerr << "Error loading analysis data: " << e.getCDetailMsg() << std::endl;
        return false;
    }
}

bool H5SessionLoader::loadInterpolationMask(H5::H5File& file, std::vector<bool>& mask) {
    try {
        Group analysisGroup = file.openGroup("/analysis");
        
        if (!datasetExists(analysisGroup, "interpolation_mask")) {
            return false;
        }
        
        DataSet dataset = analysisGroup.openDataSet("interpolation_mask");
        DataSpace dataspace = dataset.getSpace();
        
        // Get dimensions
        hsize_t dims[1];
        dataspace.getSimpleExtentDims(dims);
        size_t num_elements = dims[0];
        
        if (num_elements == 0) {
            return true;  // Empty but valid
        }
        
        // Read as uint8 array first (HDF5 stores booleans as uint8)
        std::vector<uint8_t> temp_mask(num_elements);
        dataset.read(temp_mask.data(), PredType::NATIVE_UINT8);
        
        // Convert to bool vector
        mask.resize(num_elements);
        for (size_t i = 0; i < num_elements; ++i) {
            mask[i] = (temp_mask[i] != 0);
        }
        
        return true;
    } catch (const H5::Exception& e) {
        std::cerr << "Error loading interpolation mask: " << e.getCDetailMsg() << std::endl;
        return false;
    }
}

bool H5SessionLoader::loadGapInfo(H5::H5File& file, std::string& gap_info) {
    try {
        Group analysisGroup = file.openGroup("/analysis");
        
        if (!datasetExists(analysisGroup, "gap_info")) {
            return false;
        }
        
        DataSet dataset = analysisGroup.openDataSet("gap_info");
        StrType strType(PredType::C_S1, H5T_VARIABLE);
        H5std_string h5_str;
        dataset.read(h5_str, strType);
        gap_info = h5_str;
        
        return true;
    } catch (const H5::Exception& e) {
        std::cerr << "Error loading gap info: " << e.getCDetailMsg() << std::endl;
        return false;
    }
}

// Enhanced loadH5File method that detects and handles analysis files
bool H5SessionLoader::loadH5File(const std::string& filepath, H5SessionData& data, std::string& error_message) {
    try {
        // ============================================
        // STEP 1: Validate and open file
        // ============================================
        if (!std::filesystem::exists(filepath)) {
            setError(error_message, "H5 file does not exist: " + filepath);
            return false;
        }
        
        H5File file(filepath, H5F_ACC_RDONLY);
        
        // ============================================
        // STEP 2: Determine file type (analysis vs regular)
        // ============================================
        std::filesystem::path path(filepath);
        std::string filename = path.filename().string();
        std::transform(filename.begin(), filename.end(), filename.begin(), ::tolower);
        
        // Check filename for "analysis" keyword
        bool filename_suggests_analysis = (filename.find("analysis") != std::string::npos) ||
                                         (filename.find("out_analysis") != std::string::npos);
        
        // Check for /analysis group existence
        bool has_analysis_group = groupExists(file, "/analysis");
        
        // File is analysis if EITHER filename suggests it OR it has /analysis group
        data.is_analysis_file = filename_suggests_analysis || has_analysis_group;
        
        std::cout << "Loading H5 file: " << filepath << std::endl;
        std::cout << "  File type: " << (data.is_analysis_file ? "ANALYSIS" : "REGULAR") << std::endl;
        std::cout << "  Filename suggests analysis: " << filename_suggests_analysis << std::endl;
        std::cout << "  Has /analysis group: " << has_analysis_group << std::endl;
        
        // ============================================
        // STEP 3: Load core session data
        // ============================================
        
        // Load session info (attributes from root group)
        if (!loadSessionInfo(file, data.session_info)) {
            std::cerr << "Warning: Failed to load complete session info" << std::endl;
        }
        
        // Load events
        if (!loadEvents(file, data.events)) {
            std::cerr << "Warning: Failed to load events" << std::endl;
        } else {
            std::cout << "  Loaded " << data.events.size() << " events" << std::endl;
        }
        
        // ============================================
        // STEP 4: Load tracking data
        // ============================================
        if (groupExists(file, "/tracking_data")) {
            data.has_tracking_data = true;
            
            if (!loadBoundingBoxes(file, data.bounding_boxes)) {
                std::cerr << "Warning: Failed to load bounding boxes" << std::endl;
            } else {
                std::cout << "  Loaded " << data.bounding_boxes.size() << " bounding boxes" << std::endl;
            }
            
            if (!loadChaserStates(file, data.chaser_states)) {
                std::cerr << "Warning: Failed to load chaser states" << std::endl;
            } else {
                std::cout << "  Loaded " << data.chaser_states.size() << " chaser states" << std::endl;
            }
        }
        
        // ============================================
        // STEP 5: Load video metadata and determine continuity
        // ============================================
        if (groupExists(file, "/video_metadata")) {
            data.has_video_metadata = true;
            
            if (!loadFrameMetadata(file, data.frame_metadata)) {
                std::cerr << "Warning: Failed to load frame metadata" << std::endl;
            } else {
                std::cout << "  Loaded " << data.frame_metadata.size() << " frame metadata records" << std::endl;
                
                // Determine if frames are continuous
                // This is based on the actual data, not just file type
                if (!data.frame_metadata.empty()) {
                    uint64_t min_camera_id = data.frame_metadata.front().triggering_camera_frame_id;
                    uint64_t max_camera_id = data.frame_metadata.back().triggering_camera_frame_id;
                    size_t expected_count = max_camera_id - min_camera_id + 1;
                    
                    // Check if we have exactly one record per camera frame in the range
                    bool perfectly_continuous = (data.frame_metadata.size() == expected_count);
                    
                    // For analysis files, we expect continuous frames (but may have multiple stim per camera)
                    // For regular files, we don't expect continuity
                    if (data.is_analysis_file) {
                        // Analysis files should have no gaps in camera frames
                        // But may have multiple stimulus frames per camera frame
                        // So we need to check uniqueness of camera frames
                        std::set<uint64_t> unique_camera_frames;
                        for (const auto& record : data.frame_metadata) {
                            unique_camera_frames.insert(record.triggering_camera_frame_id);
                        }
                        
                        size_t unique_camera_count = unique_camera_frames.size();
                        size_t expected_unique = max_camera_id - min_camera_id + 1;
                        
                        // Continuous if we have all camera frames in the range (even with duplicates)
                        data.has_continuous_frames = (unique_camera_count == expected_unique);
                        
                        std::cout << "  Frame continuity analysis:" << std::endl;
                        std::cout << "    Camera frame range: " << min_camera_id << " - " << max_camera_id << std::endl;
                        std::cout << "    Total metadata records: " << data.frame_metadata.size() << std::endl;
                        std::cout << "    Unique camera frames: " << unique_camera_count << std::endl;
                        std::cout << "    Expected camera frames: " << expected_unique << std::endl;
                        std::cout << "    Continuous: " << (data.has_continuous_frames ? "YES" : "NO") << std::endl;
                        
                        if (data.frame_metadata.size() > unique_camera_count) {
                            std::cout << "    Note: Multiple stimulus frames per camera frame detected" << std::endl;
                            std::cout << "          (avg " << std::fixed << std::setprecision(2) 
                                      << (double)data.frame_metadata.size() / unique_camera_count 
                                      << " stim/camera)" << std::endl;
                        }
                    } else {
                        // Regular files typically have gaps, don't assume continuity
                        data.has_continuous_frames = perfectly_continuous;
                        
                        if (!data.has_continuous_frames) {
                            std::cout << "  Frame metadata has gaps (normal for regular files)" << std::endl;
                        }
                    }
                    
                    // Calculate total frames and FPS
                    data.total_frames = data.frame_metadata.back().stimulus_frame_num + 1;
                    
                    if (data.frame_metadata.size() > 10) {
                        double time_diff = (data.frame_metadata.back().timestamp_ns -
                                          data.frame_metadata.front().timestamp_ns) / 1e9;
                        double frame_diff = data.frame_metadata.size() - 1;
                        data.fps = frame_diff / time_diff;
                    }
                }
            }
        }
        
        // ============================================
        // STEP 6: Load analysis-specific data (if present)
        // ============================================
        if (has_analysis_group) {
            std::cout << "\nLoading analysis-specific data:" << std::endl;
            
            if (loadAnalysisData(file, data)) {
                std::cout << "  ✅ Successfully loaded analysis data" << std::endl;
                
                // loadAnalysisData sets is_analysis_file and has_continuous_frames
                // But let's verify it matches our expectations
                if (!data.has_continuous_frames) {
                    std::cerr << "  ⚠️  WARNING: Analysis file but frames not continuous!" << std::endl;
                }
            } else {
                std::cerr << "  ⚠️  Failed to load analysis data completely" << std::endl;
            }
        }
        
        // ============================================
        // STEP 7: Load calibration and protocol data
        // ============================================
        loadProtocolSnapshot(file, data.protocol_json);
        loadCalibrationSnapshot(file, data.arena_config_json);
        loadCalibrationSnapshotEnhanced(file, data.arena_config_json, data.camera_calibrations);
        
        // Try to load homography directly from /homography dataset (for analysis files)
        if (loadHomographyDirect(file, data.direct_homography_matrix)) {
            data.has_direct_homography = true;
            std::cout << "  Found direct homography matrix at /homography" << std::endl;
        }
        
        file.close();
        
        // ============================================
        // STEP 8: Final summary
        // ============================================
        std::cout << "\n" << std::string(60, '=') << std::endl;
        std::cout << "H5 FILE LOADING SUMMARY" << std::endl;
        std::cout << std::string(60, '=') << std::endl;
        std::cout << "File type: " << (data.is_analysis_file ? "ANALYSIS" : "REGULAR") << std::endl;
        std::cout << "Total stimulus frames: " << data.total_frames << std::endl;
        std::cout << "Frame metadata records: " << data.frame_metadata.size() << std::endl;
        std::cout << "Chaser state records: " << data.chaser_states.size() << std::endl;
        std::cout << "Estimated FPS: " << std::fixed << std::setprecision(2) << data.fps << std::endl;
        std::cout << "Frame coverage: " << (data.has_continuous_frames ? "CONTINUOUS" : "HAS GAPS") << std::endl;
        
        if (data.is_analysis_file && !data.interpolation_mask.empty()) {
            std::cout << "Interpolation data: " << std::endl;
            std::cout << "  Original frames: " << data.getOriginalFrameCount() << std::endl;
            std::cout << "  Interpolated frames: " << data.getInterpolatedFrameCount() << std::endl;
        }
        
        std::cout << std::string(60, '=') << std::endl;
        
        return true;
        
    } catch (const H5::Exception& error) {
        setError(error_message, "HDF5 error: " + std::string(error.getCDetailMsg()));
        return false;
    } catch (const std::exception& e) {
        setError(error_message, "Exception: " + std::string(e.what()));
        return false;
    }
}



// New helper method for continuous frame access
FrameMetadataRecord* H5SessionLoader::getFrameMetadataByCameraIDContinuous(
    H5SessionData& data, uint64_t camera_frame_id) {
    
    if (data.has_continuous_frames && !data.frame_metadata.empty()) {
        // For continuous frames, we can directly calculate the index
        uint64_t min_frame_id = data.frame_metadata.front().triggering_camera_frame_id;
        uint64_t max_frame_id = data.frame_metadata.back().triggering_camera_frame_id;
        
        if (camera_frame_id >= min_frame_id && camera_frame_id <= max_frame_id) {
            size_t index = camera_frame_id - min_frame_id;
            if (index < data.frame_metadata.size()) {
                return &data.frame_metadata[index];
            }
        }
    }
    
    // Fall back to linear search for non-continuous frames
    return getFrameMetadataByCameraID(data, camera_frame_id);
}


bool H5SessionLoader::loadHomographyDirect(H5::H5File& file, cv::Mat& homography_matrix) {
    try {
        // Check if /homography dataset exists (as saved by the Python analysis script)
        if (!H5Lexists(file.getId(), "/homography", H5P_DEFAULT)) {
            return false;
        }
        
        DataSet dataset = file.openDataSet("/homography");
        DataSpace dataspace = dataset.getSpace();
        
        // Get dimensions - should be 3x3
        hsize_t dims[2];
        int ndims = dataspace.getSimpleExtentDims(dims);
        
        if (ndims != 2 || dims[0] != 3 || dims[1] != 3) {
            std::cerr << "Invalid homography dimensions: " << dims[0] << "x" << dims[1] << std::endl;
            return false;
        }
        
        // Read the data as double (float64)
        double data[9];
        dataset.read(data, PredType::NATIVE_DOUBLE);
        
        // Convert to cv::Mat
        homography_matrix = cv::Mat(3, 3, CV_64F, data).clone();
        
        std::cout << "  Loaded homography directly from /homography dataset" << std::endl;
        std::cout << "  Homography matrix:\n" << homography_matrix << std::endl;
        
        return true;
        
    } catch (const H5::Exception& e) {
        std::cerr << "Error loading homography directly: " << e.getCDetailMsg() << std::endl;
        return false;
    }
}
