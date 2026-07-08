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
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <vector>
#include <string>
#include <optional>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include "h5_loader.h"  // For LoggedBoundingBox structure compatibility
#include "keypoint_heading_utils.h"
#include "zarr/palette_clipped_resolver.h"

namespace ts = tensorstore;
using json = nlohmann::json;

enum class ZarrLayoutType {
    kUnknown = 0,
    kLegacyGrid,
    kPaletteRuns
};

struct ZarrCalibrationData {
    std::string source_group;
    std::string active_camera_id;
    std::string primary_camera_id;
    std::string source_h5;
    std::string source_stimulus_run;
    std::string homography_source;
    std::string homography_matrix_direction;
    std::string experimental_area_shape;

    // Normalized Palette contract: projector/texture/canvas pixels -> camera pixels.
    std::array<double, 9> homography_projector_to_camera = {};

    double pixel_to_mm = std::numeric_limits<double>::quiet_NaN();
    double pixels_per_mm_camera = std::numeric_limits<double>::quiet_NaN();
    double pixels_per_mm_projector = std::numeric_limits<double>::quiet_NaN();
    double real_world_ref_mm = std::numeric_limits<double>::quiet_NaN();
    double native_width_px = std::numeric_limits<double>::quiet_NaN();
    double native_height_px = std::numeric_limits<double>::quiet_NaN();
    double experimental_area_center_x_px = std::numeric_limits<double>::quiet_NaN();
    double experimental_area_center_y_px = std::numeric_limits<double>::quiet_NaN();
    double experimental_area_radius_px = std::numeric_limits<double>::quiet_NaN();
    double experimental_area_radius_mm = std::numeric_limits<double>::quiet_NaN();
    double sub_arena_x_px = 0.0;
    double sub_arena_y_px = 0.0;
    double sub_arena_width_px = std::numeric_limits<double>::quiet_NaN();
    double sub_arena_height_px = std::numeric_limits<double>::quiet_NaN();
};

// Structure to hold interpolation run data (refined detections and stimulus alignment)
struct InterpolationRunData {
    std::string run_name;
    std::string created_at;
    std::string method;
    std::string source_detection_run;
    std::string provenance_json;
    std::string stage_label;

    bool uses_palette_layout = false;
    bool has_flat_detections = false;
    bool boxes_are_pixel_xyxy = false;

    // Flattened detection data (refined/interpolated detections)
    std::vector<int32_t> frame_indices;
    std::vector<std::array<float, 4>> bbox_norm_coords;
    std::vector<float> flat_scores;
    std::vector<int32_t> flat_class_ids;
    std::vector<size_t> frame_offsets;
    std::vector<uint8_t> detection_source;              // 0 = source, 1 = interpolated
    std::vector<std::string> detection_reason;          // optional per-detection reason label
    std::vector<int32_t> n_detections;
    bool has_scores = false;
    bool has_class_ids = false;

    // Stimulus alignment (analysis/stimulus_runs)
    bool has_stimulus_alignment = false;
    std::string stimulus_run_name;
    std::string stimulus_created_at;
    int64_t camera_frame_offset = 0;
    std::vector<int32_t> camera_to_metadata_index;
    std::vector<int32_t> camera_to_metadata_index_corrected;
    std::vector<int32_t> camera_to_stimulus_frame_corrected;
    std::vector<uint8_t> camera_stimulus_frame_interpolated;
    std::vector<uint8_t> stimulus_interpolation_mask;   // 1 = original, 0 = interpolated
    std::vector<uint8_t> frame_mask;                    // 1 = interpolated frame
    std::vector<int32_t> frame_metadata_stimulus_frames;
    std::vector<int32_t> frame_metadata_stimulus_frames_corrected;
    bool frame_metadata_loaded = false;
    bool frame_metadata_corrected_loaded = false;
    bool has_direct_stimulus_lookup = false;
    int32_t first_camera_frame_with_stimulus = -1;
    int32_t first_metadata_index_with_stimulus = -1;
    int32_t first_stimulus_frame = -1;
    int32_t first_camera_frame_with_stimulus_corrected = -1;
    int32_t first_metadata_index_with_stimulus_corrected = -1;
    int32_t first_stimulus_frame_corrected = -1;

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
    
    // Metadata from Zarr attrs (v3 zarr.json; .zattrs compatibility fallback)
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

    // Review status from refined_detect_runs/<run>/zarr.json detect_review_status
    std::string review_state;        // e.g. "approved", "needs_review"
    std::string review_method;       // e.g. "manual", "algorithmic"
    std::string review_intended_use; // e.g. "full_recording", "training"
    std::string review_timestamp;
    std::string review_reviewer;
    std::string review_notes;
    bool has_review_status = false;

    // TensorStore handles for lazy loading
    ts::TensorStore<float, 3> bboxes_store;      // [frames, max_dets, 4]
    ts::TensorStore<float, 2> scores_store;      // [frames, max_dets]
    ts::TensorStore<int32_t, 2> class_ids_store; // [frames, max_dets]
    
    // Flags for optional data
    bool has_scores = false;
    bool has_class_ids = false;
    bool coordinates_normalized = false;  // true if coords are 0-1, false if pixel values
    bool boxes_are_pixel_xyxy = false;
    bool has_clipped_collection = false;

    // Palette layout flat buffers
    std::vector<int32_t> frame_indices;                 // length = total detections
    std::vector<std::array<float, 4>> bbox_norm_coords; // normalized [cx, cy, w, h]
    std::vector<float> flat_scores;                     // optional, same length as frame_indices
    std::vector<int32_t> flat_class_ids;                // optional, same length as frame_indices
    std::vector<size_t> frame_offsets;                  // size total_frames + 1
    std::vector<uint8_t> detection_source_flags;        // optional, same length as frame_indices
    std::vector<std::string> detection_reason_flags;    // optional, same length as frame_indices
    std::vector<uint8_t> frame_interpolated_flags;      // per-frame flag derived from active dataset

    // Optional heading / keypoint data aligned with detections
    std::vector<float> flat_headings_deg;               // heading angle per detection
    std::vector<std::array<float, 2>> flat_swim_bladder_px;  // resolved heading origin in pixel coords
    std::vector<uint8_t> flat_heading_valid;            // 1 if heading data valid
    bool has_heading_data = false;
    bool has_keypoints = false;
    std::string keypoints_run_name;
    std::string keypoints_source_crop_run;
    std::vector<float> flat_keypoints_px;               // flattened [det, kp, coord]
    std::vector<int32_t> keypoint_roi_indices;          // detection-aligned ROI row index in selected keypoint run
    size_t keypoints_per_detection = 0;
    std::vector<std::string> keypoint_labels;
    std::vector<std::array<size_t, 2>> skeleton_edges;  // from pose_schema.edges
    KeypointHeadingComputationSpec heading_computation_spec;

    // Refined keypoint quality metadata (detection-aligned, same indexing as flat_keypoints_px)
    bool is_refined_keypoints = false;
    std::string refined_keypoints_run_name;

    std::vector<int32_t> flat_keypoint_quality_labels;    // 0=clean, 4=source_failed, 6=flip_corrected
    std::vector<std::string> flat_keypoint_reason;        // pipe-delimited tags
    std::vector<uint8_t> flat_keypoint_flip_corrected;
    std::vector<uint8_t> flat_keypoint_usable;
    std::vector<uint8_t> flat_keypoint_confidence_valid;
    std::vector<uint8_t> flat_keypoint_geometry_valid;
    std::vector<uint8_t> flat_keypoint_refined_success;
    std::vector<uint8_t> flat_keypoint_detection_source;  // 0=real, 1=interpolated

    // Keypoint review status (from refined_keypoints_runs/<run> attrs)
    std::string kp_review_state;
    std::string kp_review_method;
    std::string kp_review_intended_use;
    std::string kp_review_timestamp;
    std::string kp_review_reviewer;
    std::string kp_review_notes;
    bool has_kp_review_status = false;

    std::vector<int32_t> mask_roi_indices;
    std::vector<float> roi_offset_x;
    std::vector<float> roi_offset_y;
    std::vector<float> roi_width_px;
    std::vector<float> roi_height_px;
    bool has_eye_masks = false;
    bool eye_masks_loaded = false;
    std::string eye_masks_run_name;
    std::string eye_masks_source_label;
    std::string eye_masks_source_path;
    std::string eye_masks_warning;
    bool eye_masks_from_refined_subject_masks = false;
    bool eye_masks_tolerant_metadata = false;
    ts::TensorStore<uint8_t, 4> eye_masks_store;
    ts::TensorStore<uint8_t, 4> eye_masks_bitpacked_store;
    size_t eye_mask_roi_count = 0;
    size_t eye_mask_height = 0;
    size_t eye_mask_width = 0;
    size_t eye_mask_chunk_rows = 0;
    std::array<size_t, 2> eye_mask_channel_indices = {
        std::numeric_limits<size_t>::max(),
        std::numeric_limits<size_t>::max()};
    std::array<std::string, 2> eye_mask_channel_labels = {
        "eye_left",
        "eye_right"};
    std::vector<std::string> refined_subject_mask_labels;
    std::vector<uint8_t> refined_subject_mask_available_channels;
    std::string refined_subject_mask_label_schema_id;
    std::string refined_subject_mask_source_crop_run;
    std::vector<int32_t> refined_subject_mask_frame_indices;
    std::vector<int64_t> refined_subject_mask_source_crop_row_ids;
    std::vector<float> refined_subject_mask_offset_x;
    std::vector<float> refined_subject_mask_offset_y;
    std::vector<float> refined_subject_mask_roi_width_px;
    std::vector<float> refined_subject_mask_roi_height_px;
    std::vector<int32_t> refined_subject_mask_source_crop_frame_indices;
    std::vector<uint8_t> refined_subject_mask_crop_frame_match;
    std::vector<std::vector<size_t>> refined_subject_mask_rows_by_frame;
    bool refined_subject_mask_row_position_fallback = false;
    bool refined_subject_mask_dense_masks_used = false;
    bool refined_subject_mask_bitpacked_masks_used = false;
    bool refined_subject_mask_rle_masks_used = false;
    mutable std::set<int32_t> refined_subject_mask_smoke_logged_frames;
    mutable size_t refined_subject_mask_rle_smoke_log_count = 0;
    struct RefinedSubjectMaskComponentInfo {
        std::string label;
        size_t channel_index = std::numeric_limits<size_t>::max();
        bool rle_available = false;
        ts::TensorStore<uint32_t, 1> rle_counts_store;
        size_t rle_counts_count = 0;
        std::vector<int64_t> rle_indptr;
        std::vector<uint8_t> rle_present;
        std::vector<int32_t> rle_area_px;
        std::vector<std::array<int32_t, 4>> rle_bbox_xyxy;
        bool contours_available = false;
        bool contour_attrs_compatible = false;
        std::string contour_warning;
        std::vector<int64_t> contour_ptr;
        std::vector<int32_t> contour_len;
        ts::TensorStore<float, 2> contour_points_store;
        size_t contour_points_count = 0;
    };
    std::vector<RefinedSubjectMaskComponentInfo>
        refined_subject_mask_overlay_components;
    std::vector<std::array<std::array<float, 4>, 2>> eye_mask_feret_axes_major;
    std::vector<std::array<std::array<float, 4>, 2>> eye_mask_feret_axes_minor;
    bool eye_masks_have_feret_axes = false;
    struct EyeMaskChunkCacheEntry {
        size_t chunk_id = std::numeric_limits<size_t>::max();
        size_t chunk_start = 0;
        size_t chunk_length = 0;
        std::vector<std::array<std::vector<uint32_t>, 2>> pixel_indices;
        std::vector<std::vector<std::vector<uint32_t>>>
            component_pixel_indices;
        std::vector<std::vector<std::vector<std::array<float, 2>>>>
            component_contours_xy;
    };
    mutable std::vector<EyeMaskChunkCacheEntry> mask_chunk_cache;
    mutable std::shared_ptr<std::mutex> mask_chunk_cache_mutex =
        std::make_shared<std::mutex>();
    mutable std::set<size_t> mask_chunk_loads_in_flight;

    bool has_eye_angles = false;
    std::string eye_angle_run_name;
    std::vector<int32_t> eye_angle_frame_indices;
    std::vector<uint8_t> eye_angle_valid_mask;
    std::vector<float> eye_angle_left_deg;
    std::vector<float> eye_angle_right_deg;
    std::vector<std::vector<size_t>> eye_angle_indices_by_frame;
    bool has_eye_frame_angles = false;
    std::vector<float> eye_frame_left_angle_deg;
    std::vector<float> eye_frame_right_angle_deg;
    std::vector<float> eye_frame_vergence_deg;
    std::vector<float> eye_vergence_signed_frame_deg;
    std::vector<float> eye_vergence_frame_time_seconds;
    std::vector<uint8_t> eye_vergence_frame_valid;
    bool has_eye_vergence_frame = false;

    struct EyeAngleFieldInfo {
        std::string name;
        std::string representation_key;
        std::string field_role;
        std::string display_name;
        std::string units;
        bool default_plot = false;
    };
    struct EyeAngleRepresentationInfo {
        std::string key;
        std::string display_name;
        std::string role;
        std::string axis;
        std::string coordinate_frame;
        std::string units;
        std::string sign_convention;
        std::string derived_from;
        std::vector<std::string> default_plot_fields;
        std::vector<std::string> primary_roi_fields;
        std::vector<std::string> aggregate_roi_fields;
        std::vector<std::string> vector_roi_fields;
        std::vector<std::string> frame_fields;
    };
    struct EyeAngleScalarField {
        std::string name;
        std::string representation_key;
        std::string field_role;
        std::string display_name;
        std::string units;
        bool has_roi = false;
        bool has_frame = false;
        std::vector<float> roi_values;
        std::vector<float> frame_values;
    };
    struct EyeAngleVectorField {
        std::string name;
        std::string representation_key;
        std::string field_role;
        std::string display_name;
        std::string units;
        bool has_roi = false;
        std::vector<std::array<float, 2>> roi_values;
    };
    struct EyeAngleAnalysisData {
        bool loaded = false;
        bool variant_schema_inferred = false;
        std::string run_name;
        std::string schema_id;
        int schema_version = -1;
        std::string method;
        std::string method_version;
        std::string source_geometry_kind;
        std::string source_eye_geometry_run;
        std::string source_subject_shape_run;
        std::string source_refined_subject_masks_run;
        std::string source_keypoints_run;
        std::string output_schema_id;
        int output_schema_version = -1;
        std::string variant_schema_id;
        int variant_schema_version = -1;
        std::string default_representation;
        std::vector<std::string> representation_order;
        std::vector<EyeAngleRepresentationInfo> representations;
        std::vector<EyeAngleFieldInfo> fields;
        std::vector<EyeAngleScalarField> scalar_fields;
        std::vector<EyeAngleVectorField> vector_fields;
        std::unordered_map<int32_t, std::string> reason_code_map;
        std::vector<int32_t> roi_frame_indices;
        std::vector<int32_t> row_to_frame;
        std::vector<float> roi_time_seconds;
        std::vector<float> frame_time_seconds;
        std::vector<uint8_t> roi_valid_left;
        std::vector<uint8_t> roi_valid_right;
        std::vector<uint8_t> roi_valid_frame;
        std::vector<uint8_t> frame_valid_frame;
        std::vector<uint8_t> roi_left_major_axis_marginal;
        std::vector<uint8_t> roi_right_major_axis_marginal;
        std::vector<uint8_t> roi_major_axis_marginal;
        std::vector<uint8_t> frame_major_axis_marginal;
        std::vector<int32_t> roi_reason_codes;
        std::vector<int32_t> frame_reason_codes;
        std::vector<std::string> roi_reason_labels;
        std::vector<std::string> frame_reason_labels;
        size_t row_count = 0;
        size_t frame_count = 0;
        std::string warning;
    };
    EyeAngleAnalysisData eye_angle_analysis;

    struct SubjectShapeData {
        bool loaded = false;
        std::string run_name;
        std::string source_refined_subject_masks_run;
        std::string schema_id;
        int schema_version = -1;
        std::string method;
        int method_version = -1;
        std::string head_endpoint_semantics;
        std::string row_axis;
        std::string warning;
        size_t row_count = 0;
        size_t centerline_points = 0;
        size_t bspline_sample_points = 0;
        size_t bspline_control_points = 0;
        size_t tail_sample_count = 0;
        size_t tail_normal_count = 0;
        std::vector<int32_t> frame_indices;
        std::vector<int32_t> row_to_frame;

        std::vector<std::array<float, 2>> body_origin_xy;
        std::vector<std::array<float, 2>> body_forward_axis_xy;
        std::vector<std::array<float, 2>> body_left_axis_xy;
        std::vector<uint8_t> body_frame_valid;
        std::vector<std::string> body_frame_failure_reason;

        std::vector<std::array<float, 2>> snout_tip_xy;
        std::vector<uint8_t> snout_tip_valid;
        std::vector<std::string> snout_tip_failure_reason;
        std::vector<std::array<float, 2>> tail_base_xy;
        std::vector<uint8_t> tail_base_valid;
        std::vector<std::array<float, 2>> tail_tip_xy;
        std::vector<float> centerline_xy;
        std::vector<uint8_t> centerline_valid;
        std::vector<uint8_t> centerline_reaches_snout;
        std::vector<std::string> centerline_failure_reason;
        std::vector<float> bspline_sample_xy;
        std::vector<float> bspline_control_points_xy;
        std::vector<uint8_t> bspline_valid;
        std::vector<std::string> bspline_failure_reason;
        std::vector<float> tail_sample_xy;
        std::vector<float> tail_normal_xy;
        std::vector<uint8_t> tail_sample_valid;
        std::vector<std::string> tail_sample_failure_reason;
        std::vector<uint8_t> source_mask_qc_severe_failure;
        std::vector<std::string> source_mask_qc_reason;

        std::vector<std::array<float, 2>> caudal_contour_point_xy;
        std::vector<uint8_t> caudal_contour_valid;
    };
    SubjectShapeData subject_shape;

    struct TailKinematicsData {
        bool loaded = false;
        std::string run_name;
        std::string source_subject_shape_run;
        std::string source_refined_subject_masks_run;
        std::string schema_id;
        int schema_version = -1;
        std::string method;
        int method_version = -1;
        std::string row_axis;
        std::string warning;
        size_t row_count = 0;
        size_t sample_count = 0;

        std::vector<int32_t> frame_index;
        std::vector<int32_t> row_to_frame;
        std::vector<uint8_t> valid;
        std::vector<std::string> failure_reason;
        std::vector<float> tail_angle_sample_s;
        std::vector<float> tail_angle_sample_xy;
        size_t tail_angle_sample_xy_count = 0;
        std::vector<float> tail_angle_deg;
        std::vector<float> tail_tip_angle_deg;
        std::vector<float> max_abs_tail_angle_deg;
        std::vector<float> tail_angle_rms_deg;
        std::vector<float> tail_lateral_deflection_px;
        std::vector<float> tail_tip_lateral_deflection_px;
        std::vector<float> tail_curvature_px_inv;
        std::vector<float> max_abs_tail_curvature_px_inv;
    };
    TailKinematicsData tail_kinematics;

    struct CropImageData {
        bool metadata_loaded = false;
        bool loaded = false;
        std::string run_name;
        size_t roi_count = 0;
        size_t height = 0;
        size_t width = 0;
        size_t channels = 0;
        std::vector<uint8_t> images;
        std::vector<int32_t> frame_indices;
    };
    CropImageData crop_data;
    std::string movement_crop_run_name;

    // Interpolation data
    InterpolationRunData latest_interpolation;
    bool has_interpolation = false;

    struct EventLogEntry {
        int32_t stimulus_frame_num = -1;
        int32_t camera_frame_id = -1;
        int64_t timestamp_ns_session = 0;
        int32_t event_type_id = -1;
        std::string name_or_context;
        std::string details_json;
    };
    struct StimulusEventSummary {
        size_t source_event_index = std::numeric_limits<size_t>::max();
        int32_t stimulus_frame_num = -1;
        int32_t camera_frame_id = -1;
        int32_t event_type_id = -1;
        std::string label;
    };
    std::vector<EventLogEntry> stimulus_events;
    std::vector<std::string> stimulus_event_labels;
    std::vector<StimulusEventSummary> stimulus_event_timeline;
    size_t stimulus_event_timeline_generation = 0;
    std::unordered_map<int32_t, std::string> event_type_names;
    std::vector<std::vector<size_t>> stimulus_events_by_frame;
    std::vector<std::vector<size_t>> stimulus_events_by_camera_frame;
    bool has_stimulus_events = false;
    struct StimulusStep {
        struct MovingGratingAttrs {
            bool present = false;
            double grating_direction_camera_deg =
                std::numeric_limits<double>::quiet_NaN();
            double orientation_degrees_authored =
                std::numeric_limits<double>::quiet_NaN();
            double camera_to_projector_offset_deg =
                std::numeric_limits<double>::quiet_NaN();
            std::string direction_mapping_status;
            bool direction_mapping_validated = false;
            bool has_direction_mapping_validated = false;
            double speed_mm_s = std::numeric_limits<double>::quiet_NaN();
            double temporal_frequency_hz =
                std::numeric_limits<double>::quiet_NaN();
        };

        struct ConcentricGratingAttrs {
            bool present = false;
            std::string stimulus_role;
            std::string radial_polarity_authored;
            double radial_sign_authored =
                std::numeric_limits<double>::quiet_NaN();
            bool radial_polarity_validated = false;
            bool has_radial_polarity_validated = false;
            double center_x_px = std::numeric_limits<double>::quiet_NaN();
            double center_y_px = std::numeric_limits<double>::quiet_NaN();
            double center_x_mm = std::numeric_limits<double>::quiet_NaN();
            double center_y_mm = std::numeric_limits<double>::quiet_NaN();
            double target_radius_min_mm =
                std::numeric_limits<double>::quiet_NaN();
            double target_radius_max_mm =
                std::numeric_limits<double>::quiet_NaN();
            double speed_mm_s = std::numeric_limits<double>::quiet_NaN();
            double temporal_frequency_hz =
                std::numeric_limits<double>::quiet_NaN();
        };

        int32_t step_index = -1;
        std::string step_name;
        int32_t stimulus_mode_id = -1;
        std::string stimulus_mode;
        int32_t start_camera_frame = -1;
        int32_t end_camera_frame = -1;
        double duration_s = std::numeric_limits<double>::quiet_NaN();
        std::string raw_protocol_params_json;
        MovingGratingAttrs moving_grating;
        ConcentricGratingAttrs concentric_grating;
    };
    std::vector<StimulusStep> stimulus_steps;
    bool has_stimulus_steps = false;
    std::string stimulus_steps_run_name;
    bool has_stimulus_alignment_data = false;
    int64_t stimulus_camera_frame_offset = 0;
    std::string stimulus_video_path;     // from run attrs "source_stimulus_video_path"
    std::string stimulus_source_h5;      // from run attrs "source_h5" (derive .mp4 by changing ext)

    // Movement analysis (analysis/movement_runs)
    bool has_movement_data = false;
    struct MovementSeries {
        std::string category;
        std::string run_name;
        std::string track_id;
        std::string speed_level;
        std::string primary_speed_label;
        std::string primary_speed_units;
        std::string primary_speed_source_path;
        std::string secondary_speed_label;
        std::string secondary_speed_units;
        std::string secondary_speed_source_path;
        std::string detection_variant;
        std::string source_detect_run;
        double fps = 0.0;
        double smoothing_seconds = 0.0;
        int video_width = 0;
        int video_height = 0;
        bool from_speed_runs = false;
        std::vector<float> time_seconds;
        std::vector<float> smoothed_speed_mm;
        std::vector<float> instant_speed_mm;
        std::vector<float> distance_to_target_mm;
        std::vector<float> heading_degrees;
        std::vector<float> smoothed_heading_degrees;
        std::vector<uint8_t> keypoint_success;
        std::vector<uint8_t> sample_valid;
        std::vector<uint8_t> transition_valid;
        std::vector<std::array<float, 2>> positions_px;
        std::vector<std::array<float, 2>> positions_mm;
        std::vector<float> heading_per_second_degrees;
        std::vector<float> heading_per_second_resultant;
        std::vector<float> heading_per_second_time_seconds;
        std::vector<int32_t> frame_indices;
        std::vector<int32_t> detection_indices;
        std::unordered_map<int32_t, size_t> frame_to_row;
    };
    struct SwimBoutSeries {
        std::string run_name;
        std::string speed_level;
        bool is_compact_layout = false;
        int32_t candidate_id = -1;
        int32_t signal_id = -1;
        std::string signal_role;
        std::string signal_name;
        std::string source_track_kinematics_run;
        int32_t track_id = -1;
        std::string detection_method;
        std::string detection_signal_label;
        std::string detection_signal_source_level;
        std::string detection_signal_source_path;
        std::string movement_metric_source_level;
        std::string path_distance_source_level;
        std::string default_level;
        float threshold_mm = std::numeric_limits<float>::quiet_NaN();
        float exponential_tau_s = std::numeric_limits<float>::quiet_NaN();
        float min_bout_duration_s = std::numeric_limits<float>::quiet_NaN();
        float min_gap_duration_s = std::numeric_limits<float>::quiet_NaN();
        float min_peak_prominence_mm_s =
            std::numeric_limits<float>::quiet_NaN();
        float peak_width_rel_height = std::numeric_limits<float>::quiet_NaN();
        bool is_latest_run = false;
        bool is_default_level = false;
        std::vector<int32_t> start_frame;
        std::vector<int32_t> end_frame;
        std::vector<int32_t> core_start_frame;
        std::vector<int32_t> core_end_frame;
        std::vector<float> start_time_s;
        std::vector<float> end_time_s;
        std::vector<float> duration_s;
        std::vector<float> path_length_mm;
        std::vector<float> path_length_px;
        std::vector<float> net_displacement_mm;
        std::vector<float> net_displacement_px;
        std::vector<float> peak_detection_signal_mm_s;
        std::vector<float> peak_speed_mm_s;
        std::vector<uint8_t> gap_censored;
        std::vector<int32_t> detector_trace_frame_indices;
        std::vector<float> detector_trace_values;
        std::string detector_trace_label;
        std::string detector_trace_units;
        bool has_detector_trace = false;
    };
    struct BoutKinematicsSeries {
        std::string run_name;
        std::string source_track_kinematics_run;
        int32_t source_track_id = -1;
        std::string source_swim_bout_run;
        std::string source_swim_bout_speed_level;
        int32_t source_swim_bout_candidate_id = -1;
        int32_t source_swim_bout_signal_id = -1;
        std::string source_swim_bout_signal_role;
        std::string schema_id;
        std::string created_at_utc;
        std::string movement_metric_source_level;
        bool is_compact_layout = false;
        bool metrics_loaded = false;
        bool metrics_load_failed = false;
        std::string metrics_load_error;
        size_t compact_movement_metric_count = 0;
        size_t compact_heading_smoothed_metric_count = 0;
        size_t compact_heading_raw_metric_count = 0;
        size_t compact_eye_gaze_metric_count = 0;
        std::vector<int32_t> source_start_frame;
        std::vector<int32_t> source_end_frame;
        std::vector<int32_t> source_core_start_frame;
        std::vector<int32_t> source_core_end_frame;
        std::vector<int32_t> physical_active_start_frame;
        std::vector<int32_t> physical_active_end_frame;
        std::vector<float> physical_active_duration_s;
        std::vector<float> physical_active_path_length_mm;
        std::vector<float> physical_active_path_length_px;
        std::vector<float> physical_active_mean_speed_mm_s;
        std::vector<float> physical_active_peak_speed_mm_s;
        std::vector<uint8_t> physical_active_valid;
        std::vector<std::string> failure_reason;
        std::vector<float> heading_smoothed_net_delta_heading_deg;
        std::vector<float> heading_raw_net_delta_heading_deg;
        std::vector<float> eye_gaze_within_bout_vergence_gaze_mean_deg;
    };
    std::vector<MovementSeries> movement_series;
    size_t movement_selected_index = std::numeric_limits<size_t>::max();
    std::vector<SwimBoutSeries> swim_bout_series;
    std::vector<BoutKinematicsSeries> bout_kinematics_series;

    struct ChaserBoundingBoxRecord {
        int32_t camera_frame_id = -1;
        int32_t stimulus_frame_num = -1;
        int32_t fish_id = -1;
        int32_t chaser_index = -1;
        float x_px = std::numeric_limits<float>::quiet_NaN();
        float y_px = std::numeric_limits<float>::quiet_NaN();
        float width_px = std::numeric_limits<float>::quiet_NaN();
        float height_px = std::numeric_limits<float>::quiet_NaN();
        float centroid_x = std::numeric_limits<float>::quiet_NaN();
        float centroid_y = std::numeric_limits<float>::quiet_NaN();
        float confidence = std::numeric_limits<float>::quiet_NaN();
        bool is_target = false;
    };
    std::vector<ChaserBoundingBoxRecord> chaser_bounding_boxes;
    std::vector<std::vector<size_t>> chaser_bboxes_by_camera_frame;
    bool has_chaser_bboxes = false;

    struct ChaserStateRecord {
        int32_t stimulus_frame_num = -1;
        int32_t camera_frame_id = -1;
        int32_t chaser_index = -1;
        float chaser_pos_x = std::numeric_limits<float>::quiet_NaN();
        float chaser_pos_y = std::numeric_limits<float>::quiet_NaN();
        float target_pos_x = std::numeric_limits<float>::quiet_NaN();
        float target_pos_y = std::numeric_limits<float>::quiet_NaN();
        float chaser_radius_px = std::numeric_limits<float>::quiet_NaN();
        float distance_to_target_px = std::numeric_limits<float>::quiet_NaN();
        float target_speed_px_per_s = std::numeric_limits<float>::quiet_NaN();
        int64_t timestamp_ns_session = 0;
        uint8_t is_chasing = 0;
        bool texture_space = true;
        std::string coordinate_frame;
        std::string coordinate_origin;
        double stimulus_canvas_offset_x = 0.0;
        double stimulus_canvas_offset_y = 0.0;
        bool has_stimulus_canvas_offset = false;
        double chaser_camera_x = std::numeric_limits<double>::quiet_NaN();
        double chaser_camera_y = std::numeric_limits<double>::quiet_NaN();
        double target_camera_x = std::numeric_limits<double>::quiet_NaN();
        double target_camera_y = std::numeric_limits<double>::quiet_NaN();
        bool has_camera_coords = false;
        std::array<float, 4> chaser_rgba = {1.0f, 0.0f, 0.0f, 1.0f};
        bool has_chaser_rgba = false;
        int32_t behavior_mode = -1;
        bool has_behavior_mode = false;
        bool enable_chase = false;
        bool has_enable_chase = false;
        bool enable_random_movement = false;
        bool has_enable_random_movement = false;
    };
    std::vector<ChaserStateRecord> chaser_states;
    std::vector<std::vector<size_t>> chaser_states_by_camera_frame;
    std::vector<std::vector<size_t>> chaser_states_by_stimulus_frame;
    bool has_chaser_states = false;
    std::vector<ChaserStateRecord> chaser_states_interpolated;
    std::vector<std::vector<size_t>> chaser_states_interpolated_by_stimulus_frame;
    bool has_chaser_states_interpolated = false;
    std::unordered_map<int32_t, std::array<float, 4>> chaser_rgba_by_index;
    bool has_chaser_rgba_metadata = false;
    struct ChaserBehaviorMetadata {
        int32_t behavior_mode = -1;
        bool has_behavior_mode = false;
        bool enable_chase = false;
        bool has_enable_chase = false;
        bool enable_random_movement = false;
        bool has_enable_random_movement = false;
    };
    std::unordered_map<int32_t, ChaserBehaviorMetadata> chaser_behavior_by_index;
    bool has_chaser_behavior_metadata = false;

    struct ChaserCoordinateTransform {
        double texture_width = 0.0;
        double texture_height = 0.0;
        double camera_width = 0.0;
        double camera_height = 0.0;
        double scale = 1.0;
        double offset_x_px = 0.0;
        double offset_y_px = 0.0;
        std::string coordinate_frame;
        std::string coordinate_origin;
        double arena_origin_canvas_x_px = 0.0;
        double arena_origin_canvas_y_px = 0.0;
        double arena_region_width_px = 0.0;
        double arena_region_height_px = 0.0;
        bool has_arena_canvas_origin = false;
        bool valid = false;
    } chaser_transform;

    // Cached detection datasets
    InterpolationRunData raw_detection_dataset;
    bool has_raw_detection_dataset = false;
    InterpolationRunData refined_filtered_dataset;
    bool has_refined_filtered_dataset = false;
    InterpolationRunData refined_interpolated_dataset;
    bool has_refined_interpolated_dataset = false;
    InterpolationRunData refined_manual_dataset;
    bool has_refined_manual_dataset = false;
    InterpolationRunData refined_root_dataset;
    bool has_refined_root_dataset = false;
};

struct ManualWriteReviewOptions {
    std::string intended_use = "full_recording";
    std::string state = "approved";
    std::string method = "manual";
    std::string reviewer;  // empty = omitted from payload
    std::string notes;     // empty = omitted from payload
};

class ZarrDetectionLoader {
public:
    enum class ReviewArtifactKind {
        Detection,
        Keypoint,
        EyeMask,
    };

    struct ReviewArtifactSummary {
        ReviewArtifactKind kind = ReviewArtifactKind::Detection;
        std::string label;
        std::string run_name;
        std::string review_state;
        std::string review_method;
        std::string review_intended_use;
        std::string review_timestamp;
        std::string review_reviewer;
        std::string review_notes;
        bool has_review_status = false;
    };

    ZarrDetectionLoader();
    ~ZarrDetectionLoader();
    static constexpr size_t kEyeMaskChunkCacheCapacity = 3;
    static constexpr size_t kRleEyeMaskChunkRows = 32;
    static constexpr size_t kRleEyeMaskChunkCacheCapacity = 8;
    static constexpr size_t kEyeMaskPrefetchQueueCapacity = 8;
    static constexpr size_t kEyeMaskPrefetchAheadChunks = 2;

    enum class DetectionDataset {
        RawDetect = 0,
        RefinedFiltered = 1,
        RefinedInterpolated = 2,
        RefinedManual = 3,
        RefinedRoot = 4
    };
    
    // Main loading function
    bool loadZarrFile(const std::string& filepath, std::string& error_message);
    std::optional<ZarrCalibrationData> loadCalibrationForCamera(
        const std::string& camera_name_or_id,
        std::string& status_message) const;
    void setRequestedSubjectShapeRunName(const std::string& run_name) {
        requested_subject_shape_run_name_ = run_name;
    }
    const std::string& getRequestedSubjectShapeRunName() const {
        return requested_subject_shape_run_name_;
    }
    void setRequestedRefinedSubjectMaskRunName(const std::string& run_name) {
        requested_refined_subject_mask_run_name_ = run_name;
    }
    const std::string& getRequestedRefinedSubjectMaskRunName() const {
        return requested_refined_subject_mask_run_name_;
    }
    void setRequestedRefinedSubjectMaskStorage(const std::string& storage) {
        requested_refined_subject_mask_storage_ = storage;
    }
    const std::string& getRequestedRefinedSubjectMaskStorage() const {
        return requested_refined_subject_mask_storage_;
    }
    void setRequestedTailKinematicsRunName(const std::string& run_name) {
        requested_tail_kinematics_run_name_ = run_name;
    }
    const std::string& getRequestedTailKinematicsRunName() const {
        return requested_tail_kinematics_run_name_;
    }
    void setRequestedEyeAngleRunName(const std::string& run_name) {
        requested_eye_angle_run_name_ = run_name;
    }
    const std::string& getRequestedEyeAngleRunName() const {
        return requested_eye_angle_run_name_;
    }
    void setRequestedStimulusRunName(const std::string& run_name) {
        requested_stimulus_run_name_ = run_name;
    }
    const std::string& getRequestedStimulusRunName() const {
        return requested_stimulus_run_name_;
    }
    
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
    const std::string& getSourceVideoPath() const { return data_.video_path; }
    const std::string& getArchivePath() const { return root_path_; }
    bool hasClippedCollection() const { return data_.has_clipped_collection; }
    const PaletteClippedResolver& getClippedResolver() const {
        return clipped_resolver_;
    }
    const PaletteClippedResolver::FrameRunRow* resolveClippedFrame(
        int64_t parent_frame_index,
        const std::string& camera_serial = std::string()) const {
        return clipped_resolver_.rowForParentFrame(parent_frame_index,
                                                   camera_serial);
    }
    const std::string& getDetectRunName() const { return data_.detect_run_name; }
    const std::string& getDetectRunMethod() const { return data_.detect_run_method; }
    const std::string& getDetectRunCreatedAt() const { return data_.detect_run_created_at; }
    bool hasScores() const { return data_.has_scores; }
    bool hasClassIDs() const { return data_.has_class_ids; }
    bool coordinatesAreNormalized() const { return data_.coordinates_normalized; }
    std::vector<std::pair<DetectionDataset, std::string>> getAvailableDetectionDatasets() const;
    DetectionDataset getActiveDetectionDataset() const { return active_dataset_; }
    bool setActiveDetectionDataset(DetectionDataset dataset);
    bool isDatasetAvailable(DetectionDataset dataset) const;
    bool activeDatasetHasSyntheticDetections() const;
    bool hasInterpolation() const { return data_.has_interpolation; }
    const std::string& getKeypointsRunName() const { return data_.keypoints_run_name; }
    bool isRefinedKeypoints() const { return data_.is_refined_keypoints; }
    bool hasKeypointReviewStatus() const { return data_.has_kp_review_status; }
    const std::string& getKeypointReviewState() const { return data_.kp_review_state; }
    const std::string& getKeypointReviewMethod() const { return data_.kp_review_method; }
    const std::string& getKeypointReviewIntendedUse() const { return data_.kp_review_intended_use; }
    const std::string& getKeypointReviewTimestamp() const { return data_.kp_review_timestamp; }
    const std::string& getKeypointReviewReviewer() const { return data_.kp_review_reviewer; }
    const std::string& getKeypointReviewNotes() const { return data_.kp_review_notes; }
    bool hasEyeMasks() const { return data_.has_eye_masks; }
    const std::string& getEyeMaskRunName() const { return data_.eye_masks_run_name; }
    const std::string& getEyeMaskSourceLabel() const { return data_.eye_masks_source_label; }
    const std::string& getEyeMaskSourcePath() const { return data_.eye_masks_source_path; }
    const std::string& getEyeMaskWarning() const { return data_.eye_masks_warning; }
    bool eyeMasksUseRefinedSubjectMasks() const { return data_.eye_masks_from_refined_subject_masks; }
    bool eyeMasksUseTolerantMetadata() const { return data_.eye_masks_tolerant_metadata; }
    void requestRefinedSubjectMaskOptionalOverlayPrefetch();
    std::string getRefinedSubjectMaskOptionalOverlayStatus() const;
    bool requestEyeMaskCacheForFrame(size_t frame_id,
                                     size_t lookahead_frames = 0) const;
    bool warmEyeMaskCacheForFrame(size_t frame_id) const;
    const std::array<std::string, 2>& getEyeMaskChannelLabels() const {
        return data_.eye_mask_channel_labels;
    }
    const std::array<size_t, 2>& getEyeMaskChannelIndices() const {
        return data_.eye_mask_channel_indices;
    }
    const std::vector<std::string>& getRefinedSubjectMaskLabels() const {
        return data_.refined_subject_mask_labels;
    }
    const std::vector<uint8_t>& getRefinedSubjectMaskAvailableChannels() const {
        return data_.refined_subject_mask_available_channels;
    }
    const std::vector<ZarrDetectionData::RefinedSubjectMaskComponentInfo>&
    getRefinedSubjectMaskOverlayComponents() const {
        return data_.refined_subject_mask_overlay_components;
    }
    struct RefinedSubjectMaskComponentRow {
        bool valid = false;
        std::string run_name;
        std::string component_name;
        size_t channel_index = std::numeric_limits<size_t>::max();
        size_t roi_index = 0;
        size_t rows = 0;
        size_t cols = 0;
        std::vector<uint8_t> mask;
    };
    bool readRefinedSubjectMaskComponentRow(
        size_t roi_index,
        const std::string& component_name,
        RefinedSubjectMaskComponentRow& out_row,
        std::string* error_message = nullptr) const;
    bool hasEyeAngleData() const { return data_.has_eye_angles; }
    const std::string& getEyeAngleRunName() const { return data_.eye_angle_run_name; }
    bool hasEyeAngleAnalysisData() const {
        return data_.eye_angle_analysis.loaded;
    }
    const ZarrDetectionData::EyeAngleAnalysisData& getEyeAngleAnalysisData() const {
        return data_.eye_angle_analysis;
    }
    const ZarrDetectionData::EyeAngleScalarField*
    findEyeAngleScalarField(const std::string& field_name) const;
    const ZarrDetectionData::EyeAngleVectorField*
    findEyeAngleVectorField(const std::string& field_name) const;
    bool hasEyeVergenceFrame() const { return data_.has_eye_vergence_frame; }
    const std::vector<float>& getEyeVergenceFrameSignedDeg() const {
        static const std::vector<float> kEmpty;
        return data_.has_eye_vergence_frame ? data_.eye_vergence_signed_frame_deg : kEmpty;
    }
    const std::vector<float>& getEyeVergenceFrameTimeSeconds() const {
        static const std::vector<float> kEmpty;
        return data_.has_eye_vergence_frame ? data_.eye_vergence_frame_time_seconds : kEmpty;
    }
    const std::vector<uint8_t>& getEyeVergenceFrameValidMask() const {
        static const std::vector<uint8_t> kEmpty;
        return data_.has_eye_vergence_frame ? data_.eye_vergence_frame_valid : kEmpty;
    }
    bool hasStimulusAlignment() const { return data_.has_stimulus_alignment_data; }
    const std::string& getStimulusVideoPath() const { return data_.stimulus_video_path; }
    const std::string& getStimulusSourceH5() const { return data_.stimulus_source_h5; }
    bool hasStimulusEvents() const { return data_.has_stimulus_events; }
    std::vector<std::string> getStimulusEventsForFrame(size_t frame_id) const;
    using StimulusEventSummary = ZarrDetectionData::StimulusEventSummary;
    const std::vector<StimulusEventSummary>& getStimulusEventTimeline() const;
    size_t getStimulusEventTimelineGeneration() const {
        return data_.stimulus_event_timeline_generation;
    }
    bool hasStimulusSteps() const { return data_.has_stimulus_steps; }
    const std::vector<ZarrDetectionData::StimulusStep>& getStimulusSteps()
        const {
        return data_.stimulus_steps;
    }
    const std::string& getStimulusStepsRunName() const {
        return data_.stimulus_steps_run_name;
    }
    const ZarrDetectionData::StimulusStep* getStimulusStepForFrame(
        int32_t camera_frame) const;
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
    bool hasReviewStatus() const { return data_.has_review_status; }
    std::string getReviewState() const { return data_.review_state; }
    std::string getReviewMethod() const { return data_.review_method; }
    std::string getReviewIntendedUse() const { return data_.review_intended_use; }
    std::string getReviewTimestamp() const { return data_.review_timestamp; }
    std::string getReviewReviewer() const { return data_.review_reviewer; }
    std::string getReviewNotes() const { return data_.review_notes; }
    std::vector<ReviewArtifactSummary> getAvailableReviewArtifacts() const {
        std::vector<ReviewArtifactSummary> artifacts;

        if (!data_.detect_run_name.empty() || data_.has_review_status) {
            artifacts.push_back(ReviewArtifactSummary{
                ReviewArtifactKind::Detection,
                "Detection Review",
                data_.detect_run_name,
                data_.review_state,
                data_.review_method,
                data_.review_intended_use,
                data_.review_timestamp,
                data_.review_reviewer,
                data_.review_notes,
                data_.has_review_status,
            });
        }

        if (!data_.keypoints_run_name.empty() || data_.has_kp_review_status) {
            artifacts.push_back(ReviewArtifactSummary{
                ReviewArtifactKind::Keypoint,
                "Keypoint Review",
                data_.keypoints_run_name,
                data_.kp_review_state,
                data_.kp_review_method,
                data_.kp_review_intended_use,
                data_.kp_review_timestamp,
                data_.kp_review_reviewer,
                data_.kp_review_notes,
                data_.has_kp_review_status,
            });
        }

        if (!data_.eye_masks_run_name.empty()) {
            artifacts.push_back(ReviewArtifactSummary{
                ReviewArtifactKind::EyeMask,
                data_.eye_masks_from_refined_subject_masks
                    ? "Refined Subject Mask Review"
                    : "Eye Mask Review",
                data_.eye_masks_run_name,
                std::string{},
                std::string{},
                std::string{},
                std::string{},
                std::string{},
                std::string{},
                false,
            });
        }

        return artifacts;
    }
    std::string getStimulusRunName() const {
        return data_.has_interpolation ? data_.latest_interpolation.stimulus_run_name : "";
    }
    bool hasStimulusFrameMapping() const;
    bool hasCorrectedStimulusFrameMapping() const;
    int64_t getStimulusCameraFrameOffset() const;
    std::optional<int32_t> getStimulusMetadataIndexForCameraFrame(
        int32_t camera_frame,
        bool prefer_corrected = true) const;
    std::optional<int32_t> getStimulusFrameForCameraFrame(
        int32_t camera_frame,
        bool prefer_corrected = true) const;
    std::optional<int32_t> getCameraFrameForStimulusFrame(
        int32_t stimulus_frame,
        bool prefer_corrected = true) const;
    std::optional<int32_t> getFirstCameraFrameWithStimulus(
        bool prefer_corrected = true) const;
    std::optional<int32_t> getFirstStimulusFrameNumber(
        bool prefer_corrected = true) const;
    
    // Movement analysis accessors
    bool hasMovementData() const {
        return getSelectedMovementSeries() != nullptr;
    }
    bool hasDeferredMovementData() const {
        return movement_data_discovered_ && !hasMovementData();
    }
    bool loadDeferredMovementData(std::string* error_message = nullptr);
    bool startDeferredMovementDataLoad(std::string* error_message = nullptr);
    bool updateDeferredMovementDataLoad(std::string* error_message = nullptr);
    bool isDeferredMovementDataLoadInProgress() const;
    const std::string& getDeferredMovementLoadError() const {
        return movement_data_load_error_;
    }
    const std::string& getDeferredMovementLoadStatus() const {
        return movement_data_load_status_;
    }
    const std::vector<float>& getMovementTimeSeconds() const {
        static const std::vector<float> kEmpty;
        const auto* series = getSelectedMovementSeries();
        return series ? series->time_seconds : kEmpty;
    }
    const std::vector<float>& getMovementSmoothedSpeedMm() const {
        static const std::vector<float> kEmpty;
        const auto* series = getSelectedMovementSeries();
        return series ? series->smoothed_speed_mm : kEmpty;
    }
    const std::vector<float>& getMovementInstantaneousSpeedMm() const {
        static const std::vector<float> kEmpty;
        const auto* series = getSelectedMovementSeries();
        return series ? series->instant_speed_mm : kEmpty;
    }
    const std::vector<float>& getMovementDistanceToTargetMm() const {
        static const std::vector<float> kEmpty;
        const auto* series = getSelectedMovementSeries();
        return series ? series->distance_to_target_mm : kEmpty;
    }
    const std::vector<float>& getMovementHeadingDegrees() const {
        static const std::vector<float> kEmpty;
        const auto* series = getSelectedMovementSeries();
        return series ? series->heading_degrees : kEmpty;
    }
    const std::vector<float>& getMovementSmoothedHeadingDegrees() const {
        static const std::vector<float> kEmpty;
        const auto* series = getSelectedMovementSeries();
        return series ? series->smoothed_heading_degrees : kEmpty;
    }
    const std::vector<uint8_t>& getMovementHeadingKeypointSuccess() const {
        static const std::vector<uint8_t> kEmpty;
        const auto* series = getSelectedMovementSeries();
        return series ? series->keypoint_success : kEmpty;
    }
    const std::vector<float>& getMovementHeadingPerSecondDegrees() const {
        static const std::vector<float> kEmpty;
        const auto* series = getSelectedMovementSeries();
        return series ? series->heading_per_second_degrees : kEmpty;
    }
    const std::vector<float>& getMovementHeadingPerSecondResultant() const {
        static const std::vector<float> kEmpty;
        const auto* series = getSelectedMovementSeries();
        return series ? series->heading_per_second_resultant : kEmpty;
    }
    const std::vector<float>& getMovementHeadingPerSecondTimeSeconds() const {
        static const std::vector<float> kEmpty;
        const auto* series = getSelectedMovementSeries();
        return series ? series->heading_per_second_time_seconds : kEmpty;
    }
    const std::vector<int32_t>& getMovementFrameIndices() const {
        static const std::vector<int32_t> kEmpty;
        const auto* series = getSelectedMovementSeries();
        return series ? series->frame_indices : kEmpty;
    }
    const std::vector<int32_t>& getMovementDetectionIndices() const {
        static const std::vector<int32_t> kEmpty;
        const auto* series = getSelectedMovementSeries();
        return series ? series->detection_indices : kEmpty;
    }
    const std::string& getMovementRunName() const {
        static const std::string kEmpty;
        const auto* series = getSelectedMovementSeries();
        return series ? series->run_name : kEmpty;
    }
    const std::string& getMovementTrackId() const {
        static const std::string kEmpty;
        const auto* series = getSelectedMovementSeries();
        return series ? series->track_id : kEmpty;
    }
    const std::string& getMovementSpeedLevel() const {
        static const std::string kEmpty;
        const auto* series = getSelectedMovementSeries();
        return series ? series->speed_level : kEmpty;
    }
    const std::string& getMovementPrimarySpeedLabel() const {
        static const std::string kDefault = "Smoothed Speed";
        const auto* series = getSelectedMovementSeries();
        return series && !series->primary_speed_label.empty()
                   ? series->primary_speed_label
                   : kDefault;
    }
    const std::string& getMovementPrimarySpeedUnits() const {
        static const std::string kDefault = "mm/s";
        const auto* series = getSelectedMovementSeries();
        return series && !series->primary_speed_units.empty()
                   ? series->primary_speed_units
                   : kDefault;
    }
    const std::string& getMovementSecondarySpeedLabel() const {
        static const std::string kDefault = "Instantaneous Speed";
        const auto* series = getSelectedMovementSeries();
        return series && !series->secondary_speed_label.empty()
                   ? series->secondary_speed_label
                   : kDefault;
    }
    const std::string& getMovementSecondarySpeedUnits() const {
        static const std::string kDefault = "mm/s";
        const auto* series = getSelectedMovementSeries();
        return series && !series->secondary_speed_units.empty()
                   ? series->secondary_speed_units
                   : kDefault;
    }
    struct MovementFrameSample {
        bool valid = false;
        int32_t frame_index = -1;
        size_t row_index = 0;
        bool has_position_px = false;
        float x_px = std::numeric_limits<float>::quiet_NaN();
        float y_px = std::numeric_limits<float>::quiet_NaN();
        bool has_heading = false;
        bool heading_smoothed = false;
        float heading_degrees = std::numeric_limits<float>::quiet_NaN();
        bool has_speed = false;
        float speed = std::numeric_limits<float>::quiet_NaN();
        std::string speed_label;
        std::string speed_units;
        std::string category;
        std::string run_name;
        std::string track_id;
        std::string speed_level;
        bool has_sample_valid = false;
        bool sample_valid = false;
        bool has_transition_valid = false;
        bool transition_valid = false;
    };
    struct MovementTrailPoint {
        int32_t frame_index = -1;
        size_t row_index = 0;
        float x_px = std::numeric_limits<float>::quiet_NaN();
        float y_px = std::numeric_limits<float>::quiet_NaN();
        float age_seconds = 0.0f;
        float alpha = 1.0f;
        bool sample_valid = true;
        bool transition_valid = true;
        bool break_before = false;
    };
    std::optional<MovementFrameSample> getMovementSampleForFrame(
        int32_t frame_index) const;
    std::vector<MovementTrailPoint> getMovementTrailForFrame(
        int32_t frame_index,
        double duration_seconds,
        bool valid_samples_only) const;
    const std::vector<int32_t>& getCropFrameIndices() const {
        static const std::vector<int32_t> kEmpty;
        return data_.crop_data.metadata_loaded ? data_.crop_data.frame_indices
                                               : kEmpty;
    }
    bool hasCropImages() const { return data_.crop_data.loaded; }
    struct CropImageView {
        const uint8_t* data = nullptr;
        size_t width = 0;
        size_t height = 0;
        size_t channels = 0;
        size_t stride = 0;
        int32_t roi_index = -1;
    };
    bool getCropImageForIndex(int32_t roi_index, CropImageView& out_view) const;
    struct KeypointRoiMetadata {
        bool valid = false;
        bool has_crop_metadata = false;
        int32_t roi_index = -1;
        float offset_x = std::numeric_limits<float>::quiet_NaN();
        float offset_y = std::numeric_limits<float>::quiet_NaN();
        float roi_width = 0.0f;
        float roi_height = 0.0f;
    };
    KeypointRoiMetadata getCropRoiMetadataForRoiIndex(int32_t roi_index) const;
    KeypointRoiMetadata getKeypointRoiMetadataForFrameDetection(
        size_t frame_id,
        size_t detection_idx,
        bool use_interpolated = false) const;
    KeypointRoiMetadata getMaskRoiMetadataForFrameDetection(
        size_t frame_id,
        size_t detection_idx,
        bool use_interpolated = false) const;
    const std::string& getMovementCategory() const {
        static const std::string kEmpty;
        const auto* series = getSelectedMovementSeries();
        return series ? series->category : kEmpty;
    }
    const std::vector<ZarrDetectionData::SwimBoutSeries>& getSwimBoutSeries()
        const {
        return data_.swim_bout_series;
    }
    const std::vector<ZarrDetectionData::BoutKinematicsSeries>&
    getBoutKinematicsSeries() const {
        return data_.bout_kinematics_series;
    }
    bool ensureBoutKinematicsMetricsLoaded(const std::string& run_name,
                                           std::string* error_message = nullptr);
    size_t getMovementSeriesCount() const;
    const ZarrDetectionData::MovementSeries* getMovementSeries(size_t index) const;
    size_t getSelectedMovementSeriesIndex() const;
    const ZarrDetectionData::MovementSeries* getSelectedMovementSeries() const;
    bool selectMovementSeries(size_t index);

    // Stimulus chaser overlays (placeholder implementations)
    struct ChaserBoundingBox {
        int32_t fish_id = -1;
        float x_px = 0.0f;
        float y_px = 0.0f;
        float width_px = 0.0f;
        float height_px = 0.0f;
        float centroid_x = 0.0f;
        float centroid_y = 0.0f;
        float confidence = std::numeric_limits<float>::quiet_NaN();
        int32_t camera_frame_id = -1;
        int32_t stimulus_frame_num = -1;
        int32_t chaser_index = -1;
        bool is_target = false;
    };
    struct ChaserState {
        int32_t stimulus_frame_num = -1;
        int32_t camera_frame_id = -1;
        int32_t chaser_index = -1;
        float chaser_pos_x = 0.0f;
        float chaser_pos_y = 0.0f;
        float target_pos_x = 0.0f;
        float target_pos_y = 0.0f;
        float chaser_radius_px = std::numeric_limits<float>::quiet_NaN();
        float distance_to_target_px = std::numeric_limits<float>::quiet_NaN();
        float target_speed_px_per_s = std::numeric_limits<float>::quiet_NaN();
        bool is_chasing = false;
        int64_t timestamp_ns_session = 0;
        bool texture_space = true;
        std::string coordinate_frame;
        std::string coordinate_origin;
        double stimulus_canvas_offset_x = 0.0;
        double stimulus_canvas_offset_y = 0.0;
        bool has_stimulus_canvas_offset = false;
        double chaser_camera_x = std::numeric_limits<double>::quiet_NaN();
        double chaser_camera_y = std::numeric_limits<double>::quiet_NaN();
        double target_camera_x = std::numeric_limits<double>::quiet_NaN();
        double target_camera_y = std::numeric_limits<double>::quiet_NaN();
        bool has_camera_coords = false;
        std::array<float, 4> chaser_rgba = {1.0f, 0.0f, 0.0f, 1.0f};
        bool has_chaser_rgba = false;
        int32_t behavior_mode = -1;
        bool has_behavior_mode = false;
        bool enable_chase = false;
        bool has_enable_chase = false;
        bool enable_random_movement = false;
        bool has_enable_random_movement = false;
    };
    std::vector<ChaserBoundingBox> getChaserBoundingBoxesForFrame(size_t frame_id) const;
    std::vector<ChaserState> getChaserStatesForFrame(size_t frame_id) const;
    std::vector<ChaserState> getChaserStatesForStimulusFrame(int32_t stimulus_frame) const;
    std::vector<ChaserState> getChaserInterpolatedStatesForCameraFrame(int32_t camera_frame) const;
    std::vector<ChaserState> getChaserInterpolatedStatesForStimulusFrame(int32_t stimulus_frame) const;
    
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
        std::vector<uint8_t> detection_source;
        std::vector<std::string> detection_reason;
        std::vector<std::vector<std::array<float, 2>>> keypoints_pixels;
        std::vector<std::string> keypoint_labels;
        std::vector<std::array<size_t, 2>> skeleton_edges;
        size_t keypoints_per_detection = 0;
        bool has_keypoints = false;
        bool is_refined_keypoints = false;
        std::vector<int32_t> keypoint_quality_labels;
        std::vector<std::string> keypoint_reason;
        std::vector<uint8_t> keypoint_flip_corrected;
        std::vector<uint8_t> keypoint_usable;
        std::vector<uint8_t> keypoint_refined_success;
        std::vector<uint8_t> keypoint_detection_source;
        struct EyeMask {
            bool valid = false;
            int rows = 0;
            int cols = 0;
            float offset_x = std::numeric_limits<float>::quiet_NaN();
            float offset_y = std::numeric_limits<float>::quiet_NaN();
            float roi_width = 0.0f;
            float roi_height = 0.0f;
            int32_t roi_index = -1;
            std::array<std::vector<uint32_t>, 2> pixel_indices;
            struct AxisSegment {
                bool valid = false;
                float x0 = 0.0f;
                float y0 = 0.0f;
                float x1 = 0.0f;
                float y1 = 0.0f;
            };
            std::array<AxisSegment, 2> feret_major;
            std::array<AxisSegment, 2> feret_minor;
            bool has_feret_axes = false;
            std::array<float, 2> feret_minor_angle_deg = {
                std::numeric_limits<float>::quiet_NaN(),
                std::numeric_limits<float>::quiet_NaN()};
            std::array<uint8_t, 2> feret_angle_valid = {0, 0};
            bool has_eye_angles = false;
            std::array<float, 2> eye_frame_angle_deg = {
                std::numeric_limits<float>::quiet_NaN(),
                std::numeric_limits<float>::quiet_NaN()};
            std::array<uint8_t, 2> eye_frame_angle_valid = {0, 0};
            float eye_frame_vergence_deg =
                std::numeric_limits<float>::quiet_NaN();
            uint8_t eye_frame_vergence_valid = 0;
            bool has_eye_frame_angles = false;
            std::array<std::array<float, 2>, 2> gaze_vector_xy = {{
                {std::numeric_limits<float>::quiet_NaN(),
                 std::numeric_limits<float>::quiet_NaN()},
                {std::numeric_limits<float>::quiet_NaN(),
                 std::numeric_limits<float>::quiet_NaN()},
            }};
            std::array<uint8_t, 2> gaze_vector_valid = {0, 0};
            bool has_gaze_vectors = false;
            struct SubjectMaskComponent {
                std::string label;
                size_t channel_index = std::numeric_limits<size_t>::max();
                std::vector<uint32_t> pixel_indices;
                std::vector<std::array<float, 2>> contour_xy;
                bool valid = false;
                bool has_contour = false;
            };
            std::vector<SubjectMaskComponent> subject_mask_components;
            bool has_subject_mask_components = false;
        };
        std::vector<EyeMask> eye_masks;
        bool includes_eye_masks = false;
        struct SubjectShape {
            bool valid = false;
            int32_t roi_index = -1;
            float offset_x = std::numeric_limits<float>::quiet_NaN();
            float offset_y = std::numeric_limits<float>::quiet_NaN();
            float roi_width = 0.0f;
            float roi_height = 0.0f;
            float coordinate_width = 0.0f;
            float coordinate_height = 0.0f;

            bool body_frame_valid = false;
            std::array<float, 2> body_origin_xy = {
                std::numeric_limits<float>::quiet_NaN(),
                std::numeric_limits<float>::quiet_NaN()};
            std::array<float, 2> body_forward_axis_xy = {
                std::numeric_limits<float>::quiet_NaN(),
                std::numeric_limits<float>::quiet_NaN()};
            std::array<float, 2> body_left_axis_xy = {
                std::numeric_limits<float>::quiet_NaN(),
                std::numeric_limits<float>::quiet_NaN()};
            std::string body_frame_failure_reason;

            bool snout_tip_valid = false;
            std::array<float, 2> snout_tip_xy = {
                std::numeric_limits<float>::quiet_NaN(),
                std::numeric_limits<float>::quiet_NaN()};
            std::string snout_tip_failure_reason;
            bool tail_base_valid = false;
            std::array<float, 2> tail_base_xy = {
                std::numeric_limits<float>::quiet_NaN(),
                std::numeric_limits<float>::quiet_NaN()};
            std::array<float, 2> tail_tip_xy = {
                std::numeric_limits<float>::quiet_NaN(),
                std::numeric_limits<float>::quiet_NaN()};
            bool centerline_valid = false;
            bool centerline_reaches_snout = false;
            std::string centerline_failure_reason;
            bool bspline_valid = false;
            std::string bspline_failure_reason;
            bool tail_sample_valid = false;
            std::string tail_sample_failure_reason;
            bool caudal_contour_valid = false;
            std::array<float, 2> caudal_contour_point_xy = {
                std::numeric_limits<float>::quiet_NaN(),
                std::numeric_limits<float>::quiet_NaN()};

            std::vector<std::array<float, 2>> centerline_xy;
            std::vector<std::array<float, 2>> bspline_sample_xy;
            std::vector<std::array<float, 2>> bspline_control_points_xy;
            std::vector<std::array<float, 2>> tail_sample_xy;
            std::vector<std::array<float, 2>> tail_normal_xy;
        };
        std::vector<SubjectShape> subject_shapes;
        bool includes_subject_shapes = false;
    };
    FrameDetections getRawDetections(size_t frame_id,
                                     bool use_interpolated = true,
                                     bool include_eye_masks = false,
                                     bool include_subject_shapes = false,
                                     bool suppress_subject_mask_smoke_log =
                                         false,
                                     bool allow_blocking_eye_mask_load =
                                         true) const;

    struct SubjectShapeQcFilterOptions {
        bool any_invalid = true;
        bool source_mask_qc_failure = false;
        bool body_frame_invalid = false;
        bool snout_invalid = false;
        bool centerline_invalid = false;
        bool centerline_misses_snout = false;
        bool bspline_invalid = false;
        bool tail_base_invalid = false;
        bool tail_sample_invalid = false;
        std::string reason_substring;
    };
    struct SubjectShapeQcJumpResult {
        std::optional<int> target_frame;
        size_t match_count = 0;
        std::string status;
    };
    SubjectShapeQcJumpResult computeSubjectShapeQcJump(
        const SubjectShapeQcFilterOptions& filters,
        int current_frame_num,
        bool forward) const;

    struct TailKinematicsQcFilterOptions {
        bool invalid_rows = true;
        bool nonfinite_tail_tip_angle = false;
        bool nonfinite_tail_tip_lateral_deflection = false;
        std::string reason_substring;
    };
    struct TailKinematicsQcJumpResult {
        std::optional<int> target_frame;
        std::optional<size_t> target_row;
        size_t match_count = 0;
        std::string status;
    };
    TailKinematicsQcJumpResult computeTailKinematicsQcJump(
        const TailKinematicsQcFilterOptions& filters,
        int current_frame_num,
        bool forward) const;
    std::optional<size_t> findTailKinematicsRowForFrame(int frame) const;
    std::optional<int32_t> getTailKinematicsFrameForRow(size_t row) const;
    struct EyeAngleQcFilterOptions {
        bool invalid_rows = true;
        bool major_axis_marginal = false;
        std::string reason_substring;
    };
    struct EyeAngleQcJumpResult {
        std::optional<int> target_frame;
        std::optional<size_t> target_row;
        size_t match_count = 0;
        std::string status;
    };
    EyeAngleQcJumpResult computeEyeAngleQcJump(
        const EyeAngleQcFilterOptions& filters,
        int current_frame_num,
        bool forward) const;
    std::optional<size_t> findEyeAngleRowForFrame(int frame) const;
    std::optional<int32_t> getEyeAngleFrameForRow(size_t row) const;
    int32_t getKeypointRoiIndexForFrameDetection(size_t frame_id,
                                                 size_t detection_idx,
                                                 bool use_interpolated = false) const;

    struct RefinedKeypointCacheUpdate {
        bool valid = false;
        size_t frame_id = 0;
        size_t detection_index = 0;
        int32_t roi_index = -1;
        std::vector<std::array<float, 2>> keypoints_img;
        float heading_deg = std::numeric_limits<float>::quiet_NaN();
        std::array<float, 2> heading_origin_img = {
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::quiet_NaN()};
        bool heading_valid = false;
        int32_t quality_label = -1;
        std::string reason;
        uint8_t flip_corrected = 0;
        uint8_t usable = 0;
        uint8_t confidence_valid = 0;
        uint8_t geometry_valid = 0;
        uint8_t refined_success = 0;
    };
    bool applyRefinedKeypointCacheUpdate(
        const RefinedKeypointCacheUpdate& update,
        std::string* error_message = nullptr);

    bool hasHeadingData() const { return data_.has_heading_data; }
    bool hasKeypointData() const { return data_.has_keypoints; }
    bool hasSubjectShapeData() const { return data_.subject_shape.loaded; }
    bool hasTailKinematicsData() const { return data_.tail_kinematics.loaded; }
    const std::string& getSubjectShapeRunName() const {
        return data_.subject_shape.run_name;
    }
    const std::string& getSubjectShapeSourceRefinedSubjectMasksRun() const {
        return data_.subject_shape.source_refined_subject_masks_run;
    }
    const std::string& getSubjectShapeWarning() const {
        return data_.subject_shape.warning;
    }
    int getSubjectShapeSchemaVersion() const {
        return data_.subject_shape.schema_version;
    }
    int getSubjectShapeMethodVersion() const {
        return data_.subject_shape.method_version;
    }
    const std::string& getSubjectShapeMethod() const {
        return data_.subject_shape.method;
    }
    const std::string& getSubjectShapeHeadEndpointSemantics() const {
        return data_.subject_shape.head_endpoint_semantics;
    }
    size_t getSubjectShapeRowCount() const {
        return data_.subject_shape.row_count;
    }
    const ZarrDetectionData::TailKinematicsData& getTailKinematicsData() const {
        return data_.tail_kinematics;
    }
    const std::string& getTailKinematicsRunName() const {
        return data_.tail_kinematics.run_name;
    }
    const std::string& getTailKinematicsSourceSubjectShapeRun() const {
        return data_.tail_kinematics.source_subject_shape_run;
    }
    const std::string& getTailKinematicsSourceRefinedSubjectMasksRun() const {
        return data_.tail_kinematics.source_refined_subject_masks_run;
    }
    const std::string& getTailKinematicsWarning() const {
        return data_.tail_kinematics.warning;
    }
    size_t getTailKinematicsRowCount() const {
        return data_.tail_kinematics.row_count;
    }
    size_t getTailKinematicsSampleCount() const {
        return data_.tail_kinematics.sample_count;
    }
    const std::vector<std::string>& getKeypointLabels() const {
        return data_.keypoint_labels;
    }
    const std::vector<std::array<size_t, 2>>& getSkeletonEdges() const {
        return data_.skeleton_edges;
    }
    const KeypointHeadingComputationSpec& getHeadingComputationSpec() const {
        return data_.heading_computation_spec;
    }
    bool hasDetectionData() const {
        return data_.has_raw_detection_dataset ||
               !data_.frame_offsets.empty() ||
               !data_.n_detections.empty() ||
               !data_.bbox_norm_coords.empty() ||
               data_.bboxes_store.valid();
    }

    bool writeManualRefinedDetections(
        const std::vector<int32_t>& frame_indices,
        const std::vector<std::array<double, 4>>& bbox_norm_coords,
        const std::vector<float>& scores,
        const std::vector<int32_t>& class_ids,
        const std::vector<int32_t>& frame_counts,
        const std::vector<int8_t>& detection_source,
        const std::vector<std::string>& reason_labels,
        const std::string& manual_group,
        const std::string& source_variant,
        std::string& error_message,
        std::string* resolved_refined_run = nullptr,
        const ManualWriteReviewOptions& review_options = ManualWriteReviewOptions{});
    
    // Static helper to find zarr files in a directory
    static std::optional<std::string> findZarrDetectionFile(const std::string& directory);
    
private:
    ZarrDetectionData data_;
    PaletteClippedResolver clipped_resolver_;
    ts::Context context_;
    std::string root_path_;
    std::string requested_subject_shape_run_name_;
    std::string requested_refined_subject_mask_run_name_;
    std::string requested_refined_subject_mask_storage_;
    std::string requested_tail_kinematics_run_name_;
    std::string requested_eye_angle_run_name_;
    std::string requested_stimulus_run_name_;
    DetectionDataset active_dataset_ = DetectionDataset::RawDetect;
    bool movement_data_discovered_ = false;
    std::string movement_data_load_error_;
    enum class MovementLoadStage {
        Full,
        TrackKinematics,
        SwimBouts,
        BoutKinematics,
    };
    struct MovementLoadResult {
        bool ok = false;
        MovementLoadStage stage = MovementLoadStage::Full;
        uint64_t generation = 0;
        std::string archive_path;
        std::string error_message;
        double elapsed_ms = 0.0;
        bool has_movement_data = false;
        size_t movement_selected_index = std::numeric_limits<size_t>::max();
        std::vector<ZarrDetectionData::MovementSeries> movement_series;
        std::vector<ZarrDetectionData::SwimBoutSeries> swim_bout_series;
        std::vector<ZarrDetectionData::BoutKinematicsSeries>
            bout_kinematics_series;
        std::string movement_crop_run_name;
        ZarrDetectionData::CropImageData crop_data;
    };
    uint64_t movement_data_load_generation_ = 0;
    std::string movement_data_load_status_;
    std::future<MovementLoadResult> movement_data_load_future_;
    mutable std::mutex refined_subject_mask_optional_overlay_mutex_;
    mutable std::thread refined_subject_mask_optional_overlay_worker_;
    uint64_t refined_subject_mask_optional_overlay_generation_ = 0;
    bool refined_subject_mask_optional_overlay_requested_ = false;
    bool refined_subject_mask_optional_overlay_loading_ = false;
    bool refined_subject_mask_optional_overlay_loaded_ = false;
    bool refined_subject_mask_optional_overlay_failed_ = false;
    std::string refined_subject_mask_optional_overlay_error_;
    mutable size_t refined_subject_mask_optional_overlay_publish_generation_ = 0;
    mutable std::mutex eye_mask_prefetch_mutex_;
    mutable std::condition_variable eye_mask_prefetch_cv_;
    mutable std::deque<size_t> eye_mask_prefetch_queue_;
    mutable std::thread eye_mask_prefetch_worker_;
    mutable bool eye_mask_prefetch_stop_requested_ = false;
    
    // Loading functions
    bool loadStandardFormat(const ts::kvstore::KvStore& store);
    bool loadMetadata(const ts::kvstore::KvStore& store);
    bool loadBoundingBoxes(const ts::kvstore::KvStore& store, const std::string& path);
    bool loadScores(const ts::kvstore::KvStore& store, const std::string& path);
    bool loadClassIDs(const ts::kvstore::KvStore& store, const std::string& path);
    bool loadNDetections(const ts::kvstore::KvStore& store, const std::string& path);
    
    // Palette layout loaders
    bool loadDetectionRuns(const ts::kvstore::KvStore& store);
    bool loadClippedRefinedCollectionAsPrimary(const ts::kvstore::KvStore& store);
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
                          std::vector<std::string>* detection_reason_out,
                          bool& has_scores_out,
                          bool& has_class_ids_out,
                          size_t& resolved_frames_out);

    bool readInt32Array(const ts::kvstore::KvStore& store, const std::string& path, std::vector<int32_t>& out);
    bool readInt64Array(const ts::kvstore::KvStore& store, const std::string& path, std::vector<int64_t>& out);
    bool readFloatArray(const ts::kvstore::KvStore& store, const std::string& path, std::vector<float>& out);
    bool readFloatMatrix(const ts::kvstore::KvStore& store, const std::string& path, std::vector<std::array<float, 4>>& out);
    bool readBoolArray(const ts::kvstore::KvStore& store, const std::string& path, std::vector<uint8_t>& out);
    bool readStringArray(const ts::kvstore::KvStore& store, const std::string& path, std::vector<std::string>& out);

    // New: Load interpolation data
    bool loadInterpolationRuns(const ts::kvstore::KvStore& store);
    bool loadRefinedDetectRuns(const ts::kvstore::KvStore& store);
    bool loadRefinedDetectionsAsPrimary(const ts::kvstore::KvStore& store);
    bool loadStimulusAlignment(const ts::kvstore::KvStore& store);
    bool loadStimulusStepsForRun(const ts::kvstore::KvStore& store,
                                 const std::string& run_base,
                                 const std::string& run_name);
    bool loadEyeAngleData(const ts::kvstore::KvStore& store, size_t roi_count);
    bool loadStimulusEventsForRun(const ts::kvstore::KvStore& store, const std::string& run_base);
    void loadStimulusEventEnums(const ts::kvstore::KvStore& store);
    bool loadChaserStates(const ts::kvstore::KvStore& store, const std::string& run_base);
    bool loadChaserBoundingBoxes(const ts::kvstore::KvStore& store, const std::string& run_base);
    bool loadChaserStatesInterpolated(const ts::kvstore::KvStore& store,
                                      const std::string& run_base);
    bool loadStimulusFrameMetadataMapping(const ts::kvstore::KvStore& store,
                                          const std::string& run_base,
                                          std::vector<int32_t>& stimulus_to_camera);
    bool loadLatestInterpolationRun(const ts::kvstore::KvStore& store, const std::string& run_name);
    bool loadPaletteInterpolationRun(const ts::kvstore::KvStore& store,
                                     const std::string& run_name,
                                     const std::string& subgroup);
    bool loadKeypointHeadingData(const ts::kvstore::KvStore& store);
    bool loadSubjectShapeData(const ts::kvstore::KvStore& store);
    bool loadTailKinematicsData(const ts::kvstore::KvStore& store);
    bool loadRefinedSubjectMaskEyeData(const ts::kvstore::KvStore& store, size_t roi_count);
    bool loadRefinedSubjectMaskOptionalOverlayData(
        uint64_t generation,
        const std::string& archive_path,
        std::string* error_message);
    bool loadRefinedEyeMaskData(const ts::kvstore::KvStore& store, size_t roi_count);
    const ZarrDetectionData::EyeMaskChunkCacheEntry* findEyeMaskChunk(size_t chunk_id) const;
    bool ensureEyeMaskChunk(size_t chunk_id,
                            bool allow_prefetch = true,
                            bool force_reload = false) const;
    void prefetchAdjacentEyeMaskChunks(size_t chunk_id) const;
    bool requestEyeMaskChunkPrefetch(size_t chunk_id) const;
    void stopEyeMaskPrefetchWorker() const;
    void stopRefinedSubjectMaskOptionalOverlayWorker();
    void eyeMaskPrefetchWorkerLoop() const;
    bool populateEyeMaskEntry(size_t roi_index,
                              FrameDetections::EyeMask& out_mask,
                              bool allow_blocking_load = true,
                              bool request_prefetch_on_miss = true) const;
    bool populateSubjectShapeEntry(size_t roi_index,
                                   FrameDetections::SubjectShape& out_shape) const;
    bool loadMovementData(const ts::kvstore::KvStore& store);
    static MovementLoadResult loadMovementDataForArchive(
        const std::string& archive_path,
        const std::string& keypoints_source_crop_run,
        double fps,
        int image_width,
        int image_height,
        uint64_t generation);
    static MovementLoadResult loadMovementTrackDataForArchive(
        const std::string& archive_path,
        const std::string& keypoints_source_crop_run,
        double fps,
        int image_width,
        int image_height,
        uint64_t generation);
    static MovementLoadResult loadMovementSwimBoutDataForArchive(
        const std::string& archive_path,
        std::vector<ZarrDetectionData::MovementSeries> movement_stubs,
        uint64_t generation);
    static MovementLoadResult loadMovementBoutKinematicsDataForArchive(
        const std::string& archive_path,
        std::vector<ZarrDetectionData::MovementSeries> movement_stubs,
        std::vector<ZarrDetectionData::SwimBoutSeries> swim_bout_stubs,
        uint64_t generation);
    void publishMovementLoadResult(MovementLoadResult&& result);
    void startMovementDataLoadStage(MovementLoadStage stage);
    void waitForDeferredMovementDataLoad();
    bool discoverMovementData(const ts::kvstore::KvStore& store);
    bool loadSpeedRunMovement(const ts::kvstore::KvStore& store);
    bool loadTrackKinematicsData(const ts::kvstore::KvStore& store);
    bool loadSwimBoutData(const ts::kvstore::KvStore& store);
    bool loadBoutKinematicsData(const ts::kvstore::KvStore& store);
    bool loadBoutKinematicsMetrics(const ts::kvstore::KvStore& store,
                                   ZarrDetectionData::BoutKinematicsSeries& series,
                                   std::string* error_message = nullptr);
    bool loadLegacyMovementData(const ts::kvstore::KvStore& store);
    bool loadMovementTrack(const ts::kvstore::KvStore& store,
                           const std::string& log_tag,
                           const std::string& run_name,
                           const std::string& track_id,
                           const std::string& track_base,
                           const std::vector<std::string>& frame_names,
                           const std::vector<std::string>& time_float_names,
                           const std::vector<std::string>& timestamp_ns_names,
                           const std::vector<std::string>& smoothed_mm_names,
                           const std::vector<std::string>& smoothed_px_names,
                           const std::vector<std::string>& instant_mm_names,
                           const std::vector<std::string>& instant_px_names,
                          float pixels_per_mm,
                          double run_fps,
                          const std::string& category,
                          const std::string& detection_variant,
                          const std::string& source_detect_run,
                          double smoothing_seconds,
                           int video_width,
                           int video_height,
                           bool from_speed_runs,
                           const std::vector<int64_t>* run_camera_frame_ids,
                           const std::unordered_map<int64_t, size_t>* run_camera_lookup,
                           const std::vector<float>* run_distance_to_target_mm,
                           const std::vector<uint8_t>* run_has_offline_flags,
                           const std::string& speed_level = std::string(),
                           const std::string& primary_speed_label = std::string(),
                           const std::string& secondary_speed_label = std::string());
    bool loadMovementCropRun(const ts::kvstore::KvStore& store,
                             const std::string& crop_run_name);
    bool loadMovementCropRunMetadata(const ts::kvstore::KvStore& store,
                                     const std::string& crop_run_name);

    std::optional<json> readGroupAttrs(const ts::kvstore::KvStore& store,
                                       const std::string& path) const;
    void finalizeMovementSelection();
    void rebuildChaserStateIndices();
    void rebuildChaserBoundingBoxIndices();
    void rebuildInterpolatedChaserStateIndices();
    void updateChaserCameraFramesFromAlignment();
    void cacheDetectionStage(InterpolationRunData stage,
                             DetectionDataset dataset_type);
    bool applyDetectionDataset(const InterpolationRunData& stage,
                               DetectionDataset dataset_type);
    void computeDetectionsFromOffsets(const std::vector<size_t>& offsets,
                                      std::vector<int32_t>& n_detections_out) const;
    void computeActiveDatasetInterpolationFlags();
    
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
    std::string formatStimulusEvent(const ZarrDetectionData::EventLogEntry& entry) const;
    void clearStimulusEventTimelineCache();
    void rebuildStimulusEventTimelineCache(
        const std::vector<int32_t>* stimulus_to_camera_map = nullptr);

    std::optional<int32_t> resolveStimulusMetadataIndex(
        const std::vector<int32_t>& mapping,
        int32_t camera_frame) const;
    std::optional<int32_t> resolveStimulusFrame(
        const std::vector<int32_t>& mapping,
        const std::vector<int32_t>& frame_numbers,
        int32_t camera_frame) const;
    std::optional<int32_t> resolveDirectStimulusFrame(int32_t camera_frame) const;
};

// Standalone helper function
bool loadZarrDetectionFromPath(
    const std::string& zarr_path,
    ZarrDetectionLoader& loader,
    std::string& error_message
);

bool loadZarrDetectionFromDirectory(
    const std::string& dir_path,
    ZarrDetectionLoader& loader,
    std::string& error_message
);

#endif // ZARR_LOADER_H
