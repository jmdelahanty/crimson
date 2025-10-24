// zarr_loader.h - Enhanced version with interpolation support
#ifndef ZARR_LOADER_H
#define ZARR_LOADER_H

#include <tensorstore/tensorstore.h>
#include <tensorstore/context.h>
#include <tensorstore/array.h>
#include <tensorstore/index_space/dim_expression.h>
#include <tensorstore/kvstore/kvstore.h>
#include <tensorstore/open.h>
#include <array>
#include <cstdint>
#include <vector>
#include <string>
#include <optional>
#include <filesystem>
#include "h5_loader.h"  // For LoggedBoundingBox structure compatibility

namespace ts = tensorstore;

enum class ZarrLayoutType {
    kUnknown = 0,
    kLegacyGrid,
    kPaletteRuns
};

// Structure to hold interpolation run data (refined detections and stimulus alignment)
struct InterpolationRunData {
    std::string run_name;
    std::string created_at;
    std::string method;
    std::string source_detection_run;
    std::string provenance_json;

    bool uses_palette_layout = false;
    bool has_flat_detections = false;

    // Flattened detection data (refined/interpolated detections)
    std::vector<int32_t> frame_indices;
    std::vector<std::array<float, 4>> bbox_norm_coords;
    std::vector<float> flat_scores;
    std::vector<int32_t> flat_class_ids;
    std::vector<size_t> frame_offsets;
    std::vector<uint8_t> detection_source;              // 0 = source, 1 = interpolated

    // Stimulus alignment (analysis/stimulus_runs)
    bool has_stimulus_alignment = false;
    std::string stimulus_run_name;
    std::string stimulus_created_at;
    int64_t camera_frame_offset = 0;
    std::vector<int32_t> camera_to_metadata_index;
    std::vector<uint8_t> stimulus_interpolation_mask;   // 1 = original, 0 = interpolated
    std::vector<uint8_t> frame_mask;                    // 1 = interpolated frame

    // Legacy dense layout fallback
    ts::TensorStore<float, 3> bboxes_store;      // [frames, max_dets, 4]
    ts::TensorStore<bool, 1> interpolation_mask; // [frames]

    bool is_loaded = false;
};

// Structure to hold zarr detection data
struct ZarrDetectionData {
    ZarrLayoutType layout = ZarrLayoutType::kUnknown;

    // Frame-level data
    std::vector<int32_t> n_detections;
    size_t total_frames = 0;
    size_t max_detections = 0;
    
    // Metadata from .zattrs
    std::string video_path;
    std::string model_path;
    double fps = 30.0;
    int image_width = 0;
    int image_height = 0;
    std::string detect_run_name;
    std::string detect_run_method;
    std::string detect_run_created_at;
    std::string detect_run_command;
    std::string detect_run_source;
    std::string detect_run_provenance_json;
    
    // TensorStore handles for lazy loading
    ts::TensorStore<float, 3> bboxes_store;      // [frames, max_dets, 4]
    ts::TensorStore<float, 2> scores_store;      // [frames, max_dets]
    ts::TensorStore<int32_t, 2> class_ids_store; // [frames, max_dets]
    
    // Flags for optional data
    bool has_scores = false;
    bool has_class_ids = false;
    bool coordinates_normalized = false;  // true if coords are 0-1, false if pixel values

    // Palette layout flat buffers
    std::vector<int32_t> frame_indices;                 // length = total detections
    std::vector<std::array<float, 4>> bbox_norm_coords; // normalized [cx, cy, w, h]
    std::vector<float> flat_scores;                     // optional, same length as frame_indices
    std::vector<int32_t> flat_class_ids;                // optional, same length as frame_indices
    std::vector<size_t> frame_offsets;                  // size total_frames + 1

    // Optional heading / keypoint data aligned with detections
    std::vector<float> flat_headings_deg;               // heading angle per detection
    std::vector<std::array<float, 2>> flat_swim_bladder_px;  // swim bladder anchor in pixel coords
    std::vector<uint8_t> flat_heading_valid;            // 1 if heading data valid
    bool has_heading_data = false;
    std::string keypoints_run_name;
    std::string keypoints_source_crop_run;

    // Interpolation data
    InterpolationRunData latest_interpolation;
    bool has_interpolation = false;
};

class ZarrDetectionLoader {
public:
    ZarrDetectionLoader();
    ~ZarrDetectionLoader();
    
    // Main loading function
    bool loadZarrFile(const std::string& filepath, std::string& error_message);
    
    // Compatibility interface matching H5SessionLoader
    std::vector<LoggedBoundingBox> getBoundingBoxesForFrame(size_t frame_id) const;
    
    // Get boxes with interpolation preference
    std::vector<LoggedBoundingBox> getBoundingBoxesForFrame(size_t frame_id, bool use_interpolated) const;
    
    // Check if frame is interpolated
    bool isFrameInterpolated(size_t frame_id) const;
    
    // Additional utility functions
    size_t getTotalFrames() const { return data_.total_frames; }
    size_t getMaxDetections() const { return data_.max_detections; }
    int32_t getDetectionsForFrame(size_t frame_id) const;
    double getFPS() const { return data_.fps; }
    int getImageWidth() const { return data_.image_width; }
    int getImageHeight() const { return data_.image_height; }
    const std::string& getDetectRunName() const { return data_.detect_run_name; }
    const std::string& getDetectRunMethod() const { return data_.detect_run_method; }
    const std::string& getDetectRunCreatedAt() const { return data_.detect_run_created_at; }
    bool hasScores() const { return data_.has_scores; }
    bool hasClassIDs() const { return data_.has_class_ids; }
    bool hasInterpolation() const { return data_.has_interpolation; }
    const std::string& getKeypointsRunName() const { return data_.keypoints_run_name; }
    bool hasStimulusAlignment() const {
        return data_.has_interpolation && data_.latest_interpolation.has_stimulus_alignment;
    }
    bool hasRefinedDetections() const {
        return data_.has_interpolation && data_.latest_interpolation.has_flat_detections;
    }
    
    // Get interpolation metadata
    std::string getInterpolationMethod() const { 
        if (!data_.has_interpolation) {
            return "";
        }
        if (!data_.latest_interpolation.method.empty()) {
            return data_.latest_interpolation.method;
        }
        if (!data_.latest_interpolation.stimulus_run_name.empty()) {
            return "stimulus_alignment";
        }
        return "";
    }
    std::string getInterpolationCreatedAt() const {
        if (!data_.has_interpolation) {
            return "";
        }
        if (!data_.latest_interpolation.created_at.empty()) {
            return data_.latest_interpolation.created_at;
        }
        if (!data_.latest_interpolation.stimulus_created_at.empty()) {
            return data_.latest_interpolation.stimulus_created_at;
        }
        return "";
    }
    std::string getInterpolationSourceRun() const {
        return data_.has_interpolation ? data_.latest_interpolation.source_detection_run : "";
    }
    std::string getStimulusRunName() const {
        return data_.has_interpolation ? data_.latest_interpolation.stimulus_run_name : "";
    }
    
    // Get raw detection data for a frame (for advanced use)
    struct FrameDetections {
        std::vector<std::array<float, 4>> boxes;  // [x_min, y_min, x_max, y_max]
        std::vector<float> scores;
        std::vector<int32_t> class_ids;
        size_t frame_id;
        bool is_interpolated = false;
        std::vector<float> headings_deg;
        std::vector<std::array<float, 2>> swim_bladder_pixels;
        std::vector<uint8_t> heading_valid;
    };
    FrameDetections getRawDetections(size_t frame_id, bool use_interpolated = true) const;

    bool hasHeadingData() const { return data_.has_heading_data; }
    
    // Static helper to find zarr files in a directory
    static std::optional<std::string> findZarrDetectionFile(const std::string& directory);
    
private:
    ZarrDetectionData data_;
    ts::Context context_;
    std::string root_path_;
    
    // Loading functions
    bool loadStandardFormat(const ts::kvstore::KvStore& store);
    bool loadMetadata(const ts::kvstore::KvStore& store);
    bool loadBoundingBoxes(const ts::kvstore::KvStore& store, const std::string& path);
    bool loadScores(const ts::kvstore::KvStore& store, const std::string& path);
    bool loadClassIDs(const ts::kvstore::KvStore& store, const std::string& path);
    bool loadNDetections(const ts::kvstore::KvStore& store, const std::string& path);
    
    // Palette layout loaders
    bool loadDetectionRuns(const ts::kvstore::KvStore& store);
    bool loadDetectionRunFromGroup(const ts::kvstore::KvStore& store,
                                   const std::string& group_path);
    bool loadFlattenedRun(const ts::kvstore::KvStore& store,
                          const std::string& base_path,
                          std::vector<int32_t>* frame_indices_out,
                          std::vector<std::array<float, 4>>& boxes_out,
                          std::vector<float>& scores_out,
                          std::vector<int32_t>& class_ids_out,
                          std::vector<int32_t>& n_detections_out,
                          std::vector<size_t>& frame_offsets_out,
                          std::vector<uint8_t>* detection_source_out,
                          bool& has_scores_out,
                          bool& has_class_ids_out,
                          size_t& resolved_frames_out);

    bool readInt32Array(const ts::kvstore::KvStore& store, const std::string& path, std::vector<int32_t>& out);
    bool readInt64Array(const ts::kvstore::KvStore& store, const std::string& path, std::vector<int64_t>& out);
    bool readFloatArray(const ts::kvstore::KvStore& store, const std::string& path, std::vector<float>& out);
    bool readFloatMatrix(const ts::kvstore::KvStore& store, const std::string& path, std::vector<std::array<float, 4>>& out);
    bool readBoolArray(const ts::kvstore::KvStore& store, const std::string& path, std::vector<uint8_t>& out);

    // New: Load interpolation data
    bool loadInterpolationRuns(const ts::kvstore::KvStore& store);
    bool loadRefinedDetectRuns(const ts::kvstore::KvStore& store);
    bool loadStimulusAlignment(const ts::kvstore::KvStore& store);
    bool loadLatestInterpolationRun(const ts::kvstore::KvStore& store, const std::string& run_name);
    bool loadPaletteInterpolationRun(const ts::kvstore::KvStore& store,
                                     const std::string& run_name,
                                     const std::string& subgroup);
    bool loadKeypointHeadingData(const ts::kvstore::KvStore& store);
    
    // Helper conversion function
    LoggedBoundingBox convertToLoggedBox(
        const std::array<float, 4>& box,
        float score,
        int32_t class_id,
        size_t frame_id,
        size_t box_index,
        bool is_interpolated = false
    ) const;
    
    // Helper to convert detections to LoggedBoundingBox format
    std::vector<LoggedBoundingBox> convertDetectionsToLoggedBoxes(
        const FrameDetections& detections, 
        size_t frame_id
    ) const;
};

// Standalone helper function
bool loadZarrDetectionFromDirectory(
    const std::string& dir_path,
    ZarrDetectionLoader& loader,
    std::string& error_message
);

#endif // ZARR_LOADER_H
