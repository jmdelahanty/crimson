// zarr_loader.h - Enhanced version with interpolation support
#ifndef ZARR_LOADER_H
#define ZARR_LOADER_H

#include <tensorstore/tensorstore.h>
#include <tensorstore/context.h>
#include <tensorstore/array.h>
#include <tensorstore/index_space/dim_expression.h>
#include <tensorstore/kvstore/kvstore.h>
#include <tensorstore/open.h>
#include <vector>
#include <string>
#include <optional>
#include <filesystem>
#include "h5_loader.h"  // For LoggedBoundingBox structure compatibility

namespace ts = tensorstore;

// Structure to hold interpolation run data
struct InterpolationRunData {
    std::string run_name;
    std::string created_at;
    std::string method;
    ts::TensorStore<float, 3> bboxes_store;      // [frames, max_dets, 4]
    ts::TensorStore<bool, 1> interpolation_mask; // [frames] - true if interpolated
    bool is_loaded = false;
};

// Structure to hold zarr detection data
struct ZarrDetectionData {
    // Frame-level data
    std::vector<int32_t> n_detections;
    size_t total_frames = 0;
    size_t max_detections = 0;
    
    // Metadata from .zattrs
    std::string video_path;
    std::string model_path;
    double fps = 30.0;
    
    // TensorStore handles for lazy loading
    ts::TensorStore<float, 3> bboxes_store;      // [frames, max_dets, 4]
    ts::TensorStore<float, 2> scores_store;      // [frames, max_dets]
    ts::TensorStore<int32_t, 2> class_ids_store; // [frames, max_dets]
    
    // Flags for optional data
    bool has_scores = false;
    bool has_class_ids = false;
    bool coordinates_normalized = false;  // true if coords are 0-1, false if pixel values
    
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
    bool hasScores() const { return data_.has_scores; }
    bool hasClassIDs() const { return data_.has_class_ids; }
    bool hasInterpolation() const { return data_.has_interpolation; }
    
    // Get interpolation metadata
    std::string getInterpolationMethod() const { 
        return data_.has_interpolation ? data_.latest_interpolation.method : "";
    }
    std::string getInterpolationCreatedAt() const {
        return data_.has_interpolation ? data_.latest_interpolation.created_at : "";
    }
    
    // Get raw detection data for a frame (for advanced use)
    struct FrameDetections {
        std::vector<std::array<float, 4>> boxes;  // [x_min, y_min, x_max, y_max]
        std::vector<float> scores;
        std::vector<int32_t> class_ids;
        size_t frame_id;
        bool is_interpolated = false;
    };
    FrameDetections getRawDetections(size_t frame_id, bool use_interpolated = true) const;
    
    // Static helper to find zarr files in a directory
    static std::optional<std::string> findZarrDetectionFile(const std::string& directory);
    
private:
    ZarrDetectionData data_;
    ts::Context context_;
    
    // Loading functions
    bool loadStandardFormat(const ts::kvstore::KvStore& store);
    bool loadMetadata(const ts::kvstore::KvStore& store);
    bool loadBoundingBoxes(const ts::kvstore::KvStore& store, const std::string& path);
    bool loadScores(const ts::kvstore::KvStore& store, const std::string& path);
    bool loadClassIDs(const ts::kvstore::KvStore& store, const std::string& path);
    bool loadNDetections(const ts::kvstore::KvStore& store, const std::string& path);
    
    // New: Load interpolation data
    bool loadInterpolationRuns(const ts::kvstore::KvStore& store);
    bool loadLatestInterpolationRun(const ts::kvstore::KvStore& store, const std::string& run_name);
    
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