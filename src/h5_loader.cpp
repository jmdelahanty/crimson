#include "h5_loader.h"
#include <iostream>
#include <algorithm>
#include <cstring>

using namespace H5;

H5SessionLoader::H5SessionLoader() {
    // Turn off HDF5 error printing by default
    H5::Exception::dontPrint();
}

H5SessionLoader::~H5SessionLoader() {
}

bool H5SessionLoader::loadH5File(const std::string& filepath, H5SessionData& data, std::string& error_message) {
    try {
        // Check if file exists
        if (!std::filesystem::exists(filepath)) {
            setError(error_message, "H5 file does not exist: " + filepath);
            return false;
        }

        // Open the HDF5 file
        H5File file(filepath, H5F_ACC_RDONLY);

        std::cout << "Loading H5 session file: " << filepath << std::endl;

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

        // Load tracking data if it exists
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

        // Load video metadata if it exists
        if (groupExists(file, "/video_metadata")) {
            data.has_video_metadata = true;

            if (!loadFrameMetadata(file, data.frame_metadata)) {
                std::cerr << "Warning: Failed to load frame metadata" << std::endl;
            } else {
                std::cout << "  Loaded " << data.frame_metadata.size() << " frame metadata records" << std::endl;

                // Calculate total frames and FPS
                if (!data.frame_metadata.empty()) {
                    data.total_frames = data.frame_metadata.back().stimulus_frame_num + 1;

                    // Calculate FPS from timestamps if we have enough frames
                    if (data.frame_metadata.size() > 10) {
                        double time_diff = (data.frame_metadata.back().timestamp_ns -
                                          data.frame_metadata.front().timestamp_ns) / 1e9;
                        double frame_diff = data.frame_metadata.back().stimulus_frame_num -
                                          data.frame_metadata.front().stimulus_frame_num;
                        if (time_diff > 0) {
                            data.fps = frame_diff / time_diff;
                        }
                    }
                }
            }
        }

        // Load protocol snapshot
        if (!loadProtocolSnapshot(file, data.protocol_json)) {
            std::cerr << "Warning: Failed to load protocol snapshot" << std::endl;
        }

        // Load calibration snapshot
        if (!loadCalibrationSnapshot(file, data.arena_config_json)) {
            std::cerr << "Warning: Failed to load calibration snapshot" << std::endl;
        }

        file.close();

        std::cout << "Successfully loaded H5 session file" << std::endl;
        std::cout << "  Total frames: " << data.total_frames << std::endl;
        std::cout << "  Estimated FPS: " << data.fps << std::endl;

        return true;

    } catch (const H5::Exception& error) {
        setError(error_message, "HDF5 error: " + std::string(error.getCDetailMsg()));
        return false;
    } catch (const std::exception& e) {
        setError(error_message, "Exception: " + std::string(e.what()));
        return false;
    }
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
            return false;
        }

        Group calibGroup = file.openGroup("/calibration_snapshot");

        if (datasetExists(calibGroup, "arena_config_json")) {
            DataSet dataset = calibGroup.openDataSet("arena_config_json");
            StrType strType(PredType::C_S1, H5T_VARIABLE);
            H5std_string h5_str;
            dataset.read(h5_str, strType);
            arena_config_json = h5_str;
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
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (entry.is_regular_file()) {
                std::string filepath = entry.path().string();
                if (isH5File(filepath)) {
                    return filepath;
                }
            }
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
    auto it = std::find_if(data.frame_metadata.begin(), data.frame_metadata.end(),
                          [camera_frame_id](const FrameMetadataRecord& record) {
                              return record.triggering_camera_frame_id == camera_frame_id;
                          });
    return (it != data.frame_metadata.end()) ? &(*it) : nullptr;
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
    std::vector<LoggedChaserState> result;
    for (const auto& state : data.chaser_states) {
        if (state.stimulus_frame_num == frame_num) {
            result.push_back(state);
        }
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