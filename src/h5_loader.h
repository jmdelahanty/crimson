#ifndef H5_LOADER_H
#define H5_LOADER_H

#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <chrono>
#include <H5Cpp.h>

// Mirror the structures from logging_structs.h
struct SessionInfo {
    std::string session_uuid;
    std::string session_start_iso8601_utc;
    int64_t session_start_ns_epoch;
    std::string loaded_protocol_filepath;
    std::string protocol_name_from_definition;
    std::string active_ipc_source;
    std::string ipc_source_name;
    int stimulus_output_width = 0;
    int stimulus_output_height = 0;
    std::string hostname;
    std::string rig_id;
    std::string arena_id;
    std::string software_version;
    std::string git_commit_hash;
    std::map<std::string, std::string> subject_metadata;
    std::string operator_notes;
    std::vector<std::string> associated_camera_ids;
};

struct EventLogEntry {
    int64_t timestamp_ns_epoch;
    int64_t timestamp_ns_session;
    int32_t event_type_id;
    int32_t current_step_index;
    char name_or_context[256];
    int32_t stimulus_mode_id;
    char details_json[1024];

    EventLogEntry() {
        name_or_context[0] = '\0';
        details_json[0] = '\0';
    }
};

struct FrameMetadataRecord {
    uint64_t stimulus_frame_num;
    uint64_t triggering_camera_frame_id;
    int64_t timestamp_ns;
};

struct LoggedBoundingBox {
    int64_t payload_timestamp_ns_epoch;
    int64_t received_timestamp_ns_epoch;
    uint64_t payload_frame_id;
    uint32_t payload_camera_id;
    uint8_t box_index_in_payload;
    float x_min;
    float y_min;
    float width;
    float height;
    uint16_t class_id;
    float confidence;
};

struct LoggedChaserState {
    uint64_t stimulus_frame_num;
    int64_t timestamp_ns_session;
    uint8_t chaser_index;
    bool is_chasing;
    float chaser_pos_x;
    float chaser_pos_y;
    float target_pos_x;
    float target_pos_y;
};

// Main H5 data structure containing all loaded data
struct H5SessionData {
    // Session metadata
    SessionInfo session_info;

    // Protocol and calibration snapshots (as JSON strings)
    std::string protocol_json;
    std::string arena_config_json;

    // Event log
    std::vector<EventLogEntry> events;

    // Tracking data
    std::vector<LoggedBoundingBox> bounding_boxes;
    std::vector<LoggedChaserState> chaser_states;

    // Video metadata
    std::vector<FrameMetadataRecord> frame_metadata;

    // Helper data
    bool has_tracking_data = false;
    bool has_video_metadata = false;
    size_t total_frames = 0;
    double fps = 30.0; // Will be calculated from frame metadata if available
};

class H5SessionLoader {
public:
    H5SessionLoader();
    ~H5SessionLoader();

    // Main loading function - loads everything from the H5 file
    bool loadH5File(const std::string& filepath, H5SessionData& data, std::string& error_message);

    // Load specific components
    bool loadSessionInfo(H5::H5File& file, SessionInfo& info);
    bool loadEvents(H5::H5File& file, std::vector<EventLogEntry>& events);
    bool loadBoundingBoxes(H5::H5File& file, std::vector<LoggedBoundingBox>& boxes);
    bool loadChaserStates(H5::H5File& file, std::vector<LoggedChaserState>& states);
    bool loadFrameMetadata(H5::H5File& file, std::vector<FrameMetadataRecord>& metadata);
    bool loadProtocolSnapshot(H5::H5File& file, std::string& protocol_json);
    bool loadCalibrationSnapshot(H5::H5File& file, std::string& arena_config_json);

    // Find H5 file in directory
    static std::string findH5FileInDirectory(const std::string& directory);

    // Check if file is H5
    static bool isH5File(const std::string& filepath);

    // Get data for specific frame
    static FrameMetadataRecord* getFrameMetadata(H5SessionData& data, uint64_t frame_num);
    static std::vector<LoggedBoundingBox> getBoundingBoxesForFrame(const H5SessionData& data, uint64_t frame_id);
    static std::vector<LoggedChaserState> getChaserStatesForFrame(const H5SessionData& data, uint64_t frame_num);
    static FrameMetadataRecord* getFrameMetadataByCameraID(H5SessionData& data, uint64_t camera_frame_id);

private:
    // Helper functions
    void setError(std::string& error_message, const std::string& msg);
    bool groupExists(H5::H5File& file, const std::string& group_name);
    bool datasetExists(H5::Group& group, const std::string& dataset_name);
};

// Integration function for your existing code
bool loadH5SessionFromDirectory(const std::string& video_directory,
                                H5SessionData& h5_data,
                                std::string& error_message);

#endif // H5_LOADER_H