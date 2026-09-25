#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class PaletteClippedResolver {
public:
    struct SelectedRun {
        std::string work_unit_id;
        std::string camera_serial;
        std::string clip_id;
        std::string detect_run;
        std::string refined_detect_run;
        std::string detect_group_path;
        std::string refined_group_path;
        std::string video_path;
        std::string metadata_path;
        std::string keyframe_path;
        std::vector<int64_t> parent_frame_by_clip_local;
    };

    struct FrameRunRow {
        std::string camera_serial;
        std::string clip_id;
        int64_t recording_frame_id = -1;
        int64_t parent_frame_index = -1;
        int64_t clip_local_frame_index = -1;
        size_t selected_run_index = 0;
    };

    void clear();

    bool load(const std::filesystem::path& analysis_zarr,
              const std::string& explicit_collection_id,
              std::string& error_message);

    bool loaded() const { return loaded_; }
    const std::filesystem::path& analysisZarrPath() const { return analysis_zarr_; }
    const std::filesystem::path& recordingFrameIndexPath() const {
        return recording_frame_index_path_;
    }
    const std::string& collectionId() const { return collection_id_; }
    const std::string& primaryCameraSerial() const { return primary_camera_serial_; }
    size_t selectedRunCount() const { return selected_runs_.size(); }
    size_t mappedFrameCount() const { return mapped_frame_count_; }
    size_t totalParentFrames() const { return total_parent_frames_; }
    size_t unselectedFramePairCount() const { return unselected_frame_pair_count_; }

    const std::vector<SelectedRun>& selectedRuns() const { return selected_runs_; }
    const SelectedRun* selectedRun(size_t index) const;
    const FrameRunRow* rowForParentFrame(
        int64_t parent_frame_index,
        const std::string& camera_serial = std::string()) const;

private:
    bool loaded_ = false;
    std::filesystem::path analysis_zarr_;
    std::filesystem::path recording_frame_index_path_;
    std::string collection_id_;
    std::string primary_camera_serial_;
    std::vector<SelectedRun> selected_runs_;
    std::vector<FrameRunRow> rows_;
    std::unordered_map<std::string, std::vector<int64_t>> parent_to_row_by_camera_;
    size_t mapped_frame_count_ = 0;
    size_t total_parent_frames_ = 0;
    size_t unselected_frame_pair_count_ = 0;
};
