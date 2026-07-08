#include "zarr_loader_internal.h"
#include "debug_flags.h"
#include <iostream>
#include <thread>

namespace {

constexpr size_t kInvalidMaskChannel = std::numeric_limits<size_t>::max();

bool startupTraceEnabled() {
    return crimson_env_flag_enabled("CRIMSON_STARTUP_TRACE");
}

struct StartupTraceSection {
    explicit StartupTraceSection(const char* prefix)
        : enabled(startupTraceEnabled()),
          prefix(prefix),
          last(std::chrono::steady_clock::now()) {}

    void step(const char* label) {
        if (!enabled) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const double step_ms =
            std::chrono::duration<double, std::milli>(now - last).count();
        std::cout << "  [StartupTrace] " << prefix << "." << label
                  << " step_ms=" << step_ms << std::endl;
        last = now;
    }

    bool enabled = false;
    const char* prefix = "";
    std::chrono::steady_clock::time_point last;
};

void clearEyeMaskState(ZarrDetectionData& data) {
    data.eye_masks_run_name.clear();
    data.eye_masks_source_label.clear();
    data.eye_masks_source_path.clear();
    data.eye_masks_warning.clear();
    data.eye_masks_from_refined_subject_masks = false;
    data.eye_masks_tolerant_metadata = false;
    data.eye_masks_loaded = false;
    data.has_eye_masks = false;
    data.eye_masks_store = ts::TensorStore<uint8_t, 4>();
    data.eye_masks_bitpacked_store = ts::TensorStore<uint8_t, 4>();
    data.eye_mask_roi_count = 0;
    data.eye_mask_height = 0;
    data.eye_mask_width = 0;
    data.eye_mask_chunk_rows = 0;
    data.eye_mask_channel_indices = {kInvalidMaskChannel, kInvalidMaskChannel};
    data.eye_mask_channel_labels = {"eye_left", "eye_right"};
    data.refined_subject_mask_labels.clear();
    data.refined_subject_mask_available_channels.clear();
    data.refined_subject_mask_label_schema_id.clear();
    data.refined_subject_mask_source_crop_run.clear();
    data.refined_subject_mask_frame_indices.clear();
    data.refined_subject_mask_source_crop_row_ids.clear();
    data.refined_subject_mask_offset_x.clear();
    data.refined_subject_mask_offset_y.clear();
    data.refined_subject_mask_roi_width_px.clear();
    data.refined_subject_mask_roi_height_px.clear();
    data.refined_subject_mask_source_crop_frame_indices.clear();
    data.refined_subject_mask_crop_frame_match.clear();
    data.refined_subject_mask_rows_by_frame.clear();
    data.refined_subject_mask_row_position_fallback = false;
    data.refined_subject_mask_dense_masks_used = false;
    data.refined_subject_mask_bitpacked_masks_used = false;
    data.refined_subject_mask_rle_masks_used = false;
    data.refined_subject_mask_smoke_logged_frames.clear();
    data.refined_subject_mask_rle_smoke_log_count = 0;
    data.refined_subject_mask_overlay_components.clear();
    data.eye_mask_feret_axes_major.clear();
    data.eye_mask_feret_axes_minor.clear();
    data.eye_masks_have_feret_axes = false;
    std::lock_guard<std::mutex> cache_lock(*data.mask_chunk_cache_mutex);
    data.mask_chunk_cache.clear();
    data.mask_chunk_loads_in_flight.clear();
}

std::vector<std::string> extractStringListAttr(const nlohmann::json& attrs,
                                               const char* key) {
    std::vector<std::string> values;
    if (!attrs.contains(key) || !attrs[key].is_array()) {
        return values;
    }
    const auto& array = attrs[key];
    values.reserve(array.size());
    for (const auto& item : array) {
        if (item.is_string()) {
            values.push_back(item.get<std::string>());
        }
    }
    return values;
}

std::string joinMaskLabels(const std::vector<std::string>& labels) {
    std::ostringstream oss;
    for (size_t i = 0; i < labels.size(); ++i) {
        if (i > 0) {
            oss << ", ";
        }
        oss << labels[i];
    }
    return oss.str();
}

std::array<float, 4> invalidAxisSegment() {
    const float nan_value = std::numeric_limits<float>::quiet_NaN();
    return {nan_value, nan_value, nan_value, nan_value};
}

std::array<std::array<float, 4>, 2> invalidEyeAxisRow() {
    return {invalidAxisSegment(), invalidAxisSegment()};
}

void ensureEyeAxisRows(std::vector<std::array<std::array<float, 4>, 2>>& rows,
                       size_t roi_dim) {
    if (rows.size() == roi_dim) {
        return;
    }
    rows.assign(roi_dim, invalidEyeAxisRow());
}

bool stringAttrMatches(const nlohmann::json& attrs,
                       const char* key,
                       const char* expected) {
    return attrs.contains(key) &&
           attrs[key].is_string() &&
           attrs[key].get<std::string>() == expected;
}

std::string jsonStringAttr(const nlohmann::json& attrs,
                           const char* key) {
    if (attrs.contains(key) && attrs[key].is_string()) {
        return attrs[key].get<std::string>();
    }
    return {};
}

bool jsonBoolAttr(const nlohmann::json& attrs, const char* key) {
    if (!attrs.contains(key)) {
        return false;
    }
    const auto& value = attrs[key];
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer()) {
        return value.get<int64_t>() != 0;
    }
    if (value.is_string()) {
        std::string text = toLowerCopy(value.get<std::string>());
        return text == "1" || text == "true" || text == "yes" ||
               text == "y" || text == "on";
    }
    return false;
}

int jsonIntAttr(const nlohmann::json& attrs, const char* key) {
    if (attrs.contains(key) && attrs[key].is_number_integer()) {
        return attrs[key].get<int>();
    }
    if (attrs.contains(key) && attrs[key].is_number()) {
        return static_cast<int>(attrs[key].get<double>());
    }
    return -1;
}

std::vector<std::string> jsonStringVector(const nlohmann::json& obj,
                                          const char* key) {
    std::vector<std::string> values;
    if (!obj.contains(key) || !obj[key].is_array()) {
        return values;
    }
    for (const auto& item : obj[key]) {
        if (item.is_string()) {
            values.push_back(item.get<std::string>());
        }
    }
    return values;
}

std::vector<int64_t> jsonIntVector(const nlohmann::json& obj,
                                   const char* key) {
    std::vector<int64_t> values;
    if (!obj.contains(key) || !obj[key].is_array()) {
        return values;
    }
    for (const auto& item : obj[key]) {
        if (item.is_number_integer()) {
            values.push_back(item.get<int64_t>());
        } else if (item.is_number()) {
            values.push_back(static_cast<int64_t>(item.get<double>()));
        }
    }
    return values;
}

std::string safeMaskRleComponentName(const std::string& component_name) {
    std::string safe;
    safe.reserve(component_name.size());
    for (char ch : component_name) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) || ch == '_' || ch == '-' || ch == '.') {
            safe.push_back(ch);
        } else {
            safe.push_back('_');
        }
    }
    while (!safe.empty() && safe.front() == '_') {
        safe.erase(safe.begin());
    }
    while (!safe.empty() && safe.back() == '_') {
        safe.pop_back();
    }
    if (safe.empty()) {
        safe = "component";
    }
    return safe;
}

std::string maskRleComponentGroupName(size_t component_index,
                                      const std::string& component_name,
                                      bool zero_padded) {
    std::ostringstream oss;
    if (zero_padded) {
        oss << std::setw(2) << std::setfill('0') << component_index;
    } else {
        oss << component_index;
    }
    oss << "_" << safeMaskRleComponentName(component_name);
    return oss.str();
}

bool subjectMaskChunkPerfLogsEnabled() {
    static const bool enabled =
        crimson_env_flag_enabled("CRIMSON_SUBJECT_MASK_CHUNK_PERF");
    return enabled;
}

double elapsedMsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - start)
        .count();
}

void appendEyeAngleWarning(
    ZarrDetectionData::EyeAngleAnalysisData& eye_angles,
    const std::string& message) {
    if (message.empty()) {
        return;
    }
    if (!eye_angles.warning.empty()) {
        eye_angles.warning += " ";
    }
    eye_angles.warning += message;
}

void addUniqueString(std::vector<std::string>& values,
                     const std::string& value) {
    if (value.empty()) {
        return;
    }
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

bool endsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string unsmoothedBaseField(const std::string& field_name) {
    constexpr const char* kSmoothedSuffix = "_smoothed";
    if (!endsWith(field_name, kSmoothedSuffix)) {
        return {};
    }
    return field_name.substr(0, field_name.size() - std::strlen(kSmoothedSuffix));
}

ZarrDetectionData::EyeAngleFieldInfo makeEyeAngleFieldInfo(
    const std::string& name,
    const std::string& representation_key,
    const std::string& field_role,
    const std::string& display_name,
    const std::string& units,
    bool default_plot) {
    ZarrDetectionData::EyeAngleFieldInfo info;
    info.name = name;
    info.representation_key = representation_key;
    info.field_role = field_role;
    info.display_name = display_name;
    info.units = units;
    info.default_plot = default_plot;
    return info;
}

void appendCompatibilityEyeAngleSchema(
    ZarrDetectionData::EyeAngleAnalysisData& eye_angles) {
    eye_angles.variant_schema_inferred = true;
    eye_angles.variant_schema_id = "inferred.compatibility_eye_angle_variant_schema";
    eye_angles.default_representation = "eye_frame";
    eye_angles.representation_order = {
        "eye_frame", "gaze", "nasal_gaze", "major", "centroid", "legacy"};

    auto make_rep =
        [](const std::string& key,
           const std::string& display_name,
           std::vector<std::string> default_plot,
           std::vector<std::string> primary,
           std::vector<std::string> aggregate,
           std::vector<std::string> vectors = {})
            -> ZarrDetectionData::EyeAngleRepresentationInfo {
        ZarrDetectionData::EyeAngleRepresentationInfo rep;
        rep.key = key;
        rep.display_name = display_name;
        rep.units = "deg";
        rep.default_plot_fields = std::move(default_plot);
        rep.primary_roi_fields = std::move(primary);
        rep.aggregate_roi_fields = std::move(aggregate);
        rep.vector_roi_fields = std::move(vectors);
        return rep;
    };

    eye_angles.representations = {
        make_rep("eye_frame",
                 "Bianco/Engert eye-frame angles",
                 {"left_eye_angle_deg_smoothed",
                  "right_eye_angle_deg_smoothed",
                  "vergence_eye_angle_deg_smoothed"},
                 {"left_eye_angle_deg", "right_eye_angle_deg"},
                 {"vergence_eye_angle_deg"}),
        make_rep("gaze",
                 "Gaze direction",
                 {"left_gaze_signed_deg_smoothed",
                  "right_gaze_signed_deg_smoothed"},
                 {"left_gaze_signed_deg", "right_gaze_signed_deg"},
                 {"vergence_gaze_deg", "version_gaze_deg"},
                 {"left_gaze_xy", "right_gaze_xy"}),
        make_rep("nasal_gaze",
                 "BEAST/Johnson nasal-gaze convergence",
                 {"left_nasal_gaze_deg_smoothed",
                  "right_nasal_gaze_deg_smoothed",
                  "mean_eye_vergence_gaze_deg_smoothed"},
                 {"left_nasal_gaze_deg", "right_nasal_gaze_deg"},
                 {"mean_eye_vergence_gaze_deg"}),
        make_rep("major",
                 "Canonical major-axis orientation",
                 {"left_major_signed_deg", "right_major_signed_deg"},
                 {"left_major_signed_deg", "right_major_signed_deg"},
                 {"vergence_major_signed_deg", "version_major_deg"}),
        make_rep("centroid",
                 "Centroid-position diagnostics",
                 {"left_centroid_deg_smoothed",
                  "right_centroid_deg_smoothed"},
                 {"left_centroid_deg", "right_centroid_deg"},
                 {"vergence_centroid_deg"}),
        make_rep("legacy",
                 "Legacy compatibility aliases",
                 {},
                 {"left_deg", "right_deg", "left_signed_deg",
                  "right_signed_deg", "left_minor_signed_deg",
                  "right_minor_signed_deg"},
                 {"vergence_deg", "vergence_signed_deg", "version_deg",
                  "vergence_minor_signed_deg", "version_minor_deg"}),
    };
}

std::string decodeEyeAngleReasonCode(
    int32_t code,
    const std::unordered_map<int32_t, std::string>& reason_code_map) {
    if (code == 0) {
        return {};
    }
    std::vector<int32_t> bits;
    bits.reserve(reason_code_map.size());
    for (const auto& entry : reason_code_map) {
        if (entry.first > 0) {
            bits.push_back(entry.first);
        }
    }
    std::sort(bits.begin(), bits.end());
    std::ostringstream oss;
    int32_t known_bits = 0;
    bool first = true;
    for (int32_t bit : bits) {
        if ((code & bit) == 0) {
            continue;
        }
        if (!first) {
            oss << "|";
        }
        auto it = reason_code_map.find(bit);
        oss << (it != reason_code_map.end()
                    ? it->second
                    : ("reason_" + std::to_string(bit)));
        known_bits |= bit;
        first = false;
    }
    const int32_t unknown_bits = code & ~known_bits;
    if (unknown_bits != 0) {
        if (!first) {
            oss << "|";
        }
        oss << "unknown_" << unknown_bits;
    }
    return oss.str();
}

}  // namespace

bool ZarrDetectionLoader::loadKeypointHeadingData(const ts::kvstore::KvStore& store) {
    data_.flat_headings_deg.clear();
    data_.flat_swim_bladder_px.clear();
    data_.flat_heading_valid.clear();
    data_.has_heading_data = false;
    data_.keypoints_run_name.clear();
    data_.keypoints_source_crop_run.clear();
    data_.flat_keypoints_px.clear();
    data_.keypoint_roi_indices.clear();
    data_.keypoint_labels.clear();
    data_.heading_computation_spec = KeypointHeadingComputationSpec{};
    data_.keypoints_per_detection = 0;
    data_.has_keypoints = false;
    data_.mask_roi_indices.clear();
    data_.roi_offset_x.clear();
    data_.roi_offset_y.clear();
    data_.roi_width_px.clear();
    data_.roi_height_px.clear();
    stopEyeMaskPrefetchWorker();
    clearEyeMaskState(data_);
    data_.eye_angle_run_name.clear();
    data_.eye_angle_frame_indices.clear();
    data_.eye_angle_valid_mask.clear();
    data_.eye_angle_left_deg.clear();
    data_.eye_angle_right_deg.clear();
    data_.eye_angle_indices_by_frame.clear();
    data_.has_eye_angles = false;
    data_.has_eye_frame_angles = false;
    data_.eye_frame_left_angle_deg.clear();
    data_.eye_frame_right_angle_deg.clear();
    data_.eye_frame_vergence_deg.clear();
    data_.eye_vergence_signed_frame_deg.clear();
    data_.eye_vergence_frame_time_seconds.clear();
    data_.eye_vergence_frame_valid.clear();
    data_.has_eye_vergence_frame = false;
    data_.eye_angle_analysis = ZarrDetectionData::EyeAngleAnalysisData{};

    // Clear refined keypoint quality fields
    data_.is_refined_keypoints = false;
    data_.refined_keypoints_run_name.clear();
    data_.flat_keypoint_quality_labels.clear();
    data_.flat_keypoint_reason.clear();
    data_.flat_keypoint_flip_corrected.clear();
    data_.flat_keypoint_usable.clear();
    data_.flat_keypoint_confidence_valid.clear();
    data_.flat_keypoint_geometry_valid.clear();
    data_.flat_keypoint_refined_success.clear();
    data_.flat_keypoint_detection_source.clear();
    data_.kp_review_state.clear();
    data_.kp_review_method.clear();
    data_.kp_review_intended_use.clear();
    data_.kp_review_timestamp.clear();
    data_.kp_review_reviewer.clear();
    data_.kp_review_notes.clear();
    data_.has_kp_review_status = false;

    if (data_.layout != ZarrLayoutType::kPaletteRuns) {
        return false;
    }

    if (data_.frame_offsets.size() < 2) {
        return false;
    }

    const size_t total_detections = data_.bbox_norm_coords.size();
    if (total_detections == 0) {
        return false;
    }

    // Phase 1: Try refined_keypoints_runs (preferred source)
    std::string latest_run;
    bool is_refined_source = false;
    std::string run_base;

    if (auto group_attrs = readAttrsAny(store, "refined_keypoints_runs")) {
        latest_run = extractLatestRunName(*group_attrs);
        if (!latest_run.empty()) {
            is_refined_source = true;
        }
    }
    if (latest_run.empty() && !root_path_.empty()) {
        auto refined_candidates = collect_runs_fs(
            root_path_,
            "refined_keypoints_runs",
            {"frame_indices", "keypoints_roi", "heading"});
        if (!refined_candidates.empty()) {
            latest_run = refined_candidates.back();
            is_refined_source = true;
        }
    }

    // Phase 2: Fall back to raw keypoints_runs
    if (latest_run.empty()) {
        if (auto group_attrs = readAttrsAny(store, "keypoints_runs")) {
            latest_run = extractLatestRunName(*group_attrs);
        }
    }
    if (latest_run.empty()) {
        if (root_path_.empty()) {
            return false;
        }
        auto candidates = collect_runs_fs(
            root_path_,
            "keypoints_runs",
            {"frame_indices", "keypoints_roi", "heading"});
        if (candidates.empty()) {
            return false;
        }
        latest_run = candidates.back();
    }

    if (is_refined_source) {
        run_base = "refined_keypoints_runs/" + latest_run + "/";
    } else {
        run_base = "keypoints_runs/" + latest_run + "/";
    }

    std::vector<int32_t> kp_frame_indices;
    if (!readInt32Array(store, run_base + "frame_indices", kp_frame_indices)) {
        return false;
    }
    if (kp_frame_indices.empty()) {
        return false;
    }
    const size_t roi_count = kp_frame_indices.size();

    std::vector<float> heading_values;
    if (!readFloatArray(store, run_base + "heading", heading_values)) {
        heading_values.assign(roi_count, 0.0f);
    } else if (heading_values.size() != roi_count) {
        heading_values.resize(roi_count, 0.0f);
    }

    std::vector<uint8_t> detection_success;
    if (!readBoolArray(store, run_base + "detection_success", detection_success) ||
        detection_success.size() != roi_count) {
        detection_success.assign(roi_count, 1);
    }

    size_t num_keypoints = 0;
    size_t coord_dim = 0;
    std::vector<float> kp_values;
    enum class KeypointSpace {
        kImage,
        kRoi,
        kNormalized
    };
    KeypointSpace keypoint_space = KeypointSpace::kImage;

    auto load_keypoints_dataset = [&](const std::string& dataset_path,
                                      KeypointSpace space) -> bool {
        auto store_float = openArrayAny<float, 3>(store, dataset_path, context_);
        if (store_float.ok()) {
            auto array_result = ts::Read(store_float.value()).result();
            if (!array_result.ok()) {
                return false;
            }
            auto array = array_result.value();
            auto shape = array.shape();
            if (shape.size() != 3 ||
                shape[0] != static_cast<tensorstore::Index>(roi_count)) {
                return false;
            }
            num_keypoints = static_cast<size_t>(shape[1]);
            coord_dim = static_cast<size_t>(shape[2]);
            if (num_keypoints == 0 || coord_dim < 2) {
                return false;
            }
            size_t total_values = roi_count * num_keypoints * coord_dim;
            kp_values.resize(total_values);
            const float* data_ptr = static_cast<const float*>(array.data());
            std::copy(data_ptr, data_ptr + total_values, kp_values.begin());
            keypoint_space = space;
            return true;
        }

        auto store_double = openArrayAny<double, 3>(store, dataset_path, context_);
        if (!store_double.ok()) {
            return false;
        }
        auto array_result = ts::Read(store_double.value()).result();
        if (!array_result.ok()) {
            return false;
        }
        auto array = array_result.value();
        auto shape = array.shape();
        if (shape.size() != 3 ||
            shape[0] != static_cast<tensorstore::Index>(roi_count)) {
            return false;
        }
        num_keypoints = static_cast<size_t>(shape[1]);
        coord_dim = static_cast<size_t>(shape[2]);
        if (num_keypoints == 0 || coord_dim < 2) {
            return false;
        }
        size_t total_values = roi_count * num_keypoints * coord_dim;
        kp_values.resize(total_values);
        const double* data_ptr = static_cast<const double*>(array.data());
        for (size_t i = 0; i < total_values; ++i) {
            kp_values[i] = static_cast<float>(data_ptr[i]);
        }
        keypoint_space = space;
        return true;
    };

    if (!load_keypoints_dataset(run_base + "keypoints_img", KeypointSpace::kImage) &&
        !load_keypoints_dataset(run_base + "keypoints_roi", KeypointSpace::kRoi) &&
        !load_keypoints_dataset(run_base + "keypoints_norm", KeypointSpace::kNormalized)) {
        return false;
    }

    auto run_attrs = readAttrsAny(store, run_base);
    std::vector<std::string> keypoint_labels_loaded;
    if (run_attrs.has_value() &&
        run_attrs->contains("keypoint_labels") &&
        (*run_attrs)["keypoint_labels"].is_array()) {
        const auto& labels = (*run_attrs)["keypoint_labels"];
        keypoint_labels_loaded.reserve(labels.size());
        for (size_t i = 0; i < labels.size(); ++i) {
            if (!labels[i].is_string()) {
                continue;
            }
            std::string label = labels[i].get<std::string>();
            keypoint_labels_loaded.push_back(label);
        }
    }

    // Load skeleton edges from pose_schema
    data_.skeleton_edges.clear();
    if (run_attrs.has_value() &&
        run_attrs->contains("pose_schema") &&
        (*run_attrs)["pose_schema"].is_object()) {
        const auto& schema = (*run_attrs)["pose_schema"];
        if (schema.contains("edges") && schema["edges"].is_array()) {
            for (const auto& edge : schema["edges"]) {
                if (edge.is_array() && edge.size() == 2 &&
                    edge[0].is_number_unsigned() && edge[1].is_number_unsigned()) {
                    size_t a = edge[0].get<size_t>();
                    size_t b = edge[1].get<size_t>();
                    data_.skeleton_edges.push_back({a, b});
                }
            }
        }
    }

    if (num_keypoints > 0) {
        data_.keypoint_labels.clear();
        if (!keypoint_labels_loaded.empty()) {
            if (keypoint_labels_loaded.size() == num_keypoints) {
                data_.keypoint_labels = keypoint_labels_loaded;
            } else {
                data_.keypoint_labels.reserve(num_keypoints);
                for (size_t i = 0; i < num_keypoints; ++i) {
                    if (i < keypoint_labels_loaded.size()) {
                        data_.keypoint_labels.push_back(keypoint_labels_loaded[i]);
                    } else {
                        data_.keypoint_labels.push_back("kp" + std::to_string(i));
                    }
                }
            }
        }
        if (data_.keypoint_labels.empty()) {
            data_.keypoint_labels.reserve(num_keypoints);
            static const std::array<std::string, 3> kDefaultLabels = {
                "swim_bladder", "left_eye", "right_eye"};
            for (size_t i = 0; i < num_keypoints; ++i) {
                if (i < kDefaultLabels.size()) {
                    data_.keypoint_labels.push_back(kDefaultLabels[i]);
                } else {
                    data_.keypoint_labels.push_back("kp" + std::to_string(i));
                }
            }
        }
    } else {
        data_.keypoint_labels.clear();
    }

    data_.heading_computation_spec =
        resolveKeypointHeadingComputationSpec(
            run_attrs.has_value() ? &*run_attrs : nullptr,
            data_.keypoint_labels);

    std::vector<std::string> crop_candidates;
    auto addCandidate = [&](const std::string& candidate) {
        if (candidate.empty()) {
            return;
        }
        std::string normalized = NormalizeCropRunName(candidate);
        if (std::find(crop_candidates.begin(), crop_candidates.end(), normalized) == crop_candidates.end()) {
            crop_candidates.push_back(normalized);
        }
    };

    if (run_attrs.has_value() && run_attrs->contains("source_crop_run") &&
        (*run_attrs)["source_crop_run"].is_string()) {
        addCandidate((*run_attrs)["source_crop_run"].get<std::string>());
    }

    if (auto crop_group_attrs = readAttrsAny(store, "crop_runs")) {
        std::string latest = extractLatestRunName(*crop_group_attrs);
        if (!latest.empty()) {
            addCandidate(latest);
        }
    }

    if (!root_path_.empty()) {
        auto fs_candidates = collect_runs_fs(
            root_path_,
            "crop_runs",
            {"roi_coordinates_full"});
        for (const auto& name : fs_candidates) {
            addCandidate(name);
        }
    }

    std::vector<float> roi_offsets;
    bool roi_ok = false;
    std::string crop_run;
    float roi_height_px = 0.0f;
    float roi_width_px = 0.0f;
    bool roi_size_available = false;

    auto updateRoiSizeFromAttrs = [&](const std::string& crop_base) {
        if (roi_size_available) {
            return;
        }
        if (auto crop_attrs = readAttrsAny(store, crop_base)) {
            const auto& attrs = *crop_attrs;
            if (attrs.contains("roi_size") && attrs["roi_size"].is_array()) {
                const auto& roi_size_attr = attrs["roi_size"];
                if (roi_size_attr.size() >= 2) {
                    if (roi_size_attr[0].is_number() && roi_size_attr[1].is_number()) {
                        roi_height_px = static_cast<float>(roi_size_attr[0].get<double>());
                        roi_width_px = static_cast<float>(roi_size_attr[1].get<double>());
                        if (roi_height_px > 0.0f && roi_width_px > 0.0f) {
                            roi_size_available = true;
                        }
                    }
                }
            }
        }
        if (!roi_size_available) {
            auto roi_image_store =
                openArrayAny<uint8_t, 3>(store, crop_base + "roi_images", context_);
            if (roi_image_store.ok()) {
                auto domain = roi_image_store.value().domain();
                auto shape = domain.shape();
                if (shape.size() == 3) {
                    float inferred_height = static_cast<float>(shape[1]);
                    float inferred_width = static_cast<float>(shape[2]);
                    if (inferred_height > 0.0f && inferred_width > 0.0f) {
                        roi_height_px = inferred_height;
                        roi_width_px = inferred_width;
                        roi_size_available = true;
                    }
                }
            }
        }
    };

    auto tryLoadCropRun = [&](const std::string& candidate) -> bool {
        if (candidate.empty()) {
            return false;
        }
        std::string crop_base = "crop_runs/" + candidate + "/";
        auto roi_float_store =
            openArrayAny<float, 2>(store, crop_base + "roi_coordinates_full", context_);
        if (roi_float_store.ok()) {
            auto roi_array_result = ts::Read(roi_float_store.value()).result();
            if (roi_array_result.ok()) {
                auto roi_array = roi_array_result.value();
                auto roi_shape = roi_array.shape();
                if (roi_shape.size() == 2 &&
                    roi_shape[0] == static_cast<tensorstore::Index>(roi_count)) {
                    size_t cols = static_cast<size_t>(roi_shape[1]);
                    if (cols >= 2) {
                        roi_offsets.assign(roi_count * 2, 0.0f);
                        const float* roi_ptr = static_cast<const float*>(roi_array.data());
                        for (size_t i = 0; i < roi_count; ++i) {
                            roi_offsets[i * 2 + 0] = roi_ptr[i * cols + 0]; // x / column
                            roi_offsets[i * 2 + 1] = roi_ptr[i * cols + 1]; // y / row
                        }
                        updateRoiSizeFromAttrs(crop_base);
                        return true;
                    }
                }
            }
        }

        auto roi_int_store =
            openArrayAny<int32_t, 2>(store, crop_base + "roi_coordinates_full", context_);
        if (roi_int_store.ok()) {
            auto roi_array_result = ts::Read(roi_int_store.value()).result();
            if (roi_array_result.ok()) {
                auto roi_array = roi_array_result.value();
                auto roi_shape = roi_array.shape();
                if (roi_shape.size() == 2 &&
                    roi_shape[0] == static_cast<tensorstore::Index>(roi_count)) {
                    size_t cols = static_cast<size_t>(roi_shape[1]);
                    if (cols >= 2) {
                        roi_offsets.assign(roi_count * 2, 0.0f);
                        const int32_t* roi_ptr = static_cast<const int32_t*>(roi_array.data());
                        for (size_t i = 0; i < roi_count; ++i) {
                            roi_offsets[i * 2 + 0] = static_cast<float>(roi_ptr[i * cols + 0]);
                            roi_offsets[i * 2 + 1] = static_cast<float>(roi_ptr[i * cols + 1]);
                        }
                        updateRoiSizeFromAttrs(crop_base);
                        return true;
                    }
                }
            }
        }
        return false;
    };

    for (const auto& candidate : crop_candidates) {
        if (tryLoadCropRun(candidate)) {
            crop_run = candidate;
            roi_ok = true;
            break;
        }
    }

    if (!roi_ok && !crop_candidates.empty()) {
        std::cerr << "[HEADING_WARNING] Failed to load ROI offsets from crop_runs for any candidate ("
                  << crop_candidates.size() << " tried)." << std::endl;
    }

    if (!roi_ok) {
        roi_offsets.clear();
    }

    const float nan_value = std::numeric_limits<float>::quiet_NaN();
    data_.flat_headings_deg.assign(total_detections, 0.0f);
    data_.flat_swim_bladder_px.assign(
        total_detections, std::array<float, 2>{nan_value, nan_value});
    data_.flat_heading_valid.assign(total_detections, 0);
    data_.keypoint_roi_indices.assign(total_detections, -1);
    data_.mask_roi_indices.assign(total_detections, -1);
    data_.roi_offset_x.assign(total_detections, nan_value);
    data_.roi_offset_y.assign(total_detections, nan_value);
    data_.roi_width_px.assign(total_detections, 0.0f);
    data_.roi_height_px.assign(total_detections, 0.0f);
    if (num_keypoints > 0) {
        data_.keypoints_per_detection = num_keypoints;
        data_.flat_keypoints_px.assign(total_detections * num_keypoints * 2, nan_value);
        data_.has_keypoints = true;
    } else {
        data_.keypoints_per_detection = 0;
        data_.flat_keypoints_px.clear();
        data_.has_keypoints = false;
    }

    std::vector<size_t> frame_cursor(
        data_.frame_offsets.size() > 0 ? data_.frame_offsets.size() - 1 : 0, 0);
    std::vector<size_t> roi_to_det(roi_count, SIZE_MAX);
    size_t filled = 0;
    size_t finite_keypoint_count = 0;

    for (size_t roi_index = 0; roi_index < roi_count; ++roi_index) {
        int32_t frame = kp_frame_indices[roi_index];
        if (frame < 0) {
            continue;
        }
        if (static_cast<size_t>(frame) >= frame_cursor.size()) {
            continue;
        }

        size_t start = data_.frame_offsets[frame];
        size_t end = data_.frame_offsets[frame + 1];
        if (start >= end) {
            frame_cursor[frame]++;
            continue;
        }

        size_t offset = frame_cursor[frame];
        if (start + offset >= end) {
            frame_cursor[frame]++;
            continue;
        }

        size_t det_index = start + offset;
        frame_cursor[frame]++;
        roi_to_det[roi_index] = det_index;
        data_.keypoint_roi_indices[det_index] = static_cast<int32_t>(roi_index);

        data_.mask_roi_indices[det_index] = (roi_ok && roi_offsets.size() >= (roi_index * 2 + 2))
                                                ? static_cast<int32_t>(roi_index)
                                                : -1;

        std::array<float, 4> pixel_box = {nan_value, nan_value, nan_value, nan_value};
        bool have_pixel_box = false;
        if (det_index < data_.bbox_norm_coords.size() &&
            data_.image_width > 0 && data_.image_height > 0) {
            pixel_box = normalizedBoxToPixels(
                data_.bbox_norm_coords[det_index],
                data_.image_width,
                data_.image_height);
            have_pixel_box = true;
        }

        float pixel_box_width = have_pixel_box ? std::max(0.0f, pixel_box[2] - pixel_box[0]) : 0.0f;
        float pixel_box_height = have_pixel_box ? std::max(0.0f, pixel_box[3] - pixel_box[1]) : 0.0f;
        float roi_width_for_det = roi_size_available ? roi_width_px : pixel_box_width;
        float roi_height_for_det = roi_size_available ? roi_height_px : pixel_box_height;

        bool success = detection_success[roi_index] != 0;
        data_.flat_heading_valid[det_index] = success ? 1 : 0;
        data_.flat_headings_deg[det_index] =
            roi_index < heading_values.size() ? heading_values[roi_index] : 0.0f;

        float offset_x_det = nan_value;
        float offset_y_det = nan_value;
        if (roi_ok && roi_offsets.size() >= (roi_index * 2 + 2)) {
            float raw_offset_x = roi_offsets[roi_index * 2 + 0];
            float raw_offset_y = roi_offsets[roi_index * 2 + 1];
            if (std::isfinite(raw_offset_x) && std::isfinite(raw_offset_y)) {
                offset_x_det = raw_offset_x;
                offset_y_det = raw_offset_y;
            }
        }

        bool assigned_anchor = false;
        float assigned_x = nan_value;
        float assigned_y = nan_value;
        float raw_anchor_x = nan_value;
        float raw_anchor_y = nan_value;
        std::vector<std::array<double, 2>> converted_positions;
        converted_positions.assign(num_keypoints,
                                   std::array<double, 2>{
                                       std::numeric_limits<double>::quiet_NaN(),
                                       std::numeric_limits<double>::quiet_NaN()});

        if (!kp_values.empty() && num_keypoints > 0 && coord_dim >= 2) {
            size_t stride = num_keypoints * coord_dim;
            size_t flat_stride = num_keypoints * 2;
            size_t flat_base = det_index * flat_stride;
            bool can_store_flat =
                data_.has_keypoints &&
                (flat_base + flat_stride) <= data_.flat_keypoints_px.size();

            auto convertRawToImage = [&](float raw_x, float raw_y) -> std::array<float, 2> {
                std::array<float, 2> result = {nan_value, nan_value};
                if (!std::isfinite(raw_x) || !std::isfinite(raw_y)) {
                    return result;
                }

                if (keypoint_space == KeypointSpace::kImage) {
                    result[0] = raw_x;
                    result[1] = raw_y;
                    return result;
                }

                float local_x = raw_x;
                float local_y = raw_y;

                if (keypoint_space == KeypointSpace::kNormalized) {
                    float scale_x = roi_width_for_det;
                    float scale_y = roi_height_for_det;
                    bool used_fallback_scale = false;
                    if (scale_x <= 0.0f || scale_y <= 0.0f) {
                        scale_x = pixel_box_width;
                        scale_y = pixel_box_height;
                        used_fallback_scale = true;
                    }
                    if (scale_x <= 0.0f || scale_y <= 0.0f) {
                        return result;
                    }
                    local_x = raw_x * scale_x;
                    local_y = raw_y * scale_y;
                    if (used_fallback_scale) {
                        roi_width_for_det = scale_x;
                        roi_height_for_det = scale_y;
                    }
                }

                float offset_x = offset_x_det;
                float offset_y = offset_y_det;
                bool has_offset = std::isfinite(offset_x) && std::isfinite(offset_y);
                if (!has_offset) {
                    if (std::isfinite(pixel_box[0]) && std::isfinite(pixel_box[1])) {
                        offset_x = pixel_box[0];
                        offset_y = pixel_box[1];
                        has_offset = true;
                    }
                }

                if (keypoint_space == KeypointSpace::kRoi ||
                    keypoint_space == KeypointSpace::kNormalized) {
                    if (!has_offset) {
                        return result;
                    }
                    result[0] = offset_x + local_x;
                    result[1] = offset_y + local_y;
                    return result;
                }

                result[0] = local_x;
                result[1] = local_y;
                return result;
            };

            for (size_t kp_idx = 0; kp_idx < num_keypoints; ++kp_idx) {
                size_t kp_base = roi_index * stride + kp_idx * coord_dim;
                float raw_x = nan_value;
                float raw_y = nan_value;
                if (kp_base + 1 < kp_values.size()) {
                    raw_x = kp_values[kp_base + 0];
                    raw_y = kp_values[kp_base + 1];
                }

                std::array<float, 2> converted = convertRawToImage(raw_x, raw_y);
                if (can_store_flat) {
                    data_.flat_keypoints_px[flat_base + kp_idx * 2 + 0] = converted[0];
                    data_.flat_keypoints_px[flat_base + kp_idx * 2 + 1] = converted[1];
                }
                converted_positions[kp_idx] = {
                    static_cast<double>(converted[0]),
                    static_cast<double>(converted[1])};
                if (can_store_flat && std::isfinite(converted[0]) && std::isfinite(converted[1])) {
                    finite_keypoint_count++;
                }
            }
        }

        if (data_.heading_computation_spec.available &&
            data_.heading_computation_spec.enabled) {
            std::array<double, 2> resolved_origin{};
            if (evaluateKeypointHeadingOrigin(data_.heading_computation_spec,
                                              converted_positions,
                                              resolved_origin)) {
                assigned_x = static_cast<float>(resolved_origin[0]);
                assigned_y = static_cast<float>(resolved_origin[1]);
                assigned_anchor = true;
            }
            if (data_.heading_computation_spec.direction_from.indices.size() == 1) {
                const int raw_anchor_idx =
                    data_.heading_computation_spec.direction_from.indices[0];
                if (raw_anchor_idx >= 0 &&
                    static_cast<size_t>(raw_anchor_idx) < num_keypoints) {
                    size_t kp_base = roi_index * num_keypoints * coord_dim +
                                     static_cast<size_t>(raw_anchor_idx) * coord_dim;
                    if (kp_base + 1 < kp_values.size()) {
                        raw_anchor_x = kp_values[kp_base + 0];
                        raw_anchor_y = kp_values[kp_base + 1];
                    }
                }
            }
        }

        if (!assigned_anchor && have_pixel_box) {
            float fallback_x = 0.5f * (pixel_box[0] + pixel_box[2]);
            float fallback_y = 0.5f * (pixel_box[1] + pixel_box[3]);
            if (std::isfinite(fallback_x) && std::isfinite(fallback_y)) {
                assigned_x = fallback_x;
                assigned_y = fallback_y;
                assigned_anchor = true;
            }
        }

        if (assigned_anchor) {
            data_.flat_swim_bladder_px[det_index] = {assigned_x, assigned_y};
        }

        if (std::isfinite(offset_x_det) && std::isfinite(offset_y_det)) {
            data_.roi_offset_x[det_index] = offset_x_det;
            data_.roi_offset_y[det_index] = offset_y_det;
        }

        if (roi_width_for_det > 0.0f && roi_height_for_det > 0.0f) {
            data_.roi_width_px[det_index] = roi_width_for_det;
            data_.roi_height_px[det_index] = roi_height_for_det;
        } else if (have_pixel_box) {
            data_.roi_width_px[det_index] = std::max(0.0f, pixel_box[2] - pixel_box[0]);
            data_.roi_height_px[det_index] = std::max(0.0f, pixel_box[3] - pixel_box[1]);
        }

        constexpr int kMaxDetailLogs = 12;
        static int detail_log_count = 0;
        if (detail_log_count < kMaxDetailLogs && kChaserDebugLoggingEnabled) {
            std::cout << "[HEADING_LOAD_DETAIL] roi=" << roi_index
                      << " frame=" << frame
                      << " det_index=" << det_index
                      << " success=" << static_cast<int>(success)
                      << " raw_kp=(";
            if (std::isfinite(raw_anchor_x) && std::isfinite(raw_anchor_y)) {
                std::cout << raw_anchor_x << ", " << raw_anchor_y << ")";
            } else {
                std::cout << "nan, nan)";
            }
            std::cout << " converted=(";
            if (std::isfinite(assigned_x) && std::isfinite(assigned_y)) {
                std::cout << assigned_x << ", " << assigned_y << ")";
            } else {
                std::cout << "nan, nan)";
            }
            std::cout << " space=";
            switch (keypoint_space) {
                case KeypointSpace::kImage:
                    std::cout << "image";
                    break;
                case KeypointSpace::kRoi:
                    std::cout << "roi";
                    break;
                case KeypointSpace::kNormalized:
                    std::cout << "normalized";
                    break;
            }
            if (std::isfinite(offset_x_det) && std::isfinite(offset_y_det)) {
                std::cout << " roi_offsets=(y=" << offset_y_det << ", x=" << offset_x_det << ")";
            } else {
                std::cout << " roi_offsets=UNAVAILABLE";
            }
            std::cout << " roi_size=(" << roi_height_for_det << ", " << roi_width_for_det << ")";
            if (assigned_anchor) {
                std::cout << " assigned=yes anchor=(" << assigned_x << ", " << assigned_y << ")";
            } else {
                std::cout << " assigned=no anchor=(nan, nan)";
            }
            std::cout << std::endl;
            detail_log_count++;
        }

        filled++;
    }

    data_.has_heading_data = filled > 0;
    if (data_.keypoints_per_detection > 0) {
        data_.has_keypoints = finite_keypoint_count > 0;
        if (!data_.has_keypoints) {
            data_.flat_keypoints_px.clear();
            data_.keypoints_per_detection = 0;
        }
    } else {
        data_.has_keypoints = false;
    }
    if (data_.has_heading_data) {
        data_.keypoints_run_name = latest_run;
        data_.keypoints_source_crop_run = crop_run;

        constexpr size_t kMaxLogEntries = 8;
        if (kChaserDebugLoggingEnabled) {
            for (size_t i = 0; i < std::min(kMaxLogEntries, data_.flat_swim_bladder_px.size()); ++i) {
                const auto& anchor = data_.flat_swim_bladder_px[i];
                const float heading = i < data_.flat_headings_deg.size() ? data_.flat_headings_deg[i] : 0.0f;
                const uint8_t valid = i < data_.flat_heading_valid.size() ? data_.flat_heading_valid[i] : 0;
                std::cout << "[HEADING_LOAD_DEBUG] idx " << i
                          << " anchor=(" << anchor[0] << ", " << anchor[1]
                          << ") heading=" << heading
                          << " valid=" << static_cast<int>(valid)
                          << " finite=" << (std::isfinite(anchor[0]) && std::isfinite(anchor[1]))
                          << std::endl;
            }
        }
    }

    // Load refined keypoint quality arrays when using refined source
    if (is_refined_source && data_.has_heading_data) {
        data_.is_refined_keypoints = true;
        data_.refined_keypoints_run_name = latest_run;

        // Allocate detection-aligned flat vectors with defaults
        data_.flat_keypoint_quality_labels.assign(total_detections, -1);
        data_.flat_keypoint_reason.assign(total_detections, std::string());
        data_.flat_keypoint_flip_corrected.assign(total_detections, 0);
        data_.flat_keypoint_usable.assign(total_detections, 0);
        data_.flat_keypoint_confidence_valid.assign(total_detections, 0);
        data_.flat_keypoint_geometry_valid.assign(total_detections, 0);
        data_.flat_keypoint_refined_success.assign(total_detections, 0);
        data_.flat_keypoint_detection_source.assign(total_detections, 0);

        // Load ROI-aligned quality arrays
        std::vector<int32_t> roi_quality_labels;
        readInt32Array(store, run_base + "quality_labels", roi_quality_labels);

        std::vector<std::string> roi_reason;
        bool reason_loaded = readStringArray(store, run_base + "reason_bytes", roi_reason);
        if (!reason_loaded) {
            if (!readStringArray(store, run_base + "reason", roi_reason)) {
                static bool reason_warned = false;
                if (!reason_warned) {
                    std::cout << "[REFINED_KP_WARNING] reason_bytes and reason arrays unavailable for run '"
                              << latest_run << "'; quality reason display disabled." << std::endl;
                    reason_warned = true;
                }
            }
        }

        std::vector<uint8_t> roi_flip_corrected;
        readBoolArray(store, run_base + "flip_corrected", roi_flip_corrected);

        std::vector<uint8_t> roi_usable;
        readBoolArray(store, run_base + "usable_keypoints", roi_usable);

        std::vector<uint8_t> roi_confidence_valid;
        readBoolArray(store, run_base + "confidence_valid", roi_confidence_valid);

        std::vector<uint8_t> roi_geometry_valid;
        readBoolArray(store, run_base + "geometry_valid", roi_geometry_valid);

        std::vector<uint8_t> roi_refined_success;
        readBoolArray(store, run_base + "refined_success", roi_refined_success);

        std::vector<int32_t> roi_detection_source_i32;
        std::vector<uint8_t> roi_det_source;
        if (readInt32Array(store, run_base + "detection_source", roi_detection_source_i32)) {
            roi_det_source.reserve(roi_detection_source_i32.size());
            for (int32_t v : roi_detection_source_i32) {
                roi_det_source.push_back(static_cast<uint8_t>(v));
            }
        } else {
            readBoolArray(store, run_base + "detection_source", roi_det_source);
        }

        // Scatter ROI arrays → detection-aligned flat vectors using roi_to_det
        for (size_t roi = 0; roi < roi_count; ++roi) {
            size_t det = roi_to_det[roi];
            if (det == SIZE_MAX || det >= total_detections) continue;

            if (roi < roi_quality_labels.size()) {
                data_.flat_keypoint_quality_labels[det] = roi_quality_labels[roi];
            }
            if (roi < roi_reason.size()) {
                data_.flat_keypoint_reason[det] = roi_reason[roi];
            }
            if (roi < roi_flip_corrected.size()) {
                data_.flat_keypoint_flip_corrected[det] = roi_flip_corrected[roi];
            }
            if (roi < roi_usable.size()) {
                data_.flat_keypoint_usable[det] = roi_usable[roi];
            }
            if (roi < roi_confidence_valid.size()) {
                data_.flat_keypoint_confidence_valid[det] = roi_confidence_valid[roi];
            }
            if (roi < roi_geometry_valid.size()) {
                data_.flat_keypoint_geometry_valid[det] = roi_geometry_valid[roi];
            }
            if (roi < roi_refined_success.size()) {
                data_.flat_keypoint_refined_success[det] = roi_refined_success[roi];
            }
            if (roi < roi_det_source.size()) {
                data_.flat_keypoint_detection_source[det] = roi_det_source[roi];
            }
        }

        // Load keypoint_review_status from run attrs (same pattern as detect review)
        auto run_review_attrs = readAttrsAny(store, run_base);
        if (run_review_attrs.has_value() &&
            run_review_attrs->contains("keypoint_review_status") &&
            (*run_review_attrs)["keypoint_review_status"].is_object()) {
            const auto& rs = (*run_review_attrs)["keypoint_review_status"];
            auto str_field = [&](const char* key) -> std::string {
                if (rs.contains(key) && rs[key].is_string()) return rs[key].get<std::string>();
                return "";
            };
            data_.kp_review_state        = str_field("state");
            data_.kp_review_method       = str_field("method");
            data_.kp_review_intended_use = str_field("intended_use");
            data_.kp_review_timestamp    = str_field("timestamp");
            data_.kp_review_reviewer     = str_field("reviewer");
            data_.kp_review_notes        = str_field("notes");
            data_.has_kp_review_status   = !data_.kp_review_state.empty();
        }

        std::cout << "  Refined keypoints run '" << latest_run << "' loaded (review: "
                  << (data_.has_kp_review_status ? data_.kp_review_state : "none") << ")"
                  << std::endl;
    }

    return data_.has_heading_data;
}

void ZarrDetectionLoader::stopRefinedSubjectMaskOptionalOverlayWorker() {
    {
        std::lock_guard<std::mutex> overlay_lock(
            refined_subject_mask_optional_overlay_mutex_);
        ++refined_subject_mask_optional_overlay_generation_;
        refined_subject_mask_optional_overlay_loading_ = false;
    }
    if (refined_subject_mask_optional_overlay_worker_.joinable()) {
        refined_subject_mask_optional_overlay_worker_.join();
    }
}

std::string ZarrDetectionLoader::getRefinedSubjectMaskOptionalOverlayStatus()
    const {
    std::lock_guard<std::mutex> overlay_lock(
        refined_subject_mask_optional_overlay_mutex_);
    if (refined_subject_mask_optional_overlay_loaded_) {
        return "optional overlays ready";
    }
    if (refined_subject_mask_optional_overlay_loading_) {
        return "optional overlays loading";
    }
    if (refined_subject_mask_optional_overlay_failed_) {
        return refined_subject_mask_optional_overlay_error_.empty()
                   ? "optional overlays failed"
                   : "optional overlays failed: " +
                         refined_subject_mask_optional_overlay_error_;
    }
    if (data_.eye_masks_from_refined_subject_masks && data_.has_eye_masks) {
        return "optional overlays deferred";
    }
    return {};
}

void ZarrDetectionLoader::requestRefinedSubjectMaskOptionalOverlayPrefetch() {
    if (refined_subject_mask_optional_overlay_worker_.joinable()) {
        bool worker_finished = false;
        {
            std::lock_guard<std::mutex> overlay_lock(
                refined_subject_mask_optional_overlay_mutex_);
            worker_finished =
                !refined_subject_mask_optional_overlay_loading_;
        }
        if (worker_finished) {
            refined_subject_mask_optional_overlay_worker_.join();
        } else {
            return;
        }
    }

    uint64_t generation = 0;
    std::string archive_path;
    {
        std::lock_guard<std::mutex> overlay_lock(
            refined_subject_mask_optional_overlay_mutex_);
        if (!data_.eye_masks_from_refined_subject_masks ||
            !data_.has_eye_masks ||
            data_.eye_masks_run_name.empty() ||
            root_path_.empty() ||
            refined_subject_mask_optional_overlay_loaded_ ||
            refined_subject_mask_optional_overlay_loading_ ||
            refined_subject_mask_optional_overlay_requested_) {
            return;
        }
        refined_subject_mask_optional_overlay_requested_ = true;
        refined_subject_mask_optional_overlay_loading_ = true;
        refined_subject_mask_optional_overlay_failed_ = false;
        refined_subject_mask_optional_overlay_error_.clear();
        generation = refined_subject_mask_optional_overlay_generation_;
        archive_path = root_path_;
    }

    refined_subject_mask_optional_overlay_worker_ = std::thread(
        [this, generation, archive_path]() {
            std::string error;
            const bool loaded = loadRefinedSubjectMaskOptionalOverlayData(
                generation,
                archive_path,
                &error);
            std::lock_guard<std::mutex> overlay_lock(
                refined_subject_mask_optional_overlay_mutex_);
            if (generation != refined_subject_mask_optional_overlay_generation_) {
                return;
            }
            refined_subject_mask_optional_overlay_loading_ = false;
            refined_subject_mask_optional_overlay_loaded_ = loaded;
            refined_subject_mask_optional_overlay_failed_ = !loaded;
            refined_subject_mask_optional_overlay_error_ =
                loaded ? std::string{} : error;
        });
}

bool ZarrDetectionLoader::loadRefinedSubjectMaskOptionalOverlayData(
    uint64_t generation,
    const std::string& archive_path,
    std::string* error_message) {
    auto fail = [&](const std::string& message) {
        if (error_message != nullptr) {
            *error_message = message;
        }
        return false;
    };

    const auto load_start = std::chrono::steady_clock::now();
    const std::string kvstore_path = normalizeKvstoreFileRootPath(archive_path);
    auto spec_result = ts::kvstore::Spec::FromJson({
        {"driver", "file"},
        {"path", kvstore_path},
    });
    if (!spec_result.ok()) {
        return fail("failed to create kvstore spec: " +
                    spec_result.status().ToString());
    }
    auto store_result = ts::kvstore::Open(spec_result.value(), context_).result();
    if (!store_result.ok()) {
        return fail("failed to open kvstore: " +
                    store_result.status().ToString());
    }
    const auto store = store_result.value();

    std::string run_name;
    size_t roi_dim = 0;
    bool left_available = false;
    bool right_available = false;
    std::vector<ZarrDetectionData::RefinedSubjectMaskComponentInfo> components;
    {
        std::lock_guard<std::mutex> overlay_lock(
            refined_subject_mask_optional_overlay_mutex_);
        std::lock_guard<std::mutex> cache_lock(*data_.mask_chunk_cache_mutex);
        if (generation != refined_subject_mask_optional_overlay_generation_) {
            return fail("stale optional overlay load");
        }
        if (!data_.eye_masks_from_refined_subject_masks ||
            !data_.has_eye_masks ||
            data_.eye_masks_run_name.empty() ||
            data_.eye_mask_roi_count == 0) {
            return fail("refined subject masks are not loaded");
        }
        run_name = data_.eye_masks_run_name;
        roi_dim = data_.eye_mask_roi_count;
        left_available =
            data_.eye_mask_channel_indices[0] != kInvalidMaskChannel;
        right_available =
            data_.eye_mask_channel_indices[1] != kInvalidMaskChannel;
        components = data_.refined_subject_mask_overlay_components;
    }

    const std::string run_base =
        "refined_subject_masks_runs/" + run_name + "/";
    size_t contour_component_count = 0;
    auto loadComponentContours =
        [&](ZarrDetectionData::RefinedSubjectMaskComponentInfo& component) {
            const std::string contour_base =
                run_base + "components/" + component.label + "/contours/";

            std::vector<int64_t> contour_ptr;
            std::vector<int32_t> contour_len;
            if (!readInt64Array(store, contour_base + "ptr", contour_ptr) ||
                !readInt32Array(store, contour_base + "len", contour_len)) {
                return;
            }
            if (contour_ptr.size() != roi_dim || contour_len.size() != roi_dim) {
                component.contour_warning =
                    "contour ptr/len shape does not match masks_roi rows";
                return;
            }

            auto points_store =
                openArrayAny<float, 2>(store, contour_base + "points_xy", context_);
            if (!points_store.ok()) {
                component.contour_warning =
                    "contour points_xy is missing or unreadable";
                return;
            }

            const auto points_shape = points_store.value().domain().shape();
            if (points_shape.size() != 2 || points_shape[1] < 2) {
                component.contour_warning =
                    "contour points_xy shape is not Nx2";
                return;
            }

            bool attrs_compatible = false;
            if (auto attrs = readAttrsAny(store, contour_base)) {
                const bool schema_ok =
                    stringAttrMatches(*attrs, "schema_id", "component_contours_v1") ||
                    stringAttrMatches(*attrs, "contour_schema_id", "component_contours_v1");
                const bool coordinate_ok =
                    !attrs->contains("coordinate_space") ||
                    stringAttrMatches(*attrs, "coordinate_space", "roi_pixels");
                const bool order_ok =
                    !attrs->contains("point_order") ||
                    stringAttrMatches(*attrs, "point_order", "xy");
                const bool source_ok =
                    !attrs->contains("source_component") ||
                    stringAttrMatches(*attrs,
                                      "source_component",
                                      component.label.c_str());
                attrs_compatible =
                    schema_ok && coordinate_ok && order_ok && source_ok;
                if (!attrs_compatible) {
                    component.contour_warning =
                        "contour attrs are incomplete or not fully compatible; loading arrays tolerantly";
                }
            } else {
                component.contour_warning =
                    "contour attrs are missing; loading arrays tolerantly";
            }

            component.contour_ptr = std::move(contour_ptr);
            component.contour_len = std::move(contour_len);
            component.contour_points_store = points_store.value();
            component.contour_points_count =
                static_cast<size_t>(points_shape[0]);
            component.contours_available = true;
            component.contour_attrs_compatible = attrs_compatible;
        };

    for (auto& component : components) {
        loadComponentContours(component);
        if (component.contours_available) {
            ++contour_component_count;
        }
    }

    std::vector<std::array<std::array<float, 4>, 2>> axes_major;
    std::vector<std::array<std::array<float, 4>, 2>> axes_minor;
    auto loadEllipseAxes =
        [&](const std::string& component_label, size_t eye_slot) -> bool {
        if (eye_slot >= 2) {
            return false;
        }

        const std::string geometry_base =
            run_base + "components/" + component_label + "/geometry/";
        auto ellipse_store =
            openArrayAny<float, 2>(store, geometry_base + "ellipse_params", context_);
        if (!ellipse_store.ok()) {
            return false;
        }

        auto ellipse_result = ts::Read(ellipse_store.value()).result();
        if (!ellipse_result.ok()) {
            return false;
        }

        auto ellipse_array = ellipse_result.value();
        auto ellipse_shape = ellipse_array.shape();
        if (ellipse_shape.size() != 2 ||
            ellipse_shape[0] != static_cast<ts::Index>(roi_dim) ||
            ellipse_shape[1] < 5) {
            return false;
        }

        std::vector<uint8_t> ellipse_success;
        if (!readBoolArray(store,
                           geometry_base + "ellipse_success",
                           ellipse_success)) {
            ellipse_success.assign(roi_dim, 1);
        }

        ensureEyeAxisRows(axes_major, roi_dim);
        ensureEyeAxisRows(axes_minor, roi_dim);

        constexpr float kPi = 3.14159265358979323846f;
        size_t valid_axes = 0;
        for (size_t roi = 0; roi < roi_dim; ++roi) {
            if (roi >= ellipse_success.size() || ellipse_success[roi] == 0) {
                continue;
            }

            const float cx =
                ellipse_array(static_cast<ts::Index>(roi), static_cast<ts::Index>(0));
            const float cy =
                ellipse_array(static_cast<ts::Index>(roi), static_cast<ts::Index>(1));
            const float major =
                ellipse_array(static_cast<ts::Index>(roi), static_cast<ts::Index>(2));
            const float minor =
                ellipse_array(static_cast<ts::Index>(roi), static_cast<ts::Index>(3));
            const float angle_deg =
                ellipse_array(static_cast<ts::Index>(roi), static_cast<ts::Index>(4));

            if (!std::isfinite(cx) || !std::isfinite(cy) ||
                !std::isfinite(major) || !std::isfinite(minor) ||
                !std::isfinite(angle_deg) || major <= 1e-5f ||
                minor <= 1e-5f) {
                continue;
            }

            const float theta = angle_deg * kPi / 180.0f;
            const float major_dx = std::cos(theta) * major * 0.5f;
            const float major_dy = std::sin(theta) * major * 0.5f;
            const float minor_theta = theta + 0.5f * kPi;
            const float minor_dx = std::cos(minor_theta) * minor * 0.5f;
            const float minor_dy = std::sin(minor_theta) * minor * 0.5f;

            axes_major[roi][eye_slot] = {
                cx - major_dx,
                cy - major_dy,
                cx + major_dx,
                cy + major_dy};
            axes_minor[roi][eye_slot] = {
                cx - minor_dx,
                cy - minor_dy,
                cx + minor_dx,
                cy + minor_dy};
            ++valid_axes;
        }

        return valid_axes > 0;
    };

    const bool left_geometry_ok =
        left_available && loadEllipseAxes("eye_left", 0);
    const bool right_geometry_ok =
        right_available && loadEllipseAxes("eye_right", 1);
    const bool have_axes = left_geometry_ok || right_geometry_ok;
    const size_t component_total = components.size();
    std::vector<size_t> cached_chunks_to_refresh;

    {
        std::lock_guard<std::mutex> overlay_lock(
            refined_subject_mask_optional_overlay_mutex_);
        if (generation != refined_subject_mask_optional_overlay_generation_) {
            return fail("stale optional overlay publish");
        }
        std::lock_guard<std::mutex> cache_lock(*data_.mask_chunk_cache_mutex);
        if (!data_.eye_masks_from_refined_subject_masks ||
            data_.eye_masks_run_name != run_name ||
            data_.eye_mask_roi_count != roi_dim) {
            return fail("refined subject mask run changed before publish");
        }
        cached_chunks_to_refresh.reserve(data_.mask_chunk_cache.size());
        for (const auto& entry : data_.mask_chunk_cache) {
            cached_chunks_to_refresh.push_back(entry.chunk_id);
        }
        data_.refined_subject_mask_overlay_components = std::move(components);
        data_.eye_mask_feret_axes_major = std::move(axes_major);
        data_.eye_mask_feret_axes_minor = std::move(axes_minor);
        data_.eye_masks_have_feret_axes = have_axes;
        data_.mask_chunk_loads_in_flight.clear();
        ++refined_subject_mask_optional_overlay_publish_generation_;
    }

    size_t refreshed_chunks = 0;
    for (size_t chunk_id : cached_chunks_to_refresh) {
        if (ensureEyeMaskChunk(chunk_id,
                               /*allow_prefetch=*/false,
                               /*force_reload=*/true)) {
            ++refreshed_chunks;
        }
    }

    std::cout << "  [SUBJECT_MASK_OPTIONAL_OVERLAY] Loaded optional refined subject-mask overlays for '"
              << run_name << "' (contours " << contour_component_count << "/"
              << component_total
              << " components; ellipse axes "
              << (have_axes ? "loaded" : "unavailable")
              << "; refreshed_chunks=" << refreshed_chunks
              << "; total_ms=" << elapsedMsSince(load_start)
              << ")" << std::endl;
    return true;
}

bool ZarrDetectionLoader::loadRefinedSubjectMaskEyeData(
    const ts::kvstore::KvStore& store,
    size_t roi_count) {
    StartupTraceSection trace("refined_subject_masks");
    stopEyeMaskPrefetchWorker();
    clearEyeMaskState(data_);
    trace.step("clear_state");

    if (data_.layout != ZarrLayoutType::kPaletteRuns) {
        return false;
    }

    const std::string requested_run = requested_refined_subject_mask_run_name_;
    const bool request_latest = requested_run == "latest";
    const bool request_review_latest =
        requested_run == "review-status-latest" ||
        requested_run == "review_status_latest" ||
        requested_run == "review-status" ||
        requested_run == "review_status";
    const bool explicit_named_run =
        !requested_run.empty() && !request_latest && !request_review_latest;

    std::vector<std::string> run_candidates;
    auto add_candidate = [&](const std::string& run_name) {
        if (run_name.empty()) {
            return;
        }
        if (std::find(run_candidates.begin(), run_candidates.end(), run_name) ==
            run_candidates.end()) {
            run_candidates.push_back(run_name);
        }
    };

    std::optional<nlohmann::json> group_attrs =
        readAttrsAny(store, "refined_subject_masks_runs");
    if (explicit_named_run) {
        add_candidate(requested_run);
    } else if (group_attrs.has_value()) {
        if (request_review_latest) {
            add_candidate(jsonStringAttr(
                *group_attrs, "refined_subject_mask_review_status_latest"));
            add_candidate(extractLatestRunName(*group_attrs));
        } else {
            add_candidate(extractLatestRunName(*group_attrs));
            add_candidate(jsonStringAttr(
                *group_attrs, "refined_subject_mask_review_status_latest"));
        }
    }

    if (run_candidates.empty() && !root_path_.empty()) {
        auto add_fs_candidates = [&](std::initializer_list<std::string> paths) {
            auto fs_candidates = collect_runs_fs(
                root_path_,
                "refined_subject_masks_runs",
                paths);
            for (auto it = fs_candidates.rbegin();
                 it != fs_candidates.rend();
                 ++it) {
                add_candidate(*it);
            }
        };
        add_fs_candidates({"masks_roi"});
        add_fs_candidates({"mask_bitpacked/masks_packed"});
        add_fs_candidates({"mask_rle"});
    }

    if (run_candidates.empty()) {
        return false;
    }

    std::string latest_run;
    std::string run_base;
    std::optional<nlohmann::json> run_attrs;
    for (const auto& candidate : run_candidates) {
        const std::string candidate_base =
            "refined_subject_masks_runs/" + candidate + "/";
        auto attrs = readAttrsAny(store, candidate_base);
        if (attrs.has_value()) {
            latest_run = candidate;
            run_base = candidate_base;
            run_attrs = std::move(attrs);
            break;
        }
        if (explicit_named_run) {
            std::cout << "[SUBJECT_MASK_WARNING] requested refined_subject_masks run '"
                      << candidate
                      << "' has no readable attrs; falling back to legacy eye masks."
                      << std::endl;
            return false;
        }
    }
    if (latest_run.empty() || !run_attrs.has_value()) {
        std::cout << "[SUBJECT_MASK_WARNING] no readable refined_subject_masks run found; "
                     "falling back to legacy eye masks."
                  << std::endl;
        return false;
    }
    trace.step("resolve_run");

    std::string storage_request =
        toLowerCopy(requested_refined_subject_mask_storage_);
    const bool force_rle =
        storage_request == "rle" || storage_request == "mask_rle" ||
        storage_request == "component_rle_v1";
    const bool force_dense =
        storage_request == "dense" || storage_request == "masks_roi" ||
        storage_request == "dense_uint8";
    const bool force_bitpacked =
        storage_request == "bitpacked" || storage_request == "mask_bitpacked" ||
        storage_request == "bitpacked_v1" ||
        storage_request == "bitpacked_binary_v1";
    if (!storage_request.empty() && storage_request != "auto" &&
        !force_rle && !force_dense && !force_bitpacked) {
        std::cout << "  [SUBJECT_MASK_WARNING] Unknown refined subject-mask storage request '"
                  << requested_refined_subject_mask_storage_
                  << "'; using dense-first auto mode." << std::endl;
    }

    std::optional<nlohmann::json> bitpacked_attrs =
        readAttrsAny(store, run_base + "mask_bitpacked");
    std::optional<nlohmann::json> rle_attrs =
        readAttrsAny(store, run_base + "mask_rle");

    std::vector<std::string> mask_labels =
        extractStringListAttr(*run_attrs, "mask_labels");
    if (mask_labels.empty() && bitpacked_attrs.has_value()) {
        mask_labels =
            extractStringListAttr(*bitpacked_attrs, "component_names");
    }
    if (mask_labels.empty() && rle_attrs.has_value()) {
        mask_labels = extractStringListAttr(*rle_attrs, "component_names");
    }
    if (mask_labels.empty()) {
        std::cout << "[SUBJECT_MASK_WARNING] refined_subject_masks run '"
                  << latest_run
                  << "' missing mask_labels/component_names; falling back to legacy eye masks."
                  << std::endl;
        return false;
    }
    trace.step("read_attrs_and_labels");

    ts::TensorStore<uint8_t, 4> dense_masks_store;
    ts::TensorStore<uint8_t, 4> bitpacked_masks_store;
    bool dense_masks_available = false;
    bool bitpacked_masks_available = false;
    if (!force_rle && !force_bitpacked) {
        auto masks_store_result =
            openArrayAny<uint8_t, 4>(store, run_base + "masks_roi", context_);
        if (masks_store_result.ok()) {
            dense_masks_store = masks_store_result.value();
            dense_masks_available = true;
        } else if (force_dense) {
            std::cout << "[SUBJECT_MASK_WARNING] Failed to open refined subject masks for run '"
                      << latest_run << "': "
                      << masks_store_result.status().ToString()
                      << "; falling back to legacy eye masks." << std::endl;
            return false;
        }
    }

    if (!dense_masks_available && !force_rle && !force_dense) {
        auto bitpacked_store_result = openArrayAny<uint8_t, 4>(
            store, run_base + "mask_bitpacked/masks_packed", context_);
        if (bitpacked_store_result.ok()) {
            bitpacked_masks_store = bitpacked_store_result.value();
            bitpacked_masks_available = true;
        } else if (force_bitpacked) {
            std::cout << "[SUBJECT_MASK_WARNING] Failed to open compact mask_bitpacked for run '"
                      << latest_run << "': "
                      << bitpacked_store_result.status().ToString()
                      << "; falling back to legacy eye masks." << std::endl;
            return false;
        }
    }

    const bool use_bitpacked_masks =
        !dense_masks_available && bitpacked_masks_available;
    const bool use_rle_masks =
        !dense_masks_available && !bitpacked_masks_available;
    if (use_bitpacked_masks && !bitpacked_attrs.has_value()) {
        std::cout << "[SUBJECT_MASK_WARNING] refined_subject_masks run '"
                  << latest_run
                  << "' has mask_bitpacked/masks_packed but no readable mask_bitpacked attrs; falling back to legacy eye masks."
                  << std::endl;
        return false;
    }
    if (use_rle_masks && !rle_attrs.has_value()) {
        std::cout << "[SUBJECT_MASK_WARNING] refined_subject_masks run '"
                  << latest_run
                  << "' has no dense masks_roi, compact mask_bitpacked, or compact mask_rle; falling back to legacy eye masks."
                  << std::endl;
        return false;
    }
    if (use_rle_masks && jsonBoolAttr(*run_attrs, "mask_rle_stale")) {
        std::cout << "[SUBJECT_MASK_WARNING] refined_subject_masks run '"
                  << latest_run
                  << "' has mask_rle_stale=true; rejecting compact RLE by default."
                  << std::endl;
        return false;
    }

    size_t roi_dim = 0;
    size_t channel_dim = 0;
    size_t mask_rows = 0;
    size_t mask_cols = 0;
    if (dense_masks_available) {
        auto domain = dense_masks_store.domain();
        auto shape = domain.shape();
        if (shape.size() != 4) {
            std::cout << "[SUBJECT_MASK_WARNING] Unexpected masks_roi rank in refined_subject_masks run '"
                      << latest_run << "' (expected 4, got " << shape.size()
                      << "); falling back to legacy eye masks." << std::endl;
            return false;
        }
        roi_dim = static_cast<size_t>(shape[0]);
        channel_dim = static_cast<size_t>(shape[1]);
        mask_rows = static_cast<size_t>(shape[2]);
        mask_cols = static_cast<size_t>(shape[3]);
    } else if (use_bitpacked_masks) {
        if (jsonStringAttr(*bitpacked_attrs, "schema_id") !=
                "palette_mask_bitpacked_binary_v1" ||
            jsonStringAttr(*bitpacked_attrs, "mask_encoding") !=
                "bitpacked_binary_v1" ||
            jsonStringAttr(*bitpacked_attrs, "mask_value_semantics") !=
                "binary_0_1" ||
            jsonStringAttr(*bitpacked_attrs, "layout") !=
                "packed_width_array" ||
            jsonStringAttr(*bitpacked_attrs, "packed_axis") != "width") {
            std::cout << "[SUBJECT_MASK_WARNING] compact mask_bitpacked attrs for run '"
                      << latest_run
                      << "' do not match palette_mask_bitpacked_binary_v1/packed_width_array."
                      << std::endl;
            return false;
        }
        const std::string bit_order =
            jsonStringAttr(*bitpacked_attrs, "packed_bitorder");
        if (!bit_order.empty() && bit_order != "little") {
            std::cout << "[SUBJECT_MASK_WARNING] compact mask_bitpacked for run '"
                      << latest_run
                      << "' uses unsupported packed_bitorder '" << bit_order
                      << "'." << std::endl;
            return false;
        }
        const auto logical_shape =
            jsonIntVector(*bitpacked_attrs, "logical_shape");
        const auto encoded_shape =
            jsonIntVector(*bitpacked_attrs, "encoded_shape");
        auto packed_domain = bitpacked_masks_store.domain();
        auto packed_shape = packed_domain.shape();
        if (logical_shape.size() != 4 || encoded_shape.size() != 4 ||
            packed_shape.size() != 4 ||
            logical_shape[0] <= 0 || logical_shape[1] <= 0 ||
            logical_shape[2] <= 0 || logical_shape[3] <= 0 ||
            encoded_shape[0] != logical_shape[0] ||
            encoded_shape[1] != logical_shape[1] ||
            encoded_shape[2] != logical_shape[2] ||
            encoded_shape[3] != (logical_shape[3] + 7) / 8 ||
            packed_shape[0] != encoded_shape[0] ||
            packed_shape[1] != encoded_shape[1] ||
            packed_shape[2] != encoded_shape[2] ||
            packed_shape[3] != encoded_shape[3]) {
            std::cout << "[SUBJECT_MASK_WARNING] compact mask_bitpacked shape metadata for run '"
                      << latest_run
                      << "' is missing or inconsistent with masks_packed."
                      << std::endl;
            return false;
        }
        roi_dim = static_cast<size_t>(logical_shape[0]);
        channel_dim = static_cast<size_t>(logical_shape[1]);
        mask_rows = static_cast<size_t>(logical_shape[2]);
        mask_cols = static_cast<size_t>(logical_shape[3]);
        if (mask_labels.size() != channel_dim) {
            std::vector<std::string> bitpacked_component_names =
                extractStringListAttr(*bitpacked_attrs, "component_names");
            if (bitpacked_component_names.size() == channel_dim) {
                mask_labels = std::move(bitpacked_component_names);
            }
        }
        if (mask_labels.size() != channel_dim) {
            std::cout << "[SUBJECT_MASK_WARNING] compact mask_bitpacked component_names length for run '"
                      << latest_run << "' is " << mask_labels.size()
                      << " but logical_shape channel count is "
                      << channel_dim << "." << std::endl;
            return false;
        }
    } else {
        if (jsonStringAttr(*rle_attrs, "schema_id") !=
                "palette_mask_rle_binary_v1" ||
            jsonStringAttr(*rle_attrs, "mask_encoding") !=
                "coco_rle_fortran_v1" ||
            jsonStringAttr(*rle_attrs, "mask_value_semantics") !=
                "binary_0_1" ||
            jsonStringAttr(*rle_attrs, "layout") != "component_groups") {
            std::cout << "[SUBJECT_MASK_WARNING] compact mask_rle attrs for run '"
                      << latest_run
                      << "' do not match palette_mask_rle_binary_v1/component_groups."
                      << std::endl;
            return false;
        }
        const auto shape_hw = jsonIntVector(*rle_attrs, "encoded_shape_hw");
        const int rle_rows_attr = jsonIntAttr(*rle_attrs, "n_rows");
        const int rle_component_count = jsonIntAttr(*rle_attrs, "component_count");
        if (shape_hw.size() != 2 || shape_hw[0] <= 0 || shape_hw[1] <= 0 ||
            rle_rows_attr <= 0 || rle_component_count <= 0) {
            std::cout << "[SUBJECT_MASK_WARNING] compact mask_rle attrs for run '"
                      << latest_run
                      << "' are missing encoded_shape_hw/n_rows/component_count."
                      << std::endl;
            return false;
        }
        roi_dim = static_cast<size_t>(rle_rows_attr);
        channel_dim = static_cast<size_t>(rle_component_count);
        mask_rows = static_cast<size_t>(shape_hw[0]);
        mask_cols = static_cast<size_t>(shape_hw[1]);
        if (mask_labels.size() != channel_dim) {
            std::vector<std::string> rle_component_names =
                extractStringListAttr(*rle_attrs, "component_names");
            if (rle_component_names.size() == channel_dim) {
                mask_labels = std::move(rle_component_names);
            }
        }
        if (mask_labels.size() != channel_dim) {
            std::cout << "[SUBJECT_MASK_WARNING] compact mask_rle component_names length for run '"
                      << latest_run << "' is " << mask_labels.size()
                      << " but component_count is " << channel_dim << "."
                      << std::endl;
            return false;
        }
    }

    if (roi_dim == 0 || channel_dim == 0 || mask_rows == 0 || mask_cols == 0) {
        std::cout << "[SUBJECT_MASK_WARNING] refined_subject_masks run '"
                  << latest_run
                  << "' has empty mask dimensions; falling back to legacy eye masks."
                  << std::endl;
        return false;
    }
    trace.step(use_rle_masks
                   ? "setup_rle_store"
                   : (use_bitpacked_masks ? "setup_bitpacked_store"
                                          : "setup_dense_store"));

    auto find_label = [&](const std::string& label) -> size_t {
        auto it = std::find(mask_labels.begin(), mask_labels.end(), label);
        if (it == mask_labels.end()) {
            return kInvalidMaskChannel;
        }
        return static_cast<size_t>(std::distance(mask_labels.begin(), it));
    };

    const size_t left_channel = find_label("eye_left");
    const size_t right_channel = find_label("eye_right");

    std::vector<uint8_t> available_channels;
    bool available_loaded =
        readBoolArray(store, run_base + "available_channels", available_channels);
    bool tolerant_metadata = false;
    std::string warning;
    auto appendWarningText = [&](const std::string& message) {
        if (message.empty()) {
            return;
        }
        if (!warning.empty()) {
            warning += " ";
        }
        warning += message;
    };
    if (!available_loaded || available_channels.size() != channel_dim) {
        const size_t observed_available_count = available_channels.size();
        available_channels.assign(channel_dim, 1);
        tolerant_metadata = true;
        std::ostringstream oss;
        oss << "available_channels ";
        if (!available_loaded) {
            oss << "missing/unreadable";
        } else {
            oss << "length " << observed_available_count
                << " did not match masks_roi channel count " << channel_dim;
        }
        oss << "; treating labeled channels as readable for UI inspection.";
        appendWarningText(oss.str());
    }
    trace.step("map_channels");

    auto channel_is_available = [&](size_t channel) -> bool {
        if (channel == kInvalidMaskChannel || channel >= channel_dim) {
            return false;
        }
        if (channel >= available_channels.size()) {
            return tolerant_metadata;
        }
        return available_channels[channel] != 0;
    };

    const bool left_available = channel_is_available(left_channel);
    const bool right_available = channel_is_available(right_channel);

    if (roi_count > 0 && roi_dim != roi_count) {
        std::ostringstream oss;
        oss << "masks_roi row count " << roi_dim
            << " does not match keypoint ROI count " << roi_count
            << "; using frame_indices/source_crop_row_ids for placement.";
        appendWarningText(oss.str());
    }

    std::vector<int32_t> mask_frame_indices;
    bool row_position_fallback = false;
    const bool frame_indices_loaded =
        readInt32Array(store, run_base + "frame_indices", mask_frame_indices);
    if (!frame_indices_loaded || mask_frame_indices.size() != roi_dim) {
        if (roi_count > 0 && roi_dim == roi_count &&
            !data_.mask_roi_indices.empty() && !data_.frame_indices.empty()) {
            row_position_fallback = true;
            mask_frame_indices.assign(roi_dim, -1);
            for (size_t det_row = 0;
                 det_row < data_.mask_roi_indices.size() &&
                 det_row < data_.frame_indices.size();
                 ++det_row) {
                const int32_t mask_row = data_.mask_roi_indices[det_row];
                if (mask_row < 0 ||
                    static_cast<size_t>(mask_row) >= mask_frame_indices.size()) {
                    continue;
                }
                if (mask_frame_indices[static_cast<size_t>(mask_row)] < 0) {
                    mask_frame_indices[static_cast<size_t>(mask_row)] =
                        data_.frame_indices[det_row];
                }
            }
            appendWarningText(
                "frame_indices missing or mismatched; used legacy detection-row alignment fallback.");
        } else {
            std::cout << "[SUBJECT_MASK_WARNING] refined_subject_masks run '"
                      << latest_run
                      << "' missing mandatory frame_indices; falling back to legacy eye masks."
                      << std::endl;
            return false;
        }
    }
    trace.step("read_frame_indices");

    std::string source_crop_run =
        NormalizeCropRunName(jsonStringAttr(*run_attrs, "source_crop_run"));
    if (source_crop_run.empty() && !data_.keypoints_source_crop_run.empty()) {
        source_crop_run = NormalizeCropRunName(data_.keypoints_source_crop_run);
        row_position_fallback = true;
        appendWarningText(
            "source_crop_run missing; using keypoint source_crop_run legacy fallback.");
    }
    if (source_crop_run.empty()) {
        std::cout << "[SUBJECT_MASK_WARNING] refined_subject_masks run '"
                  << latest_run
                  << "' missing source_crop_run; cannot place ROI-local masks."
                  << std::endl;
        return false;
    }

    const std::string crop_base = "crop_runs/" + source_crop_run + "/";
    std::vector<int32_t> crop_frame_indices;
    if (!readInt32Array(store, crop_base + "frame_indices", crop_frame_indices)) {
        std::cout << "[SUBJECT_MASK_WARNING] source crop run '"
                  << source_crop_run
                  << "' missing frame_indices; cannot verify mask crop rows."
                  << std::endl;
        return false;
    }

    std::vector<std::array<float, 2>> crop_offsets;
    auto readCropOffsets = [&](auto type_token) -> bool {
        using Source = decltype(type_token);
        auto coords_store = openArrayAny<Source, 2>(
            store, crop_base + "roi_coordinates_full", context_);
        if (!coords_store.ok()) {
            return false;
        }
        auto coords_result = ts::Read(coords_store.value()).result();
        if (!coords_result.ok()) {
            return false;
        }
        auto coords = coords_result.value();
        const auto coords_shape = coords.shape();
        if (coords_shape.size() != 2 || coords_shape[1] < 2) {
            return false;
        }
        const size_t crop_rows = static_cast<size_t>(coords_shape[0]);
        crop_offsets.resize(crop_rows);
        for (size_t row = 0; row < crop_rows; ++row) {
            crop_offsets[row] = {
                static_cast<float>(
                    coords(static_cast<ts::Index>(row), static_cast<ts::Index>(0))),
                static_cast<float>(
                    coords(static_cast<ts::Index>(row), static_cast<ts::Index>(1)))};
        }
        return true;
    };
    if (!readCropOffsets(int32_t{}) &&
        !readCropOffsets(float{}) &&
        !readCropOffsets(double{})) {
        std::cout << "[SUBJECT_MASK_WARNING] source crop run '"
                  << source_crop_run
                  << "' missing readable roi_coordinates_full; cannot place ROI-local masks."
                  << std::endl;
        return false;
    }

    float roi_width_px = static_cast<float>(mask_cols);
    float roi_height_px = static_cast<float>(mask_rows);
    if (auto crop_attrs = readAttrsAny(store, crop_base)) {
        if (crop_attrs->contains("roi_size") && (*crop_attrs)["roi_size"].is_array() &&
            (*crop_attrs)["roi_size"].size() >= 2) {
            const auto& roi_size = (*crop_attrs)["roi_size"];
            if (roi_size[0].is_number() && roi_size[1].is_number()) {
                roi_height_px = static_cast<float>(roi_size[0].get<double>());
                roi_width_px = static_cast<float>(roi_size[1].get<double>());
            }
        }
    }

    std::vector<int64_t> source_crop_row_ids;
    const bool source_crop_rows_loaded =
        readInt64Array(store,
                       run_base + "source_crop_row_ids",
                       source_crop_row_ids);
    if (!source_crop_rows_loaded || source_crop_row_ids.size() != roi_dim) {
        if (roi_dim <= crop_offsets.size() &&
            roi_dim <= crop_frame_indices.size()) {
            source_crop_row_ids.resize(roi_dim);
            for (size_t row = 0; row < roi_dim; ++row) {
                source_crop_row_ids[row] = static_cast<int64_t>(row);
            }
            row_position_fallback = true;
            appendWarningText(
                "source_crop_row_ids missing or mismatched; used legacy row-position fallback.");
        } else {
            std::cout << "[SUBJECT_MASK_WARNING] refined_subject_masks run '"
                      << latest_run
                      << "' missing mandatory source_crop_row_ids; cannot place masks."
                      << std::endl;
            return false;
        }
    }
    trace.step("read_source_crop_mapping");

    std::vector<float> refined_offset_x(
        roi_dim, std::numeric_limits<float>::quiet_NaN());
    std::vector<float> refined_offset_y(
        roi_dim, std::numeric_limits<float>::quiet_NaN());
    std::vector<float> refined_roi_width(roi_dim, roi_width_px);
    std::vector<float> refined_roi_height(roi_dim, roi_height_px);
    std::vector<int32_t> refined_crop_frame(roi_dim, -1);
    std::vector<uint8_t> crop_frame_match(roi_dim, 0);
    size_t out_of_bounds_crop_rows = 0;
    size_t crop_frame_mismatches = 0;
    size_t valid_placements = 0;
    int32_t max_mask_frame = -1;
    for (size_t mask_row = 0; mask_row < roi_dim; ++mask_row) {
        const int32_t mask_frame =
            mask_row < mask_frame_indices.size() ? mask_frame_indices[mask_row] : -1;
        if (mask_frame >= 0) {
            max_mask_frame = std::max(max_mask_frame, mask_frame);
        }
        const int64_t crop_row =
            mask_row < source_crop_row_ids.size()
                ? source_crop_row_ids[mask_row]
                : -1;
        if (crop_row < 0 ||
            static_cast<size_t>(crop_row) >= crop_offsets.size() ||
            static_cast<size_t>(crop_row) >= crop_frame_indices.size()) {
            ++out_of_bounds_crop_rows;
            continue;
        }
        refined_crop_frame[mask_row] =
            crop_frame_indices[static_cast<size_t>(crop_row)];
        if (refined_crop_frame[mask_row] != mask_frame) {
            ++crop_frame_mismatches;
            continue;
        }
        const auto offset = crop_offsets[static_cast<size_t>(crop_row)];
        if (!std::isfinite(offset[0]) || !std::isfinite(offset[1]) ||
            roi_width_px <= 0.0f || roi_height_px <= 0.0f) {
            continue;
        }
        refined_offset_x[mask_row] = offset[0];
        refined_offset_y[mask_row] = offset[1];
        refined_roi_width[mask_row] = roi_width_px;
        refined_roi_height[mask_row] = roi_height_px;
        crop_frame_match[mask_row] = 1;
        ++valid_placements;
    }
    if (valid_placements == 0) {
        std::cout << "[SUBJECT_MASK_WARNING] refined_subject_masks run '"
                  << latest_run
                  << "' had no mask rows with verified source crop placement."
                  << std::endl;
        return false;
    }
    if (out_of_bounds_crop_rows > 0) {
        appendWarningText(
            std::to_string(out_of_bounds_crop_rows) +
            " mask rows referenced out-of-bounds source_crop_row_ids.");
    }
    if (crop_frame_mismatches > 0) {
        appendWarningText(
            std::to_string(crop_frame_mismatches) +
            " mask rows failed crop frame verification and will not be displayed.");
    }

    std::vector<std::vector<size_t>> mask_rows_by_frame;
    const size_t frame_lookup_size = std::max<size_t>(
        data_.total_frames,
        max_mask_frame >= 0 ? static_cast<size_t>(max_mask_frame) + 1 : 0);
    mask_rows_by_frame.resize(frame_lookup_size);
    for (size_t mask_row = 0; mask_row < roi_dim; ++mask_row) {
        const int32_t frame =
            mask_row < mask_frame_indices.size() ? mask_frame_indices[mask_row] : -1;
        if (frame < 0 || static_cast<size_t>(frame) >= mask_rows_by_frame.size() ||
            crop_frame_match[mask_row] == 0) {
            continue;
        }
        mask_rows_by_frame[static_cast<size_t>(frame)].push_back(mask_row);
    }
    trace.step("verify_placements");

    const char* mask_storage_surface =
        use_rle_masks ? "mask_rle"
                      : (use_bitpacked_masks ? "mask_bitpacked" : "masks_roi");
    data_.mask_chunk_cache.clear();
    data_.eye_masks_store = dense_masks_available
        ? dense_masks_store
        : ts::TensorStore<uint8_t, 4>();
    data_.eye_masks_bitpacked_store = use_bitpacked_masks
        ? bitpacked_masks_store
        : ts::TensorStore<uint8_t, 4>();
    data_.eye_masks_run_name = latest_run;
    data_.eye_masks_source_label = tolerant_metadata
        ? (use_rle_masks
               ? "Refined subject masks RLE (metadata inferred)"
               : (use_bitpacked_masks
                      ? "Refined subject masks bitpacked (metadata inferred)"
                      : "Refined subject masks (metadata inferred)"))
        : (use_rle_masks
               ? "Refined subject masks RLE"
               : (use_bitpacked_masks ? "Refined subject masks bitpacked"
                                      : "Refined subject masks"));
    data_.eye_masks_source_path =
        run_base + mask_storage_surface;
    data_.eye_masks_warning = warning;
    data_.eye_masks_from_refined_subject_masks = true;
    data_.eye_masks_tolerant_metadata = tolerant_metadata;
    data_.eye_masks_loaded = true;
    data_.has_eye_masks = true;
    data_.eye_mask_roi_count = roi_dim;
    data_.eye_mask_height = mask_rows;
    data_.eye_mask_width = mask_cols;
    data_.eye_mask_channel_indices = {
        left_available ? left_channel : kInvalidMaskChannel,
        right_available ? right_channel : kInvalidMaskChannel};
    data_.eye_mask_channel_labels = {"eye_left", "eye_right"};
    data_.refined_subject_mask_labels = mask_labels;
    data_.refined_subject_mask_available_channels = available_channels;
    data_.refined_subject_mask_label_schema_id =
        jsonStringAttr(*run_attrs, "label_schema_id");
    data_.refined_subject_mask_source_crop_run = source_crop_run;
    data_.refined_subject_mask_frame_indices = std::move(mask_frame_indices);
    data_.refined_subject_mask_source_crop_row_ids =
        std::move(source_crop_row_ids);
    data_.refined_subject_mask_offset_x = std::move(refined_offset_x);
    data_.refined_subject_mask_offset_y = std::move(refined_offset_y);
    data_.refined_subject_mask_roi_width_px = std::move(refined_roi_width);
    data_.refined_subject_mask_roi_height_px = std::move(refined_roi_height);
    data_.refined_subject_mask_source_crop_frame_indices =
        std::move(refined_crop_frame);
    data_.refined_subject_mask_crop_frame_match = std::move(crop_frame_match);
    data_.refined_subject_mask_rows_by_frame = std::move(mask_rows_by_frame);
    data_.refined_subject_mask_row_position_fallback = row_position_fallback;
    data_.refined_subject_mask_dense_masks_used = dense_masks_available;
    data_.refined_subject_mask_bitpacked_masks_used = use_bitpacked_masks;
    data_.refined_subject_mask_rle_masks_used = use_rle_masks;
    data_.refined_subject_mask_overlay_components.clear();
    const size_t component_label_count =
        std::min(mask_labels.size(), channel_dim);
    for (size_t channel = 0; channel < component_label_count; ++channel) {
        if (!channel_is_available(channel)) {
            continue;
        }
        data_.refined_subject_mask_overlay_components.push_back(
            ZarrDetectionData::RefinedSubjectMaskComponentInfo{
                mask_labels[channel],
                channel});
    }
    trace.step("publish_metadata");

    auto readRleBbox =
        [&](const std::string& path,
            std::vector<std::array<int32_t, 4>>& out) -> bool {
        auto bbox_store = openArrayAny<int32_t, 2>(store, path, context_);
        if (!bbox_store.ok()) {
            return false;
        }
        auto bbox_result = ts::Read(bbox_store.value()).result();
        if (!bbox_result.ok()) {
            return false;
        }
        auto bbox = bbox_result.value();
        auto bbox_shape = bbox.shape();
        if (bbox_shape.size() != 2 ||
            bbox_shape[0] != static_cast<ts::Index>(roi_dim) ||
            bbox_shape[1] < 4) {
            return false;
        }
        out.resize(roi_dim);
        for (size_t row = 0; row < roi_dim; ++row) {
            for (size_t xy = 0; xy < 4; ++xy) {
                out[row][xy] = bbox(static_cast<ts::Index>(row),
                                    static_cast<ts::Index>(xy));
            }
        }
        return true;
    };

    auto findRleComponentBase =
        [&](size_t component_index,
            const std::string& component_name) -> std::string {
        const std::string rle_components_base =
            run_base + "mask_rle/components/";
        std::vector<std::string> candidates = {
            rle_components_base +
                maskRleComponentGroupName(component_index,
                                          component_name,
                                          true) +
                "/",
            rle_components_base +
                maskRleComponentGroupName(component_index,
                                          component_name,
                                          false) +
                "/",
        };
        auto attrs_match = [&](const nlohmann::json& attrs) -> bool {
            const std::string attr_name =
                jsonStringAttr(attrs, "component_name");
            const int attr_index = jsonIntAttr(attrs, "component_index");
            return (attr_name.empty() || attr_name == component_name) &&
                   (attr_index < 0 ||
                    static_cast<size_t>(attr_index) == component_index);
        };
        for (const auto& candidate : candidates) {
            if (auto attrs = readAttrsAny(store, candidate)) {
                if (attrs_match(*attrs)) {
                    return candidate;
                }
            }
        }
        if (!root_path_.empty()) {
            try {
                const std::filesystem::path components_path =
                    std::filesystem::path(root_path_) / run_base /
                    "mask_rle" / "components";
                if (std::filesystem::exists(components_path)) {
                    for (const auto& entry :
                         std::filesystem::directory_iterator(components_path)) {
                        if (!entry.is_directory()) {
                            continue;
                        }
                        const std::string rel =
                            rle_components_base +
                            entry.path().filename().string() + "/";
                        if (auto attrs = readAttrsAny(store, rel)) {
                            if (attrs_match(*attrs)) {
                                return rel;
                            }
                        }
                    }
                }
            } catch (const std::exception&) {
            }
        }
        return {};
    };

    auto loadRleComponent =
        [&](ZarrDetectionData::RefinedSubjectMaskComponentInfo& component)
            -> bool {
        const std::string component_base =
            findRleComponentBase(component.channel_index, component.label);
        if (component_base.empty()) {
            std::cout << "  [SUBJECT_MASK_WARNING] compact mask_rle component '"
                      << component.label << "' was not found." << std::endl;
            return false;
        }
        auto counts_store =
            openArrayAny<uint32_t, 1>(store, component_base + "counts", context_);
        if (!counts_store.ok()) {
            std::cout << "  [SUBJECT_MASK_WARNING] compact mask_rle component '"
                      << component.label << "' has no readable counts: "
                      << counts_store.status().ToString() << std::endl;
            return false;
        }
        const auto counts_shape = counts_store.value().domain().shape();
        if (counts_shape.size() != 1) {
            std::cout << "  [SUBJECT_MASK_WARNING] compact mask_rle component '"
                      << component.label << "' counts rank is not 1."
                      << std::endl;
            return false;
        }
        std::vector<int64_t> indptr;
        std::vector<uint8_t> present;
        std::vector<int32_t> area_px;
        if (!readInt64Array(store, component_base + "indptr", indptr) ||
            !readBoolArray(store, component_base + "present", present) ||
            !readInt32Array(store, component_base + "area_px", area_px)) {
            std::cout << "  [SUBJECT_MASK_WARNING] compact mask_rle component '"
                      << component.label
                      << "' is missing indptr/present/area_px arrays."
                      << std::endl;
            return false;
        }
        if (indptr.size() != roi_dim + 1 || present.size() != roi_dim ||
            area_px.size() != roi_dim) {
            std::cout << "  [SUBJECT_MASK_WARNING] compact mask_rle component '"
                      << component.label
                      << "' row arrays do not match mask row count "
                      << roi_dim << "." << std::endl;
            return false;
        }
        component.rle_counts_store = counts_store.value();
        component.rle_counts_count =
            static_cast<size_t>(std::max<ts::Index>(0, counts_shape[0]));
        component.rle_indptr = std::move(indptr);
        component.rle_present = std::move(present);
        component.rle_area_px = std::move(area_px);
        readRleBbox(component_base + "bbox_xyxy", component.rle_bbox_xyxy);
        component.rle_available = true;
        return true;
    };

    if (use_rle_masks) {
        size_t rle_component_count = 0;
        for (auto& component : data_.refined_subject_mask_overlay_components) {
            if (loadRleComponent(component)) {
                ++rle_component_count;
            }
        }
        if (rle_component_count == 0) {
            std::cout << "[SUBJECT_MASK_WARNING] compact mask_rle for run '"
                      << latest_run
                      << "' had no readable components; falling back to legacy eye masks."
                      << std::endl;
            return false;
        }
    }
    trace.step("rle_components");

    size_t contour_component_count = 0;
    const bool eager_optional_overlays =
        crimson_env_flag_enabled("CRIMSON_EAGER_SUBJECT_MASK_OPTIONAL_OVERLAYS");
    if (eager_optional_overlays) {
    auto loadComponentContours =
        [&](ZarrDetectionData::RefinedSubjectMaskComponentInfo& component) {
            const std::string contour_base =
                run_base + "components/" + component.label + "/contours/";

            std::vector<int64_t> contour_ptr;
            std::vector<int32_t> contour_len;
            if (!readInt64Array(store, contour_base + "ptr", contour_ptr) ||
                !readInt32Array(store, contour_base + "len", contour_len)) {
                return;
            }
            if (contour_ptr.size() != roi_dim || contour_len.size() != roi_dim) {
                component.contour_warning =
                    "contour ptr/len shape does not match masks_roi rows";
                std::cout << "  [SUBJECT_MASK_WARNING] Refined subject mask component '"
                          << component.label << "' "
                          << component.contour_warning << "." << std::endl;
                return;
            }

            auto points_store =
                openArrayAny<float, 2>(store, contour_base + "points_xy", context_);
            if (!points_store.ok()) {
                component.contour_warning =
                    "contour points_xy is missing or unreadable";
                std::cout << "  [SUBJECT_MASK_WARNING] Refined subject mask component '"
                          << component.label << "' "
                          << component.contour_warning << ": "
                          << points_store.status().ToString()
                          << std::endl;
                return;
            }

            const auto points_shape = points_store.value().domain().shape();
            if (points_shape.size() != 2 || points_shape[1] < 2) {
                component.contour_warning =
                    "contour points_xy shape is not Nx2";
                std::cout << "  [SUBJECT_MASK_WARNING] Refined subject mask component '"
                          << component.label << "' "
                          << component.contour_warning << "." << std::endl;
                return;
            }

            bool attrs_compatible = false;
            if (auto attrs = readAttrsAny(store, contour_base)) {
                const bool schema_ok =
                    stringAttrMatches(*attrs, "schema_id", "component_contours_v1") ||
                    stringAttrMatches(*attrs, "contour_schema_id", "component_contours_v1");
                const bool coordinate_ok =
                    !attrs->contains("coordinate_space") ||
                    stringAttrMatches(*attrs, "coordinate_space", "roi_pixels");
                const bool order_ok =
                    !attrs->contains("point_order") ||
                    stringAttrMatches(*attrs, "point_order", "xy");
                const bool source_ok =
                    !attrs->contains("source_component") ||
                    stringAttrMatches(*attrs,
                                      "source_component",
                                      component.label.c_str());
                attrs_compatible =
                    schema_ok && coordinate_ok && order_ok && source_ok;
                if (!attrs_compatible) {
                    component.contour_warning =
                        "contour attrs are incomplete or not fully compatible; loading arrays tolerantly";
                }
            } else {
                component.contour_warning =
                    "contour attrs are missing; loading arrays tolerantly";
            }

            component.contour_ptr = std::move(contour_ptr);
            component.contour_len = std::move(contour_len);
            component.contour_points_store = points_store.value();
            component.contour_points_count =
                static_cast<size_t>(points_shape[0]);
            component.contours_available = true;
            component.contour_attrs_compatible = attrs_compatible;
        };

    contour_component_count = 0;
    for (auto& component : data_.refined_subject_mask_overlay_components) {
        loadComponentContours(component);
        if (component.contours_available) {
            ++contour_component_count;
        }
    }
    trace.step("component_contours");

    auto loadEllipseAxes =
        [&](const std::string& component_label, size_t eye_slot) -> bool {
        if (eye_slot >= 2) {
            return false;
        }

        const std::string geometry_base =
            run_base + "components/" + component_label + "/geometry/";
        auto ellipse_store =
            openArrayAny<float, 2>(store, geometry_base + "ellipse_params", context_);
        if (!ellipse_store.ok()) {
            std::cout << "  [SUBJECT_MASK_WARNING] Refined subject mask component '"
                      << component_label
                      << "' has no readable ellipse_params; axis overlay unavailable for this eye."
                      << std::endl;
            return false;
        }

        auto ellipse_result = ts::Read(ellipse_store.value()).result();
        if (!ellipse_result.ok()) {
            std::cout << "  [SUBJECT_MASK_WARNING] Failed to read ellipse_params for refined subject mask component '"
                      << component_label << "': "
                      << ellipse_result.status().ToString()
                      << std::endl;
            return false;
        }

        auto ellipse_array = ellipse_result.value();
        auto ellipse_shape = ellipse_array.shape();
        if (ellipse_shape.size() != 2 ||
            ellipse_shape[0] != static_cast<ts::Index>(roi_dim) ||
            ellipse_shape[1] < 5) {
            std::cout << "  [SUBJECT_MASK_WARNING] Unexpected ellipse_params shape for refined subject mask component '"
                      << component_label << "' (expected " << roi_dim
                      << "x5, got ";
            for (size_t i = 0; i < ellipse_shape.size(); ++i) {
                std::cout << ellipse_shape[i]
                          << (i + 1 < ellipse_shape.size() ? "x" : "");
            }
            std::cout << "); axis overlay unavailable for this eye."
                      << std::endl;
            return false;
        }

        std::vector<uint8_t> ellipse_success;
        const bool success_loaded =
            readBoolArray(store, geometry_base + "ellipse_success", ellipse_success);
        if (!success_loaded) {
            ellipse_success.assign(roi_dim, 1);
            std::cout << "  [SUBJECT_MASK_WARNING] Refined subject mask component '"
                      << component_label
                      << "' missing ellipse_success; using finite ellipse_params for axis overlay."
                      << std::endl;
        } else if (ellipse_success.size() != roi_dim) {
            std::cout << "  [SUBJECT_MASK_WARNING] ellipse_success length for refined subject mask component '"
                      << component_label << "' is " << ellipse_success.size()
                      << " but expected " << roi_dim
                      << "; missing rows will be treated as invalid."
                      << std::endl;
        }

        ensureEyeAxisRows(data_.eye_mask_feret_axes_major, roi_dim);
        ensureEyeAxisRows(data_.eye_mask_feret_axes_minor, roi_dim);

        constexpr float kPi = 3.14159265358979323846f;
        size_t valid_axes = 0;
        for (size_t roi = 0; roi < roi_dim; ++roi) {
            if (roi >= ellipse_success.size() || ellipse_success[roi] == 0) {
                continue;
            }

            const float cx =
                ellipse_array(static_cast<ts::Index>(roi), static_cast<ts::Index>(0));
            const float cy =
                ellipse_array(static_cast<ts::Index>(roi), static_cast<ts::Index>(1));
            const float major =
                ellipse_array(static_cast<ts::Index>(roi), static_cast<ts::Index>(2));
            const float minor =
                ellipse_array(static_cast<ts::Index>(roi), static_cast<ts::Index>(3));
            const float angle_deg =
                ellipse_array(static_cast<ts::Index>(roi), static_cast<ts::Index>(4));

            if (!std::isfinite(cx) || !std::isfinite(cy) ||
                !std::isfinite(major) || !std::isfinite(minor) ||
                !std::isfinite(angle_deg) || major <= 1e-5f || minor <= 1e-5f) {
                continue;
            }

            const float theta = angle_deg * kPi / 180.0f;
            const float major_dx = std::cos(theta) * major * 0.5f;
            const float major_dy = std::sin(theta) * major * 0.5f;
            const float minor_theta = theta + 0.5f * kPi;
            const float minor_dx = std::cos(minor_theta) * minor * 0.5f;
            const float minor_dy = std::sin(minor_theta) * minor * 0.5f;

            data_.eye_mask_feret_axes_major[roi][eye_slot] = {
                cx - major_dx,
                cy - major_dy,
                cx + major_dx,
                cy + major_dy};
            data_.eye_mask_feret_axes_minor[roi][eye_slot] = {
                cx - minor_dx,
                cy - minor_dy,
                cx + minor_dx,
                cy + minor_dy};
            ++valid_axes;
        }

        if (valid_axes == 0) {
            std::cout << "  [SUBJECT_MASK_WARNING] Refined subject mask component '"
                      << component_label
                      << "' ellipse geometry contained no valid axis rows."
                      << std::endl;
            return false;
        }

        return true;
    };

    const bool left_geometry_ok =
        left_available && loadEllipseAxes("eye_left", 0);
    const bool right_geometry_ok =
        right_available && loadEllipseAxes("eye_right", 1);
    data_.eye_masks_have_feret_axes = left_geometry_ok || right_geometry_ok;
    trace.step("ellipse_geometry");
    } else {
        trace.step("component_contours_deferred");
        data_.eye_masks_have_feret_axes = false;
        trace.step("ellipse_geometry_deferred");
    }

    data_.eye_mask_chunk_rows = 0;
    if (dense_masks_available) {
        auto chunk_layout_result = data_.eye_masks_store.chunk_layout();
        if (chunk_layout_result.ok()) {
            const auto& chunk_layout = chunk_layout_result.value();
            auto chunk_shape = chunk_layout.read_chunk_shape();
            if (!chunk_shape.empty()) {
                auto chunk_size = chunk_shape[0];
                if (chunk_size > 0) {
                    data_.eye_mask_chunk_rows =
                        static_cast<size_t>(chunk_size);
                }
            }
        }
    } else if (use_bitpacked_masks) {
        auto chunk_layout_result =
            data_.eye_masks_bitpacked_store.chunk_layout();
        if (chunk_layout_result.ok()) {
            const auto& chunk_layout = chunk_layout_result.value();
            auto chunk_shape = chunk_layout.read_chunk_shape();
            if (!chunk_shape.empty()) {
                auto chunk_size = chunk_shape[0];
                if (chunk_size > 0) {
                    data_.eye_mask_chunk_rows =
                        static_cast<size_t>(chunk_size);
                }
            }
        }
    }
    if (use_rle_masks) {
        data_.eye_mask_chunk_rows =
            std::min<size_t>(roi_dim, kRleEyeMaskChunkRows);
    } else if (data_.eye_mask_chunk_rows == 0) {
        data_.eye_mask_chunk_rows =
            std::min<size_t>(roi_dim, 512);
    }
    trace.step("chunk_metadata");

    std::cout << "  Refined subject mask run '" << latest_run
              << "' loaded for eye overlay (source: "
              << mask_storage_surface
              << "; labels: "
              << joinMaskLabels(mask_labels) << "; eye_left channel "
              << (left_available ? std::to_string(left_channel) : "unavailable")
              << ", eye_right channel "
              << (right_available ? std::to_string(right_channel) : "unavailable")
              << "; source_crop_run " << source_crop_run
              << "; verified placements " << valid_placements << "/"
              << roi_dim
              << "; row-position fallback "
              << (row_position_fallback ? "yes" : "no")
              << "; ellipse axes "
              << (data_.eye_masks_have_feret_axes ? "loaded" : "unavailable")
              << "; contours " << contour_component_count << "/"
              << data_.refined_subject_mask_overlay_components.size()
              << " components"
              << ")" << std::endl;
    if (!warning.empty()) {
        std::cout << "  [SUBJECT_MASK_WARNING] " << warning << std::endl;
    }
    {
        std::lock_guard<std::mutex> overlay_lock(
            refined_subject_mask_optional_overlay_mutex_);
        ++refined_subject_mask_optional_overlay_generation_;
        refined_subject_mask_optional_overlay_requested_ = false;
        refined_subject_mask_optional_overlay_loading_ = false;
        refined_subject_mask_optional_overlay_loaded_ =
            eager_optional_overlays && data_.eye_masks_have_feret_axes &&
            contour_component_count > 0;
        refined_subject_mask_optional_overlay_failed_ = false;
        refined_subject_mask_optional_overlay_error_.clear();
    }
    {
        std::lock_guard<std::mutex> cache_lock(*data_.mask_chunk_cache_mutex);
        ++refined_subject_mask_optional_overlay_publish_generation_;
    }
    return true;
}

bool ZarrDetectionLoader::loadRefinedEyeMaskData(const ts::kvstore::KvStore& store,
                                                 size_t roi_count) {
    stopEyeMaskPrefetchWorker();
    clearEyeMaskState(data_);

    if (data_.layout != ZarrLayoutType::kPaletteRuns) {
        return false;
    }

    std::string latest_run;
    if (auto group_attrs = readAttrsAny(store, "refined_eye_masks_runs")) {
        latest_run = extractLatestRunName(*group_attrs);
    }

    if (latest_run.empty() && !root_path_.empty()) {
        auto fs_candidates = collect_runs_fs(
            root_path_,
            "refined_eye_masks_runs",
            {"masks_roi"});
        if (!fs_candidates.empty()) {
            latest_run = fs_candidates.back();
        }
    }

    if (latest_run.empty()) {
        return false;
    }

    std::string run_base = "refined_eye_masks_runs/" + latest_run + "/";

    std::vector<int32_t> mask_frame_indices;
    if (!readInt32Array(store, run_base + "frame_indices", mask_frame_indices)) {
        std::cout << "[EYE_MASK_WARNING] refined_eye_masks run '" << latest_run
                  << "' missing frame_indices; skipping mask overlay." << std::endl;
        return false;
    }
    if (mask_frame_indices.size() != roi_count) {
        std::cout << "[EYE_MASK_WARNING] refined_eye_masks run '" << latest_run
                  << "' frame_indices size (" << mask_frame_indices.size()
                  << ") does not match keypoint ROI count (" << roi_count << ")." << std::endl;
        if (mask_frame_indices.empty()) {
            return false;
        }
    }

    auto masks_store_result =
        openArrayAny<uint8_t, 4>(store, run_base + "masks_roi", context_);
    if (!masks_store_result.ok()) {
        std::cout << "[EYE_MASK_WARNING] Failed to open refined eye masks for run '"
                  << latest_run << "': " << masks_store_result.status().ToString() << std::endl;
        return false;
    }

    auto domain = masks_store_result.value().domain();
    auto shape = domain.shape();
    if (shape.size() != 4) {
        std::cout << "[EYE_MASK_WARNING] Unexpected masks_roi rank in run '"
                  << latest_run << "' (expected 4, got " << shape.size() << ")." << std::endl;
        return false;
    }

    size_t roi_dim = static_cast<size_t>(shape[0]);
    size_t channel_dim = static_cast<size_t>(shape[1]);
    size_t mask_rows = static_cast<size_t>(shape[2]);
    size_t mask_cols = static_cast<size_t>(shape[3]);

    if (roi_dim == 0 || channel_dim == 0 || mask_rows == 0 || mask_cols == 0) {
        std::cout << "[EYE_MASK_WARNING] masks_roi dataset '" << latest_run
                  << "' has empty dimensions; skipping." << std::endl;
        return false;
    }

    data_.mask_chunk_cache.clear();
    data_.eye_masks_store = masks_store_result.value();
    data_.eye_masks_run_name = latest_run;
    data_.eye_masks_source_label = "Legacy refined eye masks";
    data_.eye_masks_source_path = run_base + "masks_roi";
    data_.eye_mask_channel_indices = {0, 1};
    data_.eye_mask_channel_labels = {"left", "right"};
    data_.eye_masks_loaded = true;
    data_.has_eye_masks = true;
    data_.eye_mask_roi_count = roi_dim;
    data_.eye_mask_height = mask_rows;
    data_.eye_mask_width = mask_cols;
    data_.eye_mask_chunk_rows = 0;
    auto chunk_layout_result = data_.eye_masks_store.chunk_layout();
    if (chunk_layout_result.ok()) {
        const auto& chunk_layout = chunk_layout_result.value();
        auto chunk_shape = chunk_layout.read_chunk_shape();
        if (!chunk_shape.empty()) {
            auto chunk_size = chunk_shape[0];
            if (chunk_size > 0) {
                data_.eye_mask_chunk_rows =
                    static_cast<size_t>(chunk_size);
            }
        }
    }
    if (data_.eye_mask_chunk_rows == 0) {
        data_.eye_mask_chunk_rows = std::min<size_t>(roi_dim, 512);
    }

    auto loadFeretAxes = [&](const std::string& dataset_name,
                             std::vector<std::array<std::array<float, 4>, 2>>& target) -> bool {
        target.clear();
        auto axes_store =
            openArrayAny<float, 3>(store, run_base + dataset_name, context_);
        if (!axes_store.ok()) {
            return false;
        }
        auto axes_result = ts::Read(axes_store.value()).result();
        if (!axes_result.ok()) {
            std::cout << "[EYE_MASK_WARNING] Failed to read " << dataset_name
                      << " for run '" << latest_run << "': "
                      << axes_result.status().ToString() << std::endl;
            return false;
        }

        auto axes_array = axes_result.value();
        auto axes_shape = axes_array.shape();
        if (axes_shape.size() != 3 ||
            axes_shape[0] != static_cast<ts::Index>(roi_dim) ||
            axes_shape[1] < 1 || axes_shape[2] < 4) {
            std::cout << "[EYE_MASK_WARNING] Unexpected shape for " << dataset_name
                      << " in run '" << latest_run << "' (expected "
                      << roi_dim << "x2x4, got ";
            for (size_t i = 0; i < axes_shape.size(); ++i) {
                std::cout << axes_shape[i] << (i + 1 < axes_shape.size() ? "x" : "");
            }
            std::cout << ")." << std::endl;
            return false;
        }

        const float nan_value = std::numeric_limits<float>::quiet_NaN();
        target.resize(roi_dim);
        for (auto& roi_entry : target) {
            roi_entry = {std::array<float, 4>{nan_value, nan_value, nan_value, nan_value},
                         std::array<float, 4>{nan_value, nan_value, nan_value, nan_value}};
        }

        const size_t eye_dim = std::min<size_t>(2, static_cast<size_t>(axes_shape[1]));
        const size_t axis_len = std::min<size_t>(4, static_cast<size_t>(axes_shape[2]));

        for (size_t roi = 0; roi < roi_dim; ++roi) {
            for (size_t eye = 0; eye < eye_dim; ++eye) {
                std::array<float, 4> values = {nan_value, nan_value, nan_value, nan_value};
                bool all_finite = true;
                for (size_t idx = 0; idx < axis_len; ++idx) {
                    float value = axes_array( static_cast<ts::Index>(roi),
                                             static_cast<ts::Index>(eye),
                                             static_cast<ts::Index>(idx));
                    values[idx] = value;
                    if (!std::isfinite(value)) {
                        all_finite = false;
                        break;
                    }
                }
                if (!all_finite) {
                    continue;
                }
                float dx = values[0] - values[2];
                float dy = values[1] - values[3];
                if (std::fabs(dx) < 1e-5f && std::fabs(dy) < 1e-5f) {
                    continue;
                }
                target[roi][eye] = values;
            }
        }
        return true;
    };

    bool feret_major_ok =
        loadFeretAxes("feret_axes_major", data_.eye_mask_feret_axes_major);
    bool feret_minor_ok =
        loadFeretAxes("feret_axes_minor", data_.eye_mask_feret_axes_minor);
    data_.eye_masks_have_feret_axes = feret_major_ok && feret_minor_ok;

    std::cout << "  Refined eye mask run '" << latest_run
              << "' loaded (" << channel_dim << " channels, "
              << mask_cols << "x" << mask_rows << " masks)" << std::endl;
    return true;
}

bool ZarrDetectionLoader::loadEyeAngleData(const ts::kvstore::KvStore& store,
                                           size_t roi_count) {
    data_.eye_angle_run_name.clear();
    data_.eye_angle_frame_indices.clear();
    data_.eye_angle_valid_mask.clear();
    data_.eye_angle_left_deg.clear();
    data_.eye_angle_right_deg.clear();
    data_.eye_angle_indices_by_frame.clear();
    data_.has_eye_angles = false;
    data_.has_eye_frame_angles = false;
    data_.eye_frame_left_angle_deg.clear();
    data_.eye_frame_right_angle_deg.clear();
    data_.eye_frame_vergence_deg.clear();

    {
        data_.eye_vergence_signed_frame_deg.clear();
        data_.eye_vergence_frame_time_seconds.clear();
        data_.eye_vergence_frame_valid.clear();
        data_.has_eye_vergence_frame = false;
        data_.eye_angle_analysis =
            ZarrDetectionData::EyeAngleAnalysisData{};

        std::string selected_run = requested_eye_angle_run_name_;
        if (selected_run.empty()) {
            if (auto group_attrs = readAttrsAny(store, "analysis/eye_angle_runs")) {
                selected_run = extractLatestRunName(*group_attrs);
            }
        }
        if (selected_run.empty() && !root_path_.empty()) {
            auto candidates = collect_runs_fs(
                root_path_,
                "analysis/eye_angle_runs",
                {"angles/roi/left_eye_angle_deg"});
            if (candidates.empty()) {
                candidates = collect_runs_fs(
                    root_path_,
                    "analysis/eye_angle_runs",
                    {"angles/roi/left_gaze_signed_deg"});
            }
            if (candidates.empty()) {
                candidates = collect_runs_fs(
                    root_path_,
                    "analysis/eye_angle_runs",
                    {"angles/roi/left_minor_signed_deg"});
            }
            if (candidates.empty()) {
                candidates = collect_runs_fs(
                    root_path_,
                    "analysis/eye_angle_runs",
                    {"roi_angles", "angle_channel_index/name"});
            }
            if (!candidates.empty()) {
                selected_run = candidates.back();
            }
        }
        if (selected_run.empty()) {
            return false;
        }

        const std::string run_base =
            "analysis/eye_angle_runs/" + selected_run + "/";
        auto run_attrs = readAttrsAny(store, run_base);
        if (!run_attrs.has_value()) {
            if (!requested_eye_angle_run_name_.empty()) {
                std::cout
                    << "  [EYE_ANGLE_WARNING] Requested eye-angle run '"
                    << requested_eye_angle_run_name_ << "' was not found at "
                    << run_base << std::endl;
            }
            return false;
        }

        auto& eye = data_.eye_angle_analysis;
        eye.run_name = selected_run;
        eye.schema_id = jsonStringAttr(*run_attrs, "schema_id");
        eye.schema_version = jsonIntAttr(*run_attrs, "schema_version");
        eye.method = jsonStringAttr(*run_attrs, "method");
        eye.method_version = jsonStringAttr(*run_attrs, "method_version");
        eye.source_geometry_kind =
            jsonStringAttr(*run_attrs, "source_geometry_kind");
        eye.source_eye_geometry_run =
            jsonStringAttr(*run_attrs, "source_eye_geometry_run");
        eye.source_subject_shape_run =
            jsonStringAttr(*run_attrs, "source_subject_shape_run");
        eye.source_refined_subject_masks_run =
            jsonStringAttr(*run_attrs, "source_refined_subject_masks_run");
        eye.source_keypoints_run =
            jsonStringAttr(*run_attrs, "source_keypoints_run");
        if (eye.source_keypoints_run.empty()) {
            eye.source_keypoints_run =
                jsonStringAttr(*run_attrs, "source_keypoint_run");
        }
        if (!eye.schema_id.empty() &&
            eye.schema_id != "analysis.eye_angle_runs") {
            appendEyeAngleWarning(
                eye,
                "Unexpected eye-angle schema_id '" + eye.schema_id +
                    "'; attempting partial load.");
        }
        if (eye.schema_version >= 0 && eye.schema_version < 5) {
            appendEyeAngleWarning(
                eye,
                "Eye-angle run schema_version is older than 5; using compatibility load.");
        }

        if (run_attrs->contains("reason_code_map") &&
            (*run_attrs)["reason_code_map"].is_object()) {
            for (auto it = (*run_attrs)["reason_code_map"].begin();
                 it != (*run_attrs)["reason_code_map"].end(); ++it) {
                try {
                    const int32_t code = std::stoi(it.key());
                    if (it.value().is_string()) {
                        eye.reason_code_map[code] =
                            it.value().get<std::string>();
                    }
                } catch (const std::exception&) {
                    continue;
                }
            }
        }

        nlohmann::json variant_schema;
        if (run_attrs->contains("eye_angle_variant_schema") &&
            (*run_attrs)["eye_angle_variant_schema"].is_object()) {
            variant_schema = (*run_attrs)["eye_angle_variant_schema"];
        }
        if (run_attrs->contains("eye_angle_output_schema") &&
            (*run_attrs)["eye_angle_output_schema"].is_object()) {
            const auto& output_schema = (*run_attrs)["eye_angle_output_schema"];
            eye.output_schema_id = jsonStringAttr(output_schema, "schema_id");
            eye.output_schema_version =
                jsonIntAttr(output_schema, "schema_version");
            if (variant_schema.is_null() &&
                output_schema.contains("variant_schema") &&
                output_schema["variant_schema"].is_object()) {
                variant_schema = output_schema["variant_schema"];
            }
        }
        if (eye.output_schema_version >= 0 && eye.output_schema_version < 7) {
            appendEyeAngleWarning(
                eye,
                "Eye-angle output schema is older than v7; using compatibility UI metadata.");
        }

        if (variant_schema.is_object()) {
            eye.variant_schema_id =
                jsonStringAttr(variant_schema, "schema_id");
            eye.variant_schema_version =
                jsonIntAttr(variant_schema, "schema_version");
            eye.default_representation =
                jsonStringAttr(variant_schema, "default_representation");
            eye.representation_order =
                jsonStringVector(variant_schema, "representation_order");
            if (variant_schema.contains("representations") &&
                variant_schema["representations"].is_object()) {
                const auto& reps = variant_schema["representations"];
                for (const auto& key : eye.representation_order) {
                    if (!reps.contains(key) || !reps[key].is_object()) {
                        continue;
                    }
                    const auto& rep_json = reps[key];
                    ZarrDetectionData::EyeAngleRepresentationInfo rep;
                    rep.key = key;
                    rep.display_name =
                        jsonStringAttr(rep_json, "display_name");
                    rep.role = jsonStringAttr(rep_json, "role");
                    rep.axis = jsonStringAttr(rep_json, "axis");
                    rep.coordinate_frame =
                        jsonStringAttr(rep_json, "coordinate_frame");
                    rep.units = jsonStringAttr(rep_json, "units");
                    rep.sign_convention =
                        jsonStringAttr(rep_json, "sign_convention");
                    rep.derived_from =
                        jsonStringAttr(rep_json, "derived_from");
                    rep.default_plot_fields =
                        jsonStringVector(rep_json, "default_plot_fields");
                    rep.primary_roi_fields =
                        jsonStringVector(rep_json, "primary_roi_fields");
                    rep.aggregate_roi_fields =
                        jsonStringVector(rep_json, "aggregate_roi_fields");
                    rep.vector_roi_fields =
                        jsonStringVector(rep_json, "vector_roi_fields");
                    rep.frame_fields =
                        jsonStringVector(rep_json, "frame_fields");
                    eye.representations.push_back(std::move(rep));
                }
            }
            if (variant_schema.contains("fields") &&
                variant_schema["fields"].is_object()) {
                for (auto it = variant_schema["fields"].begin();
                     it != variant_schema["fields"].end(); ++it) {
                    if (!it.value().is_object()) {
                        continue;
                    }
                    const auto& field_json = it.value();
                    eye.fields.push_back(makeEyeAngleFieldInfo(
                        it.key(),
                        jsonStringAttr(field_json, "representation"),
                        jsonStringAttr(field_json, "field_role"),
                        jsonStringAttr(field_json, "display_name"),
                        jsonStringAttr(field_json, "units"),
                        field_json.contains("default_plot") &&
                            field_json["default_plot"].is_boolean() &&
                            field_json["default_plot"].get<bool>()));
                }
            }
        }
        if (eye.representations.empty()) {
            appendCompatibilityEyeAngleSchema(eye);
            appendEyeAngleWarning(
                eye,
                "eye_angle_variant_schema missing or incomplete; representation metadata is inferred.");
        }
        if (eye.default_representation.empty()) {
            eye.default_representation =
                eye.representation_order.empty()
                    ? "eye_frame"
                    : eye.representation_order.front();
        }

        std::vector<std::string> scalar_fields_to_load;
        std::vector<std::string> vector_fields_to_load;
        for (const auto& rep : eye.representations) {
            for (const auto& field : rep.default_plot_fields) {
                addUniqueString(scalar_fields_to_load, field);
                addUniqueString(scalar_fields_to_load,
                                unsmoothedBaseField(field));
            }
            for (const auto& field : rep.primary_roi_fields) {
                addUniqueString(scalar_fields_to_load, field);
            }
            for (const auto& field : rep.aggregate_roi_fields) {
                addUniqueString(scalar_fields_to_load, field);
            }
            for (const auto& field : rep.frame_fields) {
                addUniqueString(scalar_fields_to_load, field);
                addUniqueString(scalar_fields_to_load, field + "_smoothed");
            }
            for (const auto& field : rep.vector_roi_fields) {
                addUniqueString(vector_fields_to_load, field);
            }
        }
        addUniqueString(scalar_fields_to_load, "left_eye_angle_deg");
        addUniqueString(scalar_fields_to_load, "right_eye_angle_deg");
        addUniqueString(scalar_fields_to_load, "vergence_eye_angle_deg");
        addUniqueString(scalar_fields_to_load, "left_eye_angle_deg_smoothed");
        addUniqueString(scalar_fields_to_load, "right_eye_angle_deg_smoothed");
        addUniqueString(scalar_fields_to_load, "left_gaze_signed_deg");
        addUniqueString(scalar_fields_to_load, "right_gaze_signed_deg");
        addUniqueString(scalar_fields_to_load, "left_gaze_deg");
        addUniqueString(scalar_fields_to_load, "right_gaze_deg");
        addUniqueString(scalar_fields_to_load, "vergence_gaze_deg");
        addUniqueString(scalar_fields_to_load, "left_minor_signed_deg");
        addUniqueString(scalar_fields_to_load, "right_minor_signed_deg");
        addUniqueString(scalar_fields_to_load, "vergence_eye_angle_deg_smoothed");
        addUniqueString(scalar_fields_to_load, "vergence_eye_angle_deg");
        addUniqueString(scalar_fields_to_load, "vergence_signed_deg_smoothed");
        addUniqueString(scalar_fields_to_load, "vergence_signed_deg");
        addUniqueString(vector_fields_to_load, "left_gaze_xy");
        addUniqueString(vector_fields_to_load, "right_gaze_xy");

        auto field_info_for =
            [&](const std::string& name) -> ZarrDetectionData::EyeAngleFieldInfo {
            auto field_it = std::find_if(
                eye.fields.begin(),
                eye.fields.end(),
                [&](const auto& field) { return field.name == name; });
            if (field_it != eye.fields.end()) {
                return *field_it;
            }
            for (const auto& rep : eye.representations) {
                auto contains = [&](const std::vector<std::string>& values) {
                    return std::find(values.begin(), values.end(), name) !=
                           values.end();
                };
                if (contains(rep.default_plot_fields) ||
                    contains(rep.primary_roi_fields) ||
                    contains(rep.aggregate_roi_fields) ||
                    contains(rep.frame_fields) ||
                    contains(rep.vector_roi_fields)) {
                    return makeEyeAngleFieldInfo(
                        name,
                        rep.key,
                        contains(rep.vector_roi_fields) ? "vector_roi" : "",
                        name,
                        rep.units,
                        contains(rep.default_plot_fields));
                }
            }
            return makeEyeAngleFieldInfo(name, {}, {}, name, "deg", false);
        };

        auto readVec2Array =
            [&](const std::string& rel_path,
                std::vector<std::array<float, 2>>& out) -> bool {
            const std::string path = run_base + rel_path;
            auto try_read = [&](auto type_token) -> bool {
                using Source = decltype(type_token);
                auto open_result =
                    openArrayAny<Source, 2>(store, path, context_);
                if (!open_result.ok()) {
                    return false;
                }
                auto array_result = ts::Read(open_result.value()).result();
                if (!array_result.ok()) {
                    return false;
                }
                auto array = array_result.value();
                if (array.rank() != 2 || array.shape()[1] < 2) {
                    return false;
                }
                const size_t rows = static_cast<size_t>(array.shape()[0]);
                out.resize(rows);
                for (size_t row = 0; row < rows; ++row) {
                    out[row] = {
                        static_cast<float>(
                            array(static_cast<ts::Index>(row), 0)),
                        static_cast<float>(
                            array(static_cast<ts::Index>(row), 1))};
                }
                return true;
            };
            return try_read(float{}) || try_read(double{});
        };

        struct CompactFloatMatrix {
            size_t rows = 0;
            size_t cols = 0;
            std::vector<float> values;
            bool loaded = false;
        };
        struct CompactVectorTensor {
            size_t rows = 0;
            size_t channels = 0;
            std::vector<std::array<float, 2>> values;
            bool loaded = false;
        };
        struct CompactQaMatrix {
            size_t rows = 0;
            size_t cols = 0;
            std::vector<int32_t> values;
            bool loaded = false;
        };

        const bool compact_dense_layout =
            jsonStringAttr(*run_attrs, "layout") == "compact_dense_v2" ||
            readNodeMetaV3(store, run_base + "roi_angles").has_value();

        auto readCompactFloatMatrix =
            [&](const std::string& rel_path,
                CompactFloatMatrix& out) -> bool {
            const std::string path = run_base + rel_path;
            auto try_read = [&](auto type_token) -> bool {
                using Source = decltype(type_token);
                auto open_result =
                    openArrayAny<Source, 2>(store, path, context_);
                if (!open_result.ok()) {
                    return false;
                }
                auto array_result = ts::Read(open_result.value()).result();
                if (!array_result.ok()) {
                    return false;
                }
                auto array = array_result.value();
                if (array.rank() != 2) {
                    return false;
                }
                out.rows = static_cast<size_t>(array.shape()[0]);
                out.cols = static_cast<size_t>(array.shape()[1]);
                out.values.resize(out.rows * out.cols);
                const auto* data = static_cast<const Source*>(array.data());
                for (size_t i = 0; i < out.values.size(); ++i) {
                    out.values[i] = static_cast<float>(data[i]);
                }
                out.loaded = true;
                return true;
            };
            return try_read(float{}) || try_read(double{});
        };

        auto readCompactVectorTensor =
            [&](const std::string& rel_path,
                CompactVectorTensor& out) -> bool {
            const std::string path = run_base + rel_path;
            auto try_read = [&](auto type_token) -> bool {
                using Source = decltype(type_token);
                auto open_result =
                    openArrayAny<Source, 3>(store, path, context_);
                if (!open_result.ok()) {
                    return false;
                }
                auto array_result = ts::Read(open_result.value()).result();
                if (!array_result.ok()) {
                    return false;
                }
                auto array = array_result.value();
                if (array.rank() != 3 || array.shape()[2] < 2) {
                    return false;
                }
                out.rows = static_cast<size_t>(array.shape()[0]);
                out.channels = static_cast<size_t>(array.shape()[1]);
                out.values.resize(out.rows * out.channels);
                const auto* data = static_cast<const Source*>(array.data());
                const size_t stride_channels = out.channels * 2;
                for (size_t row = 0; row < out.rows; ++row) {
                    for (size_t channel = 0; channel < out.channels; ++channel) {
                        const size_t src = row * stride_channels + channel * 2;
                        out.values[row * out.channels + channel] = {
                            static_cast<float>(data[src + 0]),
                            static_cast<float>(data[src + 1])};
                    }
                }
                out.loaded = true;
                return true;
            };
            return try_read(float{}) || try_read(double{});
        };

        auto readCompactQaMatrix =
            [&](const std::string& rel_path,
                CompactQaMatrix& out) -> bool {
            const std::string path = run_base + rel_path;
            auto try_read = [&](auto type_token) -> bool {
                using Source = decltype(type_token);
                auto open_result =
                    openArrayAny<Source, 2>(store, path, context_);
                if (!open_result.ok()) {
                    return false;
                }
                auto array_result = ts::Read(open_result.value()).result();
                if (!array_result.ok()) {
                    return false;
                }
                auto array = array_result.value();
                if (array.rank() != 2) {
                    return false;
                }
                out.rows = static_cast<size_t>(array.shape()[0]);
                out.cols = static_cast<size_t>(array.shape()[1]);
                out.values.resize(out.rows * out.cols);
                for (size_t row = 0; row < out.rows; ++row) {
                    for (size_t col = 0; col < out.cols; ++col) {
                        out.values[row * out.cols + col] =
                            static_cast<int32_t>(array(
                                static_cast<ts::Index>(row),
                                static_cast<ts::Index>(col)));
                    }
                }
                out.loaded = true;
                return true;
            };
            return try_read(bool{}) ||
                   try_read(uint16_t{}) ||
                   try_read(uint8_t{}) ||
                   try_read(int32_t{}) ||
                   try_read(int16_t{});
        };

        auto compactChannelMap =
            [&](const std::string& index_base,
                const std::string& availability_name)
                -> std::unordered_map<std::string, size_t> {
            std::vector<std::string> names;
            std::vector<uint8_t> available;
            std::unordered_map<std::string, size_t> channels;
            if (!readStringArray(store, run_base + index_base + "/name",
                                 names)) {
                return channels;
            }
            readBoolArray(store,
                          run_base + index_base + "/" + availability_name,
                          available);
            for (size_t i = 0; i < names.size(); ++i) {
                if (names[i].empty()) {
                    continue;
                }
                const bool is_available =
                    available.empty() ||
                    (i < available.size() && available[i] != 0);
                if (is_available) {
                    channels[names[i]] = i;
                }
            }
            return channels;
        };

        if (compact_dense_layout) {
            const auto angle_roi_channels =
                compactChannelMap("angle_channel_index", "roi_available");
            const auto angle_frame_channels =
                compactChannelMap("angle_channel_index", "frame_available");
            const auto vector_roi_channels =
                compactChannelMap("vector_channel_index", "roi_available");

            CompactFloatMatrix roi_angles;
            CompactFloatMatrix frame_angles;
            CompactVectorTensor roi_vectors;
            if (!angle_roi_channels.empty()) {
                readCompactFloatMatrix("roi_angles", roi_angles);
            }
            if (!angle_frame_channels.empty()) {
                readCompactFloatMatrix("frame_angles", frame_angles);
            }
            if (!vector_roi_channels.empty()) {
                readCompactVectorTensor("roi_vectors", roi_vectors);
            }

            auto copyCompactScalarColumn =
                [](const CompactFloatMatrix& matrix,
                   size_t channel,
                   std::vector<float>& out) -> bool {
                if (!matrix.loaded || channel >= matrix.cols) {
                    return false;
                }
                out.resize(matrix.rows);
                for (size_t row = 0; row < matrix.rows; ++row) {
                    out[row] = matrix.values[row * matrix.cols + channel];
                }
                return true;
            };

            for (const auto& field_name : scalar_fields_to_load) {
                ZarrDetectionData::EyeAngleScalarField field;
                const auto info = field_info_for(field_name);
                field.name = field_name;
                field.representation_key = info.representation_key;
                field.field_role = info.field_role;
                field.display_name =
                    info.display_name.empty() ? field_name : info.display_name;
                field.units = info.units.empty() ? "deg" : info.units;
                if (auto it = angle_roi_channels.find(field_name);
                    it != angle_roi_channels.end()) {
                    field.has_roi = copyCompactScalarColumn(
                        roi_angles, it->second, field.roi_values);
                }
                if (auto it = angle_frame_channels.find(field_name);
                    it != angle_frame_channels.end()) {
                    field.has_frame = copyCompactScalarColumn(
                        frame_angles, it->second, field.frame_values);
                }
                if (field.has_roi || field.has_frame) {
                    if (field.has_roi) {
                        eye.row_count =
                            std::max(eye.row_count, field.roi_values.size());
                    }
                    if (field.has_frame) {
                        eye.frame_count =
                            std::max(eye.frame_count, field.frame_values.size());
                    }
                    eye.scalar_fields.push_back(std::move(field));
                }
            }

            for (const auto& field_name : vector_fields_to_load) {
                ZarrDetectionData::EyeAngleVectorField field;
                const auto info = field_info_for(field_name);
                field.name = field_name;
                field.representation_key = info.representation_key;
                field.field_role = info.field_role;
                field.display_name =
                    info.display_name.empty() ? field_name : info.display_name;
                field.units = info.units;
                if (auto it = vector_roi_channels.find(field_name);
                    it != vector_roi_channels.end()) {
                    const size_t channel = it->second;
                    if (roi_vectors.loaded && channel < roi_vectors.channels) {
                        field.roi_values.resize(roi_vectors.rows);
                        for (size_t row = 0; row < roi_vectors.rows; ++row) {
                            field.roi_values[row] =
                                roi_vectors.values[row * roi_vectors.channels +
                                                   channel];
                        }
                        field.has_roi = true;
                    }
                }
                if (field.has_roi) {
                    eye.row_count =
                        std::max(eye.row_count, field.roi_values.size());
                    eye.vector_fields.push_back(std::move(field));
                }
            }
        } else {
            for (const auto& field_name : scalar_fields_to_load) {
                ZarrDetectionData::EyeAngleScalarField field;
                const auto info = field_info_for(field_name);
                field.name = field_name;
                field.representation_key = info.representation_key;
                field.field_role = info.field_role;
                field.display_name =
                    info.display_name.empty() ? field_name : info.display_name;
                field.units = info.units;
                field.has_roi = readFloatArray(
                    store, run_base + "angles/roi/" + field_name,
                    field.roi_values);
                field.has_frame = readFloatArray(
                    store, run_base + "angles/frame/" + field_name,
                    field.frame_values);
                if (field.has_roi || field.has_frame) {
                    if (field.has_roi) {
                        eye.row_count =
                            std::max(eye.row_count, field.roi_values.size());
                    }
                    if (field.has_frame) {
                        eye.frame_count =
                            std::max(eye.frame_count, field.frame_values.size());
                    }
                    eye.scalar_fields.push_back(std::move(field));
                }
            }
            for (const auto& field_name : vector_fields_to_load) {
                ZarrDetectionData::EyeAngleVectorField field;
                const auto info = field_info_for(field_name);
                field.name = field_name;
                field.representation_key = info.representation_key;
                field.field_role = info.field_role;
                field.display_name =
                    info.display_name.empty() ? field_name : info.display_name;
                field.units = info.units;
                field.has_roi =
                    readVec2Array("angles/roi/" + field_name, field.roi_values);
                if (field.has_roi) {
                    eye.row_count =
                        std::max(eye.row_count, field.roi_values.size());
                    eye.vector_fields.push_back(std::move(field));
                }
            }
        }

        readInt32Array(store, run_base + "support/frame_indices",
                       eye.roi_frame_indices);
        if (eye.roi_frame_indices.empty()) {
            readInt32Array(store, run_base + "angles/roi/frame_indices",
                           eye.roi_frame_indices);
        }
        readFloatArray(store, run_base + "support/time_seconds",
                       eye.roi_time_seconds);
        readFloatArray(store, run_base + "support/frame_time_seconds",
                       eye.frame_time_seconds);
        if (compact_dense_layout) {
            const auto qa_roi_channels =
                compactChannelMap("qa_channel_index", "roi_available");
            const auto qa_frame_channels =
                compactChannelMap("qa_channel_index", "frame_available");
            CompactQaMatrix roi_qa;
            CompactQaMatrix frame_qa;
            if (!qa_roi_channels.empty()) {
                readCompactQaMatrix("roi_qa", roi_qa);
            }
            if (!qa_frame_channels.empty()) {
                readCompactQaMatrix("frame_qa", frame_qa);
            }
            auto copyQaBoolColumn =
                [](const CompactQaMatrix& matrix,
                   size_t channel,
                   std::vector<uint8_t>& out) -> bool {
                if (!matrix.loaded || channel >= matrix.cols) {
                    return false;
                }
                out.resize(matrix.rows);
                for (size_t row = 0; row < matrix.rows; ++row) {
                    out[row] =
                        matrix.values[row * matrix.cols + channel] != 0 ? 1 : 0;
                }
                return true;
            };
            auto copyQaIntColumn =
                [](const CompactQaMatrix& matrix,
                   size_t channel,
                   std::vector<int32_t>& out) -> bool {
                if (!matrix.loaded || channel >= matrix.cols) {
                    return false;
                }
                out.resize(matrix.rows);
                for (size_t row = 0; row < matrix.rows; ++row) {
                    out[row] = matrix.values[row * matrix.cols + channel];
                }
                return true;
            };
            auto copyRoiBool = [&](const std::string& name,
                                   std::vector<uint8_t>& out) {
                if (auto it = qa_roi_channels.find(name);
                    it != qa_roi_channels.end()) {
                    copyQaBoolColumn(roi_qa, it->second, out);
                }
            };
            auto copyFrameBool = [&](const std::string& name,
                                     std::vector<uint8_t>& out) {
                if (auto it = qa_frame_channels.find(name);
                    it != qa_frame_channels.end()) {
                    copyQaBoolColumn(frame_qa, it->second, out);
                }
            };
            copyRoiBool("valid_left", eye.roi_valid_left);
            copyRoiBool("valid_right", eye.roi_valid_right);
            copyRoiBool("valid_frame", eye.roi_valid_frame);
            copyFrameBool("valid_frame", eye.frame_valid_frame);
            copyRoiBool("left_major_axis_marginal",
                        eye.roi_left_major_axis_marginal);
            copyRoiBool("right_major_axis_marginal",
                        eye.roi_right_major_axis_marginal);
            copyRoiBool("major_axis_marginal",
                        eye.roi_major_axis_marginal);
            copyFrameBool("major_axis_marginal",
                          eye.frame_major_axis_marginal);
            if (auto it = qa_roi_channels.find("reason_codes");
                it != qa_roi_channels.end()) {
                copyQaIntColumn(roi_qa, it->second, eye.roi_reason_codes);
            }
            if (auto it = qa_frame_channels.find("reason_codes");
                it != qa_frame_channels.end()) {
                copyQaIntColumn(frame_qa, it->second, eye.frame_reason_codes);
            }
        } else {
            readBoolArray(store, run_base + "qa/roi/valid_left",
                          eye.roi_valid_left);
            readBoolArray(store, run_base + "qa/roi/valid_right",
                          eye.roi_valid_right);
            readBoolArray(store, run_base + "qa/roi/valid_frame",
                          eye.roi_valid_frame);
            readBoolArray(store, run_base + "qa/frame/valid_frame",
                          eye.frame_valid_frame);
            readBoolArray(store, run_base + "qa/roi/left_major_axis_marginal",
                          eye.roi_left_major_axis_marginal);
            readBoolArray(store, run_base + "qa/roi/right_major_axis_marginal",
                          eye.roi_right_major_axis_marginal);
            readBoolArray(store, run_base + "qa/roi/major_axis_marginal",
                          eye.roi_major_axis_marginal);
            readBoolArray(store, run_base + "qa/frame/major_axis_marginal",
                          eye.frame_major_axis_marginal);
            readInt32Array(store, run_base + "qa/roi/reason_codes",
                           eye.roi_reason_codes);
            readInt32Array(store, run_base + "qa/frame/reason_codes",
                           eye.frame_reason_codes);
        }

        eye.row_count = std::max(
            {eye.row_count,
             eye.roi_frame_indices.size(),
             eye.roi_time_seconds.size(),
             eye.roi_valid_left.size(),
             eye.roi_valid_right.size(),
             eye.roi_valid_frame.size(),
             eye.roi_reason_codes.size()});
        eye.frame_count = std::max(
            {eye.frame_count,
             eye.frame_time_seconds.size(),
             eye.frame_valid_frame.size(),
             eye.frame_reason_codes.size()});
        if (eye.row_count == 0 && eye.frame_count == 0) {
            return false;
        }
        eye.roi_reason_labels.resize(eye.roi_reason_codes.size());
        for (size_t i = 0; i < eye.roi_reason_codes.size(); ++i) {
            eye.roi_reason_labels[i] =
                decodeEyeAngleReasonCode(eye.roi_reason_codes[i],
                                         eye.reason_code_map);
        }
        eye.frame_reason_labels.resize(eye.frame_reason_codes.size());
        for (size_t i = 0; i < eye.frame_reason_codes.size(); ++i) {
            eye.frame_reason_labels[i] =
                decodeEyeAngleReasonCode(eye.frame_reason_codes[i],
                                         eye.reason_code_map);
        }
        if (!eye.roi_frame_indices.empty() &&
            eye.roi_frame_indices.size() == eye.row_count) {
            eye.row_to_frame = eye.roi_frame_indices;
        } else {
            eye.row_to_frame.assign(eye.row_count, -1);
            appendEyeAngleWarning(
                eye,
                "Eye-angle support/frame_indices missing or length-mismatched; QC seeking may be unavailable.");
        }

        auto scalar_roi_values =
            [&](const std::vector<std::string>& candidates)
                -> const std::vector<float>* {
            for (const auto& name : candidates) {
                auto it = std::find_if(
                    eye.scalar_fields.begin(),
                    eye.scalar_fields.end(),
                    [&](const auto& field) { return field.name == name; });
                if (it != eye.scalar_fields.end() && it->has_roi &&
                    !it->roi_values.empty()) {
                    return &it->roi_values;
                }
            }
            return nullptr;
        };
        const auto* left_eye_frame_source = scalar_roi_values(
            {"left_eye_angle_deg", "left_eye_angle_deg_smoothed"});
        const auto* right_eye_frame_source = scalar_roi_values(
            {"right_eye_angle_deg", "right_eye_angle_deg_smoothed"});
        const auto* vergence_eye_frame_source = scalar_roi_values(
            {"vergence_eye_angle_deg", "vergence_eye_angle_deg_smoothed"});
        if (left_eye_frame_source != nullptr &&
            right_eye_frame_source != nullptr) {
            size_t count =
                std::min(left_eye_frame_source->size(),
                         right_eye_frame_source->size());
            if (eye.row_count > 0) {
                count = std::min(count, eye.row_count);
            }
            data_.eye_frame_left_angle_deg.assign(
                left_eye_frame_source->begin(),
                left_eye_frame_source->begin() + count);
            data_.eye_frame_right_angle_deg.assign(
                right_eye_frame_source->begin(),
                right_eye_frame_source->begin() + count);
            if (vergence_eye_frame_source != nullptr) {
                size_t vergence_count =
                    std::min(vergence_eye_frame_source->size(), count);
                data_.eye_frame_vergence_deg.assign(
                    vergence_eye_frame_source->begin(),
                    vergence_eye_frame_source->begin() + vergence_count);
            } else {
                data_.eye_frame_vergence_deg.resize(count);
                for (size_t i = 0; i < count; ++i) {
                    data_.eye_frame_vergence_deg[i] =
                        data_.eye_frame_left_angle_deg[i] +
                        data_.eye_frame_right_angle_deg[i];
                }
                appendEyeAngleWarning(
                    eye,
                    "ROI vergence_eye_angle_deg missing; camera overlay computes eye-frame vergence from left+right eye-frame angles.");
            }
            data_.has_eye_frame_angles = true;
        } else {
            appendEyeAngleWarning(
                eye,
                "No ROI left_eye_angle_deg/right_eye_angle_deg fields were available for default eye-frame camera labels.");
        }

        const auto* left_angle_source = scalar_roi_values(
            {"left_gaze_signed_deg", "left_minor_signed_deg",
             "left_feret_minor_signed_deg"});
        const auto* right_angle_source = scalar_roi_values(
            {"right_gaze_signed_deg", "right_minor_signed_deg",
             "right_feret_minor_signed_deg"});
        if (left_angle_source != nullptr && right_angle_source != nullptr) {
            size_t count =
                std::min(left_angle_source->size(),
                         right_angle_source->size());
            if (eye.row_count > 0) {
                count = std::min(count, eye.row_count);
            }
            data_.eye_angle_left_deg.assign(
                left_angle_source->begin(),
                left_angle_source->begin() + count);
            data_.eye_angle_right_deg.assign(
                right_angle_source->begin(),
                right_angle_source->begin() + count);
        } else {
            appendEyeAngleWarning(
                eye,
                "No ROI gaze/minor signed fields were available for eye-angle arc overlays.");
        }
        const size_t angle_index_count = std::max(
            {data_.eye_frame_left_angle_deg.size(),
             data_.eye_frame_right_angle_deg.size(),
             data_.eye_frame_vergence_deg.size(),
             data_.eye_angle_left_deg.size(),
             data_.eye_angle_right_deg.size()});
        if (angle_index_count > 0) {
            if (!eye.row_to_frame.empty()) {
                data_.eye_angle_frame_indices.assign(
                    eye.row_to_frame.begin(),
                    eye.row_to_frame.begin() +
                        std::min(angle_index_count, eye.row_to_frame.size()));
            }
            data_.eye_angle_frame_indices.resize(angle_index_count, -1);
            data_.eye_angle_valid_mask.assign(angle_index_count, 1);
            for (size_t i = 0; i < angle_index_count; ++i) {
                const bool frame_valid =
                    eye.roi_valid_frame.empty() ||
                    (i < eye.roi_valid_frame.size() &&
                     eye.roi_valid_frame[i] != 0);
                data_.eye_angle_valid_mask[i] = frame_valid ? 1 : 0;
            }
        }
        data_.has_eye_angles =
            data_.has_eye_frame_angles ||
            !data_.eye_angle_left_deg.empty() ||
            !data_.eye_angle_right_deg.empty();
        data_.eye_angle_run_name = selected_run;

        size_t max_frame_index = 0;
        for (auto frame : data_.eye_angle_frame_indices) {
            if (frame >= 0) {
                max_frame_index =
                    std::max(max_frame_index, static_cast<size_t>(frame));
            }
        }
        const size_t desired_size =
            std::max({max_frame_index + 1, data_.total_frames, roi_count});
        data_.eye_angle_indices_by_frame.assign(desired_size, {});
        for (size_t i = 0; i < data_.eye_angle_frame_indices.size(); ++i) {
            const int32_t frame = data_.eye_angle_frame_indices[i];
            if (frame < 0) {
                continue;
            }
            const size_t frame_index = static_cast<size_t>(frame);
            if (frame_index >= data_.eye_angle_indices_by_frame.size()) {
                data_.eye_angle_indices_by_frame.resize(frame_index + 1);
            }
            data_.eye_angle_indices_by_frame[frame_index].push_back(i);
        }

        auto scalar_field_by_name =
            [&](const std::vector<std::string>& candidates)
                -> const ZarrDetectionData::EyeAngleScalarField* {
            for (const auto& name : candidates) {
                auto it = std::find_if(
                    eye.scalar_fields.begin(),
                    eye.scalar_fields.end(),
                    [&](const auto& field) { return field.name == name; });
                if (it != eye.scalar_fields.end()) {
                    return &(*it);
                }
            }
            return nullptr;
        };
        const auto* vergence_field = scalar_field_by_name(
            {"vergence_eye_angle_deg_smoothed", "vergence_eye_angle_deg",
             "vergence_signed_deg_smoothed", "vergence_signed_deg"});
        if (vergence_field != nullptr) {
            if (vergence_field->has_frame &&
                !vergence_field->frame_values.empty()) {
                data_.eye_vergence_signed_frame_deg =
                    vergence_field->frame_values;
            } else if (vergence_field->has_roi &&
                       !vergence_field->roi_values.empty()) {
                data_.eye_vergence_signed_frame_deg =
                    vergence_field->roi_values;
            }
            if (!data_.eye_vergence_signed_frame_deg.empty()) {
                const size_t frame_count =
                    data_.eye_vergence_signed_frame_deg.size();
                if (vergence_field->has_frame &&
                    eye.frame_time_seconds.size() == frame_count) {
                    data_.eye_vergence_frame_time_seconds =
                        eye.frame_time_seconds;
                } else if (eye.roi_time_seconds.size() == frame_count) {
                    data_.eye_vergence_frame_time_seconds =
                        eye.roi_time_seconds;
                } else {
                    data_.eye_vergence_frame_time_seconds.resize(frame_count);
                    const double fps = data_.fps > 0.0 ? data_.fps : 30.0;
                    const double inv_fps =
                        fps > 0.0 ? (1.0 / fps) : 0.033333333;
                    for (size_t i = 0; i < frame_count; ++i) {
                        data_.eye_vergence_frame_time_seconds[i] =
                            static_cast<float>(
                                static_cast<double>(i) * inv_fps);
                    }
                }
                if (vergence_field->has_frame &&
                    eye.frame_valid_frame.size() == frame_count) {
                    data_.eye_vergence_frame_valid = eye.frame_valid_frame;
                } else if (eye.roi_valid_frame.size() == frame_count) {
                    data_.eye_vergence_frame_valid = eye.roi_valid_frame;
                } else {
                    data_.eye_vergence_frame_valid.assign(frame_count, 1);
                }
                data_.has_eye_vergence_frame = true;
            }
        }

        eye.loaded = true;
        std::cout << "  Eye angle run '" << selected_run << "' loaded ("
                  << eye.row_count << " ROI rows, " << eye.frame_count
                  << " frame rows, default representation '"
                  << eye.default_representation << "'";
        if (compact_dense_layout) {
            std::cout << ", layout compact_dense_v2";
        }
        std::cout << ")" << std::endl;
        if (roi_count > 0 && eye.row_count != 0 && eye.row_count != roi_count) {
            std::cout << "    [EYE_ANGLE_WARNING] ROI count mismatch: angles="
                      << eye.row_count << ", expected " << roi_count
                      << std::endl;
        }
        if (!eye.warning.empty()) {
            std::cout << "  [EYE_ANGLE_WARNING] " << eye.warning
                      << std::endl;
        }
        if (compact_dense_layout) {
            auto scalar_axes = [&](const std::string& name) {
                auto it = std::find_if(
                    eye.scalar_fields.begin(),
                    eye.scalar_fields.end(),
                    [&](const auto& field) { return field.name == name; });
                std::string axes;
                if (it != eye.scalar_fields.end() && it->has_roi) {
                    axes += "roi";
                }
                if (it != eye.scalar_fields.end() && it->has_frame) {
                    if (!axes.empty()) {
                        axes += "+";
                    }
                    axes += "frame";
                }
                return axes.empty() ? std::string("missing") : axes;
            };
            auto vector_axes = [&](const std::string& name) {
                auto it = std::find_if(
                    eye.vector_fields.begin(),
                    eye.vector_fields.end(),
                    [&](const auto& field) { return field.name == name; });
                return (it != eye.vector_fields.end() && it->has_roi)
                           ? std::string("roi")
                           : std::string("missing");
            };
            std::string valid_frame_axes;
            if (!eye.roi_valid_frame.empty()) {
                valid_frame_axes += "roi";
            }
            if (!eye.frame_valid_frame.empty()) {
                if (!valid_frame_axes.empty()) {
                    valid_frame_axes += "+";
                }
                valid_frame_axes += "frame";
            }
            if (valid_frame_axes.empty()) {
                valid_frame_axes = "missing";
            }
            std::cout
                << "  [EyeAngleCompact] overlay fields:"
                << " left_gaze_xy=" << vector_axes("left_gaze_xy")
                << " right_gaze_xy=" << vector_axes("right_gaze_xy")
                << " left_eye_angle_deg=" << scalar_axes("left_eye_angle_deg")
                << " right_eye_angle_deg=" << scalar_axes("right_eye_angle_deg")
                << " vergence_eye_angle_deg="
                << scalar_axes("vergence_eye_angle_deg")
                << " left_gaze_deg=" << scalar_axes("left_gaze_deg")
                << " right_gaze_deg=" << scalar_axes("right_gaze_deg")
                << " vergence_gaze_deg=" << scalar_axes("vergence_gaze_deg")
                << " valid_frame=" << valid_frame_axes
                << std::endl;
        }
        return true;
    }
    return false;
}

const ZarrDetectionData::EyeAngleScalarField*
ZarrDetectionLoader::findEyeAngleScalarField(
    const std::string& field_name) const {
    const auto& fields = data_.eye_angle_analysis.scalar_fields;
    auto it = std::find_if(
        fields.begin(),
        fields.end(),
        [&](const auto& field) { return field.name == field_name; });
    if (it == fields.end()) {
        return nullptr;
    }
    return &(*it);
}

const ZarrDetectionData::EyeAngleVectorField*
ZarrDetectionLoader::findEyeAngleVectorField(
    const std::string& field_name) const {
    const auto& fields = data_.eye_angle_analysis.vector_fields;
    auto it = std::find_if(
        fields.begin(),
        fields.end(),
        [&](const auto& field) { return field.name == field_name; });
    if (it == fields.end()) {
        return nullptr;
    }
    return &(*it);
}

ZarrDetectionLoader::EyeAngleQcJumpResult
ZarrDetectionLoader::computeEyeAngleQcJump(
    const EyeAngleQcFilterOptions& filters,
    int current_frame_num,
    bool forward) const {
    EyeAngleQcJumpResult result;
    const auto& eye = data_.eye_angle_analysis;
    if (!eye.loaded || eye.row_count == 0) {
        result.status = "Eye-angle data unavailable.";
        return result;
    }

    const std::string reason_filter =
        toLowerCopy(filters.reason_substring);
    const bool filter_enabled =
        filters.invalid_rows ||
        filters.major_axis_marginal ||
        !reason_filter.empty();
    if (!filter_enabled) {
        result.status = "Enable at least one eye-angle QC filter.";
        return result;
    }

    auto boolAtRow = [](const std::vector<uint8_t>& values,
                        size_t row) -> bool {
        return row < values.size() && values[row] != 0;
    };
    auto rowInvalid = [&](size_t row) -> bool {
        const bool left_invalid =
            row < eye.roi_valid_left.size() && eye.roi_valid_left[row] == 0;
        const bool right_invalid =
            row < eye.roi_valid_right.size() && eye.roi_valid_right[row] == 0;
        const bool frame_invalid =
            row < eye.roi_valid_frame.size() && eye.roi_valid_frame[row] == 0;
        return left_invalid || right_invalid || frame_invalid;
    };
    auto rowMarginal = [&](size_t row) -> bool {
        return boolAtRow(eye.roi_major_axis_marginal, row) ||
               boolAtRow(eye.roi_left_major_axis_marginal, row) ||
               boolAtRow(eye.roi_right_major_axis_marginal, row);
    };
    auto rowHasReason = [&](size_t row) -> bool {
        if (reason_filter.empty() || row >= eye.roi_reason_labels.size()) {
            return false;
        }
        return toLowerCopy(eye.roi_reason_labels[row]).find(reason_filter) !=
               std::string::npos;
    };
    auto rowMatches = [&](size_t row) -> bool {
        bool matched = false;
        matched = matched || (filters.invalid_rows && rowInvalid(row));
        matched = matched ||
                  (filters.major_axis_marginal && rowMarginal(row));
        matched = matched || rowHasReason(row);
        return matched;
    };

    std::vector<std::pair<int, size_t>> frame_rows;
    frame_rows.reserve(eye.row_count);
    for (size_t row = 0; row < eye.row_count; ++row) {
        if (!rowMatches(row)) {
            continue;
        }
        result.match_count++;
        if (row < eye.row_to_frame.size() && eye.row_to_frame[row] >= 0) {
            frame_rows.emplace_back(eye.row_to_frame[row], row);
        }
    }
    std::sort(frame_rows.begin(), frame_rows.end());
    frame_rows.erase(std::unique(frame_rows.begin(), frame_rows.end()),
                     frame_rows.end());
    if (frame_rows.empty()) {
        if (result.match_count == 0) {
            result.status = "No eye-angle rows match QC filters.";
        } else {
            result.status =
                "Eye-angle rows matched, but no video frame mapping is available.";
        }
        return result;
    }

    auto select_forward = [&]() -> std::pair<int, size_t> {
        auto it = std::upper_bound(
            frame_rows.begin(),
            frame_rows.end(),
            std::make_pair(current_frame_num,
                           std::numeric_limits<size_t>::max()));
        if (it == frame_rows.end()) {
            it = frame_rows.begin();
        }
        return *it;
    };
    auto select_backward = [&]() -> std::pair<int, size_t> {
        auto it = std::lower_bound(
            frame_rows.begin(),
            frame_rows.end(),
            std::make_pair(current_frame_num, static_cast<size_t>(0)));
        if (it == frame_rows.begin()) {
            return frame_rows.back();
        }
        --it;
        return *it;
    };
    const auto selected = forward ? select_forward() : select_backward();
    result.target_frame = selected.first;
    result.target_row = selected.second;
    result.status = "Eye-angle QC rows: " +
                    std::to_string(result.match_count) +
                    " matching rows across " +
                    std::to_string(frame_rows.size()) + " frame/row entries.";
    return result;
}

std::optional<size_t> ZarrDetectionLoader::findEyeAngleRowForFrame(
    int frame) const {
    const auto& eye = data_.eye_angle_analysis;
    if (!eye.loaded || frame < 0) {
        return std::nullopt;
    }
    for (size_t row = 0; row < eye.row_to_frame.size(); ++row) {
        if (eye.row_to_frame[row] == frame) {
            return row;
        }
    }
    return std::nullopt;
}

std::optional<int32_t> ZarrDetectionLoader::getEyeAngleFrameForRow(
    size_t row) const {
    const auto& eye = data_.eye_angle_analysis;
    if (!eye.loaded || row >= eye.row_to_frame.size() ||
        eye.row_to_frame[row] < 0) {
        return std::nullopt;
    }
    return eye.row_to_frame[row];
}

const ZarrDetectionData::EyeMaskChunkCacheEntry*
ZarrDetectionLoader::findEyeMaskChunk(size_t chunk_id) const {
    for (auto& entry : data_.mask_chunk_cache) {
        if (entry.chunk_id == chunk_id) {
            return &entry;
        }
    }
    return nullptr;
}

bool ZarrDetectionLoader::ensureEyeMaskChunk(size_t chunk_id,
                                             bool allow_prefetch,
                                             bool force_reload) const {
    if (!data_.eye_masks_loaded || data_.eye_mask_roi_count == 0) {
        return false;
    }
    const bool chunk_perf_log = subjectMaskChunkPerfLogsEnabled();
    const auto ensure_start = std::chrono::steady_clock::now();
    const bool use_rle_masks = data_.refined_subject_mask_rle_masks_used;
    const bool use_bitpacked_masks =
        data_.refined_subject_mask_bitpacked_masks_used;
    const char* mask_storage_surface =
        use_rle_masks ? "mask_rle"
                      : (use_bitpacked_masks ? "mask_bitpacked"
                                             : "masks_roi");

    bool cache_hit = false;
    {
        std::lock_guard<std::mutex> cache_lock(*data_.mask_chunk_cache_mutex);
        auto& cache = data_.mask_chunk_cache;
        auto it = std::find_if(
            cache.begin(),
            cache.end(),
            [&](const ZarrDetectionData::EyeMaskChunkCacheEntry& entry) {
                return entry.chunk_id == chunk_id;
            });
        if (it != cache.end()) {
            if (std::next(it) != cache.end()) {
                ZarrDetectionData::EyeMaskChunkCacheEntry entry = std::move(*it);
                cache.erase(it);
                cache.push_back(std::move(entry));
            }
            cache_hit = !force_reload;
        }
    }
    if (cache_hit) {
        if (chunk_perf_log) {
            std::cout << "[SUBJECT_MASK_CHUNK_PERF] source="
                      << mask_storage_surface
                      << " chunk_id=" << chunk_id
                      << " cache_hit=yes"
                      << " force_reload=" << (force_reload ? "yes" : "no")
                      << " allow_prefetch="
                      << (allow_prefetch ? "yes" : "no")
                      << " thread=" << std::this_thread::get_id()
                      << " total_ms=" << elapsedMsSince(ensure_start)
                      << std::endl;
        }
        if (allow_prefetch) {
            prefetchAdjacentEyeMaskChunks(chunk_id);
        }
        return true;
    }

    size_t chunk_rows =
        data_.eye_mask_chunk_rows > 0 ? data_.eye_mask_chunk_rows : 512;
    size_t chunk_start = chunk_id * chunk_rows;
    if (chunk_start >= data_.eye_mask_roi_count) {
        return false;
    }
    size_t chunk_end =
        std::min(chunk_start + chunk_rows, data_.eye_mask_roi_count);

    size_t chunk_len = chunk_end - chunk_start;
    size_t channels = data_.refined_subject_mask_labels.size();
    size_t rows = data_.eye_mask_height;
    size_t cols = data_.eye_mask_width;
    std::vector<ZarrDetectionData::RefinedSubjectMaskComponentInfo>
        overlay_components;
    size_t optional_overlay_generation = 0;
    {
        std::lock_guard<std::mutex> cache_lock(*data_.mask_chunk_cache_mutex);
        overlay_components = data_.refined_subject_mask_overlay_components;
        optional_overlay_generation =
            refined_subject_mask_optional_overlay_publish_generation_;
    }
    double rle_row_scan_ms = 0.0;
    double rle_counts_read_ms = 0.0;
    double rle_counts_copy_ms = 0.0;
    double rle_decode_ms = 0.0;
    double component_copy_ms = 0.0;
    double dense_read_ms = 0.0;
    double dense_collect_ms = 0.0;
    double bitpacked_read_ms = 0.0;
    double bitpacked_unpack_ms = 0.0;
    double contour_row_scan_ms = 0.0;
    double contour_read_ms = 0.0;
    double contour_copy_ms = 0.0;
    size_t rle_components_available = 0;
    size_t rle_components_with_rows = 0;
    size_t rle_rows_present = 0;
    size_t rle_rows_decoded = 0;
    size_t rle_count_values_read = 0;
    size_t rle_foreground_pixels = 0;
    size_t bitpacked_foreground_pixels = 0;
    size_t contour_components_with_rows = 0;
    size_t contour_rows_loaded = 0;
    size_t contour_points_loaded = 0;
    size_t dense_nonzero_pixels = 0;

    ZarrDetectionData::EyeMaskChunkCacheEntry entry;
    entry.chunk_id = chunk_id;
    entry.chunk_start = chunk_start;
    entry.chunk_length = chunk_len;
    entry.pixel_indices.resize(chunk_len);
    const size_t component_count = overlay_components.size();
    if (component_count > 0) {
        entry.component_pixel_indices.resize(chunk_len);
        entry.component_contours_xy.resize(chunk_len);
        for (size_t roi = 0; roi < chunk_len; ++roi) {
            entry.component_pixel_indices[roi].resize(component_count);
            entry.component_contours_xy[roi].resize(component_count);
        }
    }

    auto copyEyePixelsFromComponents = [&]() {
        if (component_count == 0) {
            return;
        }
        for (size_t roi = 0; roi < chunk_len; ++roi) {
            const auto& components_for_roi =
                entry.component_pixel_indices[roi];
            for (size_t eye = 0; eye < 2; ++eye) {
                auto& indices_vec = entry.pixel_indices[roi][eye];
                const size_t source_channel =
                    data_.eye_mask_channel_indices[eye];
                auto component_it = std::find_if(
                    overlay_components.begin(),
                    overlay_components.end(),
                    [&](const ZarrDetectionData::RefinedSubjectMaskComponentInfo&
                            component) {
                        return component.channel_index == source_channel;
                    });
                if (component_it == overlay_components.end()) {
                    indices_vec.clear();
                    continue;
                }
                const size_t component_index = static_cast<size_t>(
                    std::distance(
                        overlay_components.begin(),
                        component_it));
                if (component_index >= components_for_roi.size()) {
                    indices_vec.clear();
                    continue;
                }
                indices_vec = components_for_roi[component_index];
            }
        }
    };

    if (use_rle_masks) {
        const size_t total_pixels = rows * cols;
        auto decodeRleToPixelIndices =
            [&](const uint32_t* counts,
                size_t n_counts,
                std::vector<uint32_t>& indices_vec,
                size_t& count_sum,
                size_t& decoded_area) -> bool {
            indices_vec.clear();
            indices_vec.reserve(256);
            count_sum = 0;
            decoded_area = 0;
            size_t offset = 0;
            uint8_t value = 0;
            for (size_t idx = 0; idx < n_counts; ++idx) {
                const size_t count = static_cast<size_t>(counts[idx]);
                if (offset + count > total_pixels) {
                    return false;
                }
                if (value != 0 && count > 0) {
                    indices_vec.reserve(indices_vec.size() + count);
                    for (size_t flat = offset; flat < offset + count; ++flat) {
                        const size_t y = flat % rows;
                        const size_t x = flat / rows;
                        indices_vec.push_back(
                            static_cast<uint32_t>(y * cols + x));
                    }
                    decoded_area += count;
                }
                offset += count;
                value = static_cast<uint8_t>(1 - value);
            }
            count_sum = offset;
            return offset == total_pixels;
        };

        for (size_t component = 0; component < component_count; ++component) {
            const auto& component_info =
                overlay_components[component];
            if (!component_info.rle_available ||
                component_info.rle_indptr.size() < data_.eye_mask_roi_count + 1 ||
                component_info.rle_present.size() < data_.eye_mask_roi_count ||
                component_info.rle_area_px.size() < data_.eye_mask_roi_count) {
                continue;
            }
            ++rle_components_available;

            int64_t read_start = std::numeric_limits<int64_t>::max();
            int64_t read_end = 0;
            std::vector<uint8_t> row_has_rle(chunk_len, 0);
            const auto row_scan_start = std::chrono::steady_clock::now();
            for (size_t local_roi = 0; local_roi < chunk_len; ++local_roi) {
                const size_t absolute_roi = chunk_start + local_roi;
                if (absolute_roi >= data_.eye_mask_roi_count ||
                    component_info.rle_present[absolute_roi] == 0) {
                    continue;
                }
                const int64_t start = component_info.rle_indptr[absolute_roi];
                const int64_t stop = component_info.rle_indptr[absolute_roi + 1];
                if (start < 0 || stop < start ||
                    static_cast<uint64_t>(stop) >
                        static_cast<uint64_t>(component_info.rle_counts_count)) {
                    std::cerr << "[SUBJECT_MASK_RLE_WARNING] Invalid RLE indptr for component '"
                              << component_info.label << "' row "
                              << absolute_roi << std::endl;
                    continue;
                }
                if (stop == start) {
                    continue;
                }
                row_has_rle[local_roi] = 1;
                ++rle_rows_present;
                read_start = std::min(read_start, start);
                read_end = std::max(read_end, stop);
            }
            rle_row_scan_ms += elapsedMsSince(row_scan_start);

            if (read_start == std::numeric_limits<int64_t>::max() ||
                read_end <= read_start) {
                continue;
            }
            ++rle_components_with_rows;

            auto counts_slice = component_info.rle_counts_store |
                ts::Dims(0).HalfOpenInterval(
                    static_cast<ts::Index>(read_start),
                    static_cast<ts::Index>(read_end));
            const auto counts_read_start = std::chrono::steady_clock::now();
            auto counts_result = ts::Read(counts_slice).result();
            rle_counts_read_ms += elapsedMsSince(counts_read_start);
            if (!counts_result.ok()) {
                std::cerr << "[SUBJECT_MASK_RLE_WARNING] Failed to read RLE counts for component '"
                          << component_info.label << "': "
                          << counts_result.status().ToString()
                          << std::endl;
                continue;
            }

            auto counts_array = counts_result.value();
            auto counts_shape = counts_array.shape();
            auto counts_strides = counts_array.byte_strides();
            if (counts_shape.size() != 1 || counts_strides.size() != 1) {
                std::cerr << "[SUBJECT_MASK_RLE_WARNING] Unexpected RLE counts shape for component '"
                          << component_info.label << "'" << std::endl;
                continue;
            }
            const uint32_t* counts_origin =
                static_cast<const uint32_t*>(
                    counts_array.byte_strided_origin_pointer());
            const uint8_t* counts_base =
                reinterpret_cast<const uint8_t*>(counts_origin);
            const ts::Index stride_count = counts_strides[0];
            auto countValue = [&](size_t local_count) -> uint32_t {
                const uint8_t* ptr = counts_base +
                    stride_count * static_cast<ts::Index>(local_count);
                return *reinterpret_cast<const uint32_t*>(ptr);
            };
            const size_t counts_len = static_cast<size_t>(counts_shape[0]);
            std::vector<uint32_t> local_counts(counts_len);
            const auto counts_copy_start = std::chrono::steady_clock::now();
            for (size_t idx = 0; idx < counts_len; ++idx) {
                local_counts[idx] = countValue(idx);
            }
            rle_counts_copy_ms += elapsedMsSince(counts_copy_start);
            rle_count_values_read += counts_len;

            for (size_t local_roi = 0; local_roi < chunk_len; ++local_roi) {
                if (row_has_rle[local_roi] == 0) {
                    continue;
                }
                const size_t absolute_roi = chunk_start + local_roi;
                const int64_t start = component_info.rle_indptr[absolute_roi];
                const int64_t stop = component_info.rle_indptr[absolute_roi + 1];
                const int64_t local_start = start - read_start;
                const int64_t local_stop = stop - read_start;
                if (local_start < 0 || local_stop < local_start ||
                    static_cast<size_t>(local_stop) > local_counts.size()) {
                    continue;
                }
                auto& components_for_roi =
                    entry.component_pixel_indices[local_roi];
                components_for_roi.resize(component_count);
                entry.component_contours_xy[local_roi].resize(component_count);
                size_t count_sum = 0;
                size_t decoded_area = 0;
                const auto decode_start = std::chrono::steady_clock::now();
                const bool decoded = decodeRleToPixelIndices(
                    local_counts.data() + static_cast<size_t>(local_start),
                    static_cast<size_t>(local_stop - local_start),
                    components_for_roi[component],
                    count_sum,
                    decoded_area);
                rle_decode_ms += elapsedMsSince(decode_start);
                const int32_t expected_area =
                    component_info.rle_area_px[absolute_roi];
                if (!decoded) {
                    components_for_roi[component].clear();
                    std::cerr << "[SUBJECT_MASK_RLE_WARNING] RLE decode failed for component '"
                              << component_info.label << "' row "
                              << absolute_roi << " (count sum "
                              << count_sum << ", expected " << total_pixels
                              << ")" << std::endl;
                    continue;
                }
                ++rle_rows_decoded;
                rle_foreground_pixels += decoded_area;
                if (expected_area >= 0 &&
                    decoded_area != static_cast<size_t>(expected_area)) {
                    std::cerr << "[SUBJECT_MASK_RLE_WARNING] RLE decoded area mismatch for component '"
                              << component_info.label << "' row "
                              << absolute_roi << ": decoded "
                              << decoded_area << ", area_px "
                              << expected_area << std::endl;
                }
                if (data_.refined_subject_mask_rle_smoke_log_count < 12) {
                    const float offset_x =
                        absolute_roi < data_.refined_subject_mask_offset_x.size()
                            ? data_.refined_subject_mask_offset_x[absolute_roi]
                            : std::numeric_limits<float>::quiet_NaN();
                    const float offset_y =
                        absolute_roi < data_.refined_subject_mask_offset_y.size()
                            ? data_.refined_subject_mask_offset_y[absolute_roi]
                            : std::numeric_limits<float>::quiet_NaN();
                    std::cout << "[SUBJECT_MASK_RLE_SMOKE] mask_row="
                              << absolute_roi
                              << " component=" << component_info.label
                              << " rle_count_sum=" << count_sum
                              << " decoded_area=" << decoded_area
                              << " area_px=" << expected_area
                              << " placement_xy=(" << offset_x << ","
                              << offset_y << ")";
                    if (absolute_roi < component_info.rle_bbox_xyxy.size()) {
                        const auto bbox =
                            component_info.rle_bbox_xyxy[absolute_roi];
                        std::cout << " bbox_xyxy=[" << bbox[0] << ","
                                  << bbox[1] << "," << bbox[2] << ","
                                  << bbox[3] << "]";
                    }
                    std::cout << std::endl;
                    ++data_.refined_subject_mask_rle_smoke_log_count;
                }
            }
        }
        const auto copy_start = std::chrono::steady_clock::now();
        copyEyePixelsFromComponents();
        component_copy_ms += elapsedMsSince(copy_start);
    } else if (use_bitpacked_masks) {
        auto slice = data_.eye_masks_bitpacked_store |
                     ts::Dims(0).HalfOpenInterval(
                         static_cast<ts::Index>(chunk_start),
                         static_cast<ts::Index>(chunk_end));
        const auto bitpacked_read_start = std::chrono::steady_clock::now();
        auto read_result = ts::Read(slice).result();
        bitpacked_read_ms += elapsedMsSince(bitpacked_read_start);
        if (!read_result.ok()) {
            std::cerr << "[SUBJECT_MASK_WARNING] Failed to read bitpacked mask chunk "
                      << chunk_id << ": "
                      << read_result.status().ToString() << std::endl;
            return false;
        }

        auto array = read_result.value();
        auto shape = array.shape();
        if (shape.size() != 4) {
            std::cerr << "[SUBJECT_MASK_WARNING] Unexpected bitpacked mask chunk rank ("
                      << shape.size() << ")" << std::endl;
            return false;
        }

        chunk_len = static_cast<size_t>(shape[0]);
        channels = static_cast<size_t>(shape[1]);
        rows = static_cast<size_t>(shape[2]);
        const size_t packed_cols = static_cast<size_t>(shape[3]);
        const size_t required_packed_cols = (cols + 7) / 8;
        if (packed_cols < required_packed_cols) {
            std::cerr << "[SUBJECT_MASK_WARNING] Bitpacked mask chunk has packed width "
                      << packed_cols << " but logical width " << cols
                      << " requires " << required_packed_cols << " bytes."
                      << std::endl;
            return false;
        }

        const uint8_t* base_ptr =
            static_cast<const uint8_t*>(array.byte_strided_origin_pointer());
        auto byte_strides = array.byte_strides();
        if (byte_strides.size() != 4) {
            std::cerr << "[SUBJECT_MASK_WARNING] Unexpected bitpacked mask chunk stride rank ("
                      << byte_strides.size() << ")" << std::endl;
            return false;
        }

        const ts::Index stride_roi = byte_strides[0];
        const ts::Index stride_channel = byte_strides[1];
        const ts::Index stride_row = byte_strides[2];
        const ts::Index stride_col = byte_strides[3];

        auto collectPackedChannelPixels =
            [&](const uint8_t* roi_ptr,
                size_t source_channel,
                std::vector<uint32_t>& indices_vec) {
                indices_vec.clear();
                indices_vec.reserve(256);
                if (source_channel == kInvalidMaskChannel ||
                    source_channel >= channels) {
                    return;
                }

                const auto channel_offset =
                    stride_channel * static_cast<ts::Index>(source_channel);
                const uint8_t* channel_ptr = roi_ptr + channel_offset;
                for (size_t r = 0; r < rows; ++r) {
                    const auto row_offset =
                        stride_row * static_cast<ts::Index>(r);
                    const uint8_t* row_ptr = channel_ptr + row_offset;
                    for (size_t packed_x = 0;
                         packed_x < required_packed_cols;
                         ++packed_x) {
                        const auto col_offset =
                            stride_col * static_cast<ts::Index>(packed_x);
                        const uint8_t byte = *(row_ptr + col_offset);
                        if (byte == 0) {
                            continue;
                        }
                        for (size_t bit = 0; bit < 8; ++bit) {
                            const size_t x = packed_x * 8 + bit;
                            if (x >= cols) {
                                break;
                            }
                            if ((byte & static_cast<uint8_t>(1u << bit)) != 0) {
                                indices_vec.push_back(
                                    static_cast<uint32_t>(r * cols + x));
                            }
                        }
                    }
                }
            };

        for (size_t roi = 0; roi < chunk_len; ++roi) {
            const auto roi_offset =
                stride_roi * static_cast<ts::Index>(roi);
            const uint8_t* roi_ptr = base_ptr + roi_offset;
            const auto unpack_start = std::chrono::steady_clock::now();

            if (component_count > 0) {
                auto& components_for_roi = entry.component_pixel_indices[roi];
                components_for_roi.resize(component_count);
                entry.component_contours_xy[roi].resize(component_count);
                for (size_t component = 0;
                     component < component_count;
                     ++component) {
                    collectPackedChannelPixels(
                        roi_ptr,
                        overlay_components[component].channel_index,
                        components_for_roi[component]);
                    bitpacked_foreground_pixels +=
                        components_for_roi[component].size();
                }
                bitpacked_unpack_ms += elapsedMsSince(unpack_start);
                continue;
            }

            for (size_t eye = 0; eye < 2; ++eye) {
                auto& indices_vec = entry.pixel_indices[roi][eye];
                collectPackedChannelPixels(
                    roi_ptr,
                    data_.eye_mask_channel_indices[eye],
                    indices_vec);
                bitpacked_foreground_pixels += indices_vec.size();
            }
            bitpacked_unpack_ms += elapsedMsSince(unpack_start);
        }
        const auto copy_start = std::chrono::steady_clock::now();
        copyEyePixelsFromComponents();
        component_copy_ms += elapsedMsSince(copy_start);
    } else {
        auto slice = data_.eye_masks_store |
                     ts::Dims(0).HalfOpenInterval(
                         static_cast<ts::Index>(chunk_start),
                         static_cast<ts::Index>(chunk_end));
        const auto dense_read_start = std::chrono::steady_clock::now();
        auto read_result = ts::Read(slice).result();
        dense_read_ms += elapsedMsSince(dense_read_start);
        if (!read_result.ok()) {
            std::cerr << "[EYE_MASK_WARNING] Failed to read mask chunk "
                      << chunk_id << ": " << read_result.status().ToString()
                      << std::endl;
            return false;
        }

        auto array = read_result.value();
        auto shape = array.shape();
        if (shape.size() != 4) {
            std::cerr << "[EYE_MASK_WARNING] Unexpected mask chunk rank ("
                      << shape.size() << ")" << std::endl;
            return false;
        }

        chunk_len = static_cast<size_t>(shape[0]);
        channels = static_cast<size_t>(shape[1]);
        rows = static_cast<size_t>(shape[2]);
        cols = static_cast<size_t>(shape[3]);

        const uint8_t* base_ptr =
            static_cast<const uint8_t*>(array.byte_strided_origin_pointer());
        auto byte_strides = array.byte_strides();
        if (byte_strides.size() != 4) {
            std::cerr << "[EYE_MASK_WARNING] Unexpected mask chunk stride rank ("
                      << byte_strides.size() << ")" << std::endl;
            return false;
        }

        const ts::Index stride_roi = byte_strides[0];
        const ts::Index stride_channel = byte_strides[1];
        const ts::Index stride_row = byte_strides[2];
        const ts::Index stride_col = byte_strides[3];

        auto collectChannelPixels =
            [&](const uint8_t* roi_ptr,
                size_t source_channel,
                std::vector<uint32_t>& indices_vec) {
                indices_vec.clear();
                indices_vec.reserve(256);
                if (source_channel == kInvalidMaskChannel ||
                    source_channel >= channels) {
                    return;
                }

                const auto channel_offset =
                    stride_channel * static_cast<ts::Index>(source_channel);
                const uint8_t* channel_ptr = roi_ptr + channel_offset;
                for (size_t r = 0; r < rows; ++r) {
                    const auto row_offset =
                        stride_row * static_cast<ts::Index>(r);
                    const uint8_t* row_ptr = channel_ptr + row_offset;
                    for (size_t c = 0; c < cols; ++c) {
                        const auto col_offset =
                            stride_col * static_cast<ts::Index>(c);
                        const uint8_t* elem_ptr = row_ptr + col_offset;
                        if (*elem_ptr != 0) {
                            indices_vec.push_back(
                                static_cast<uint32_t>(r * cols + c));
                        }
                    }
                }
            };

        for (size_t roi = 0; roi < chunk_len; ++roi) {
            const auto roi_offset =
                stride_roi * static_cast<ts::Index>(roi);
            const uint8_t* roi_ptr = base_ptr + roi_offset;
            const auto collect_start = std::chrono::steady_clock::now();

            if (component_count > 0) {
                auto& components_for_roi = entry.component_pixel_indices[roi];
                components_for_roi.resize(component_count);
                entry.component_contours_xy[roi].resize(component_count);
                for (size_t component = 0; component < component_count; ++component) {
                    collectChannelPixels(
                        roi_ptr,
                        overlay_components[component].channel_index,
                        components_for_roi[component]);
                    dense_nonzero_pixels += components_for_roi[component].size();
                }
                dense_collect_ms += elapsedMsSince(collect_start);
                continue;
            }

            for (size_t eye = 0; eye < 2; ++eye) {
                auto& indices_vec = entry.pixel_indices[roi][eye];
                collectChannelPixels(
                    roi_ptr,
                    data_.eye_mask_channel_indices[eye],
                    indices_vec);
                dense_nonzero_pixels += indices_vec.size();
            }
            dense_collect_ms += elapsedMsSince(collect_start);
        }
        const auto copy_start = std::chrono::steady_clock::now();
        copyEyePixelsFromComponents();
        component_copy_ms += elapsedMsSince(copy_start);
    }

    for (size_t component = 0; component < component_count; ++component) {
        const auto& component_info =
            overlay_components[component];
        if (!component_info.contours_available ||
            component_info.contour_ptr.size() < data_.eye_mask_roi_count ||
            component_info.contour_len.size() < data_.eye_mask_roi_count ||
            component_info.contour_points_count == 0) {
            continue;
        }

        int64_t read_start = std::numeric_limits<int64_t>::max();
        int64_t read_end = 0;
        std::vector<uint8_t> row_has_contour(chunk_len, 0);
        const auto contour_scan_start = std::chrono::steady_clock::now();
        for (size_t local_roi = 0; local_roi < chunk_len; ++local_roi) {
            const size_t absolute_roi = chunk_start + local_roi;
            if (absolute_roi >= data_.eye_mask_roi_count ||
                absolute_roi >= component_info.contour_ptr.size() ||
                absolute_roi >= component_info.contour_len.size()) {
                continue;
            }
            const int64_t start = component_info.contour_ptr[absolute_roi];
            const int32_t length = component_info.contour_len[absolute_roi];
            if (start < 0 || length <= 1) {
                continue;
            }
            const int64_t end = start + static_cast<int64_t>(length);
            if (end <= start ||
                static_cast<uint64_t>(end) >
                    static_cast<uint64_t>(component_info.contour_points_count)) {
                continue;
            }
            row_has_contour[local_roi] = 1;
            ++contour_rows_loaded;
            read_start = std::min(read_start, start);
            read_end = std::max(read_end, end);
        }
        contour_row_scan_ms += elapsedMsSince(contour_scan_start);

        if (read_start == std::numeric_limits<int64_t>::max() ||
            read_end <= read_start) {
            continue;
        }
        ++contour_components_with_rows;

        auto contour_slice = component_info.contour_points_store |
            ts::Dims(0).HalfOpenInterval(
                static_cast<ts::Index>(read_start),
                static_cast<ts::Index>(read_end));
        const auto contour_read_start = std::chrono::steady_clock::now();
        auto contour_result = ts::Read(contour_slice).result();
        contour_read_ms += elapsedMsSince(contour_read_start);
        if (!contour_result.ok()) {
            std::cerr << "[SUBJECT_MASK_WARNING] Failed to read contour chunk for component '"
                      << component_info.label << "': "
                      << contour_result.status().ToString()
                      << std::endl;
            continue;
        }

        auto contour_array = contour_result.value();
        auto contour_shape = contour_array.shape();
        auto contour_strides = contour_array.byte_strides();
        if (contour_shape.size() != 2 || contour_shape[1] < 2 ||
            contour_strides.size() != 2) {
            std::cerr << "[SUBJECT_MASK_WARNING] Unexpected contour chunk shape for component '"
                      << component_info.label << "'" << std::endl;
            continue;
        }

        const float* contour_origin =
            static_cast<const float*>(
                contour_array.byte_strided_origin_pointer());
        const uint8_t* contour_base =
            reinterpret_cast<const uint8_t*>(contour_origin);
        const ts::Index stride_point = contour_strides[0];
        const ts::Index stride_xy = contour_strides[1];
        auto pointValue = [&](size_t local_point, size_t xy) -> float {
            const uint8_t* ptr = contour_base +
                stride_point * static_cast<ts::Index>(local_point) +
                stride_xy * static_cast<ts::Index>(xy);
            return *reinterpret_cast<const float*>(ptr);
        };

        for (size_t local_roi = 0; local_roi < chunk_len; ++local_roi) {
            if (row_has_contour[local_roi] == 0) {
                continue;
            }
            const size_t absolute_roi = chunk_start + local_roi;
            const int64_t start = component_info.contour_ptr[absolute_roi];
            const int32_t length = component_info.contour_len[absolute_roi];
            const int64_t local_start = start - read_start;
            if (local_start < 0) {
                continue;
            }

            auto& contour_points =
                entry.component_contours_xy[local_roi][component];
            contour_points.clear();
            contour_points.reserve(static_cast<size_t>(length));
            const auto contour_copy_start = std::chrono::steady_clock::now();
            for (int32_t point = 0; point < length; ++point) {
                const size_t local_point =
                    static_cast<size_t>(local_start + point);
                if (local_point >= static_cast<size_t>(contour_shape[0])) {
                    break;
                }
                const float x = pointValue(local_point, 0);
                const float y = pointValue(local_point, 1);
                if (std::isfinite(x) && std::isfinite(y)) {
                    contour_points.push_back({x, y});
                }
            }
            contour_copy_ms += elapsedMsSince(contour_copy_start);
            contour_points_loaded += contour_points.size();
        }
    }

    bool optional_overlay_changed = false;
    {
        std::lock_guard<std::mutex> cache_lock(*data_.mask_chunk_cache_mutex);
        optional_overlay_changed =
            optional_overlay_generation !=
            refined_subject_mask_optional_overlay_publish_generation_;
        auto& cache = data_.mask_chunk_cache;
        auto existing = std::find_if(
            cache.begin(),
            cache.end(),
            [&](const ZarrDetectionData::EyeMaskChunkCacheEntry& cache_entry) {
                return cache_entry.chunk_id == chunk_id;
            });
        if (existing != cache.end() && force_reload) {
            cache.erase(existing);
            cache.push_back(std::move(entry));
        } else if (existing == cache.end()) {
            const size_t cache_capacity = (use_rle_masks || use_bitpacked_masks)
                ? kRleEyeMaskChunkCacheCapacity
                : kEyeMaskChunkCacheCapacity;
            if (cache.size() >= cache_capacity) {
                cache.erase(cache.begin());
            }
            cache.push_back(std::move(entry));
        }
    }
    if (optional_overlay_changed) {
        return ensureEyeMaskChunk(chunk_id, allow_prefetch, force_reload);
    }

    if (chunk_perf_log) {
        std::cout << "[SUBJECT_MASK_CHUNK_PERF] source="
                  << mask_storage_surface
                  << " chunk_id=" << chunk_id
                  << " chunk_start=" << chunk_start
                  << " chunk_len=" << chunk_len
                  << " components=" << component_count
                  << " cache_hit=no"
                  << " force_reload=" << (force_reload ? "yes" : "no")
                  << " allow_prefetch=" << (allow_prefetch ? "yes" : "no")
                  << " thread=" << std::this_thread::get_id()
                  << " rle_components_available=" << rle_components_available
                  << " rle_components_with_rows=" << rle_components_with_rows
                  << " rle_rows_present=" << rle_rows_present
                  << " rle_rows_decoded=" << rle_rows_decoded
                  << " rle_count_values_read=" << rle_count_values_read
                  << " rle_foreground_pixels=" << rle_foreground_pixels
                  << " rle_row_scan_ms=" << rle_row_scan_ms
                  << " rle_counts_read_ms=" << rle_counts_read_ms
                  << " rle_counts_copy_ms=" << rle_counts_copy_ms
                  << " rle_decode_ms=" << rle_decode_ms
                  << " dense_read_ms=" << dense_read_ms
                  << " dense_collect_ms=" << dense_collect_ms
                  << " dense_nonzero_pixels=" << dense_nonzero_pixels
                  << " bitpacked_read_ms=" << bitpacked_read_ms
                  << " bitpacked_unpack_ms=" << bitpacked_unpack_ms
                  << " bitpacked_foreground_pixels="
                  << bitpacked_foreground_pixels
                  << " component_copy_ms=" << component_copy_ms
                  << " contour_components_with_rows="
                  << contour_components_with_rows
                  << " contour_rows_loaded=" << contour_rows_loaded
                  << " contour_points_loaded=" << contour_points_loaded
                  << " contour_row_scan_ms=" << contour_row_scan_ms
                  << " contour_read_ms=" << contour_read_ms
                  << " contour_copy_ms=" << contour_copy_ms
                  << " total_ms=" << elapsedMsSince(ensure_start)
                  << std::endl;
    }
    if (allow_prefetch) {
        prefetchAdjacentEyeMaskChunks(chunk_id);
    }
    return true;
}

void ZarrDetectionLoader::prefetchAdjacentEyeMaskChunks(size_t chunk_id) const {
    size_t chunk_rows =
        data_.eye_mask_chunk_rows > 0 ? data_.eye_mask_chunk_rows : 512;
    if (chunk_id > 0) {
        requestEyeMaskChunkPrefetch(chunk_id - 1);
    }
    if ((chunk_id + 1) * chunk_rows < data_.eye_mask_roi_count) {
        requestEyeMaskChunkPrefetch(chunk_id + 1);
    }
}

void ZarrDetectionLoader::stopEyeMaskPrefetchWorker() const {
    bool should_join = false;
    {
        std::lock_guard<std::mutex> prefetch_lock(eye_mask_prefetch_mutex_);
        eye_mask_prefetch_stop_requested_ = true;
        eye_mask_prefetch_queue_.clear();
        should_join = eye_mask_prefetch_worker_.joinable();
    }
    eye_mask_prefetch_cv_.notify_all();

    if (should_join) {
        eye_mask_prefetch_worker_.join();
    }

    {
        std::lock_guard<std::mutex> prefetch_lock(eye_mask_prefetch_mutex_);
        eye_mask_prefetch_stop_requested_ = false;
        eye_mask_prefetch_queue_.clear();
    }
    {
        std::lock_guard<std::mutex> cache_lock(*data_.mask_chunk_cache_mutex);
        data_.mask_chunk_loads_in_flight.clear();
    }
}

void ZarrDetectionLoader::eyeMaskPrefetchWorkerLoop() const {
    for (;;) {
        size_t chunk_id = std::numeric_limits<size_t>::max();
        {
            std::unique_lock<std::mutex> prefetch_lock(
                eye_mask_prefetch_mutex_);
            eye_mask_prefetch_cv_.wait(prefetch_lock, [&]() {
                return eye_mask_prefetch_stop_requested_ ||
                       !eye_mask_prefetch_queue_.empty();
            });
            if (eye_mask_prefetch_stop_requested_) {
                return;
            }
            chunk_id = eye_mask_prefetch_queue_.front();
            eye_mask_prefetch_queue_.pop_front();
        }

        ensureEyeMaskChunk(chunk_id, /*allow_prefetch=*/false);
        {
            std::lock_guard<std::mutex> cache_lock(
                *data_.mask_chunk_cache_mutex);
            data_.mask_chunk_loads_in_flight.erase(chunk_id);
        }
    }
}

void ZarrDetectionLoader::requestEyeMaskChunkPrefetch(size_t chunk_id) const {
    if (!data_.eye_masks_loaded || data_.eye_mask_roi_count == 0) {
        return;
    }

    size_t chunk_rows =
        data_.eye_mask_chunk_rows > 0 ? data_.eye_mask_chunk_rows : 512;
    if (chunk_id * chunk_rows >= data_.eye_mask_roi_count) {
        return;
    }

    {
        std::lock_guard<std::mutex> cache_lock(*data_.mask_chunk_cache_mutex);
        const bool already_cached =
            std::find_if(
                data_.mask_chunk_cache.begin(),
                data_.mask_chunk_cache.end(),
                [&](const ZarrDetectionData::EyeMaskChunkCacheEntry& entry) {
                    return entry.chunk_id == chunk_id;
                }) != data_.mask_chunk_cache.end();
        if (already_cached ||
            data_.mask_chunk_loads_in_flight.count(chunk_id) != 0) {
            return;
        }
        data_.mask_chunk_loads_in_flight.insert(chunk_id);
    }

    std::vector<size_t> dropped_chunks;
    bool queued = false;
    {
        std::lock_guard<std::mutex> prefetch_lock(eye_mask_prefetch_mutex_);
        if (!eye_mask_prefetch_stop_requested_) {
            while (eye_mask_prefetch_queue_.size() >=
                   kEyeMaskPrefetchQueueCapacity) {
                dropped_chunks.push_back(eye_mask_prefetch_queue_.front());
                eye_mask_prefetch_queue_.pop_front();
            }
            if (!eye_mask_prefetch_worker_.joinable()) {
                eye_mask_prefetch_worker_ = std::thread(
                    &ZarrDetectionLoader::eyeMaskPrefetchWorkerLoop, this);
            }
            eye_mask_prefetch_queue_.push_back(chunk_id);
            queued = true;
        }
    }

    if (!dropped_chunks.empty() || !queued) {
        std::lock_guard<std::mutex> cache_lock(*data_.mask_chunk_cache_mutex);
        for (size_t dropped_chunk : dropped_chunks) {
            data_.mask_chunk_loads_in_flight.erase(dropped_chunk);
        }
        if (!queued) {
            data_.mask_chunk_loads_in_flight.erase(chunk_id);
        }
    }
    if (queued) {
        eye_mask_prefetch_cv_.notify_one();
    }
}

bool ZarrDetectionLoader::requestEyeMaskCacheForFrame(
    size_t frame_id,
    size_t lookahead_frames) const {
    if (!data_.eye_masks_loaded || data_.eye_mask_roi_count == 0 ||
        data_.eye_mask_chunk_rows == 0) {
        return false;
    }

    const size_t chunk_rows = data_.eye_mask_chunk_rows;
    const size_t max_frame =
        data_.total_frames > 0
            ? data_.total_frames - 1
            : std::numeric_limits<size_t>::max();
    if (frame_id > max_frame) {
        return false;
    }
    const size_t end_frame =
        lookahead_frames > max_frame - frame_id
            ? max_frame
            : frame_id + lookahead_frames;

    std::set<size_t> chunk_ids;
    auto add_roi = [&](size_t roi_index) {
        if (roi_index < data_.eye_mask_roi_count) {
            chunk_ids.insert(roi_index / chunk_rows);
        }
    };
    auto collect_frame = [&](size_t frame) {
        if (data_.eye_masks_from_refined_subject_masks &&
            frame < data_.refined_subject_mask_rows_by_frame.size()) {
            for (size_t mask_row :
                 data_.refined_subject_mask_rows_by_frame[frame]) {
                add_roi(mask_row);
            }
            return;
        }

        if (frame + 1 >= data_.frame_offsets.size()) {
            return;
        }
        const size_t start = data_.frame_offsets[frame];
        const size_t end = data_.frame_offsets[frame + 1];
        for (size_t detection_idx = start; detection_idx < end;
             ++detection_idx) {
            if (detection_idx >= data_.mask_roi_indices.size()) {
                break;
            }
            const int32_t roi_index = data_.mask_roi_indices[detection_idx];
            if (roi_index >= 0) {
                add_roi(static_cast<size_t>(roi_index));
            }
        }
    };

    for (size_t frame = frame_id; frame <= end_frame; ++frame) {
        collect_frame(frame);
        if (frame == std::numeric_limits<size_t>::max()) {
            break;
        }
    }
    size_t queued_chunks = 0;
    for (size_t chunk_id : chunk_ids) {
        if (queued_chunks >= kEyeMaskPrefetchQueueCapacity) {
            break;
        }
        requestEyeMaskChunkPrefetch(chunk_id);
        ++queued_chunks;
    }
    return !chunk_ids.empty();
}

bool ZarrDetectionLoader::warmEyeMaskCacheForFrame(size_t frame_id) const {
    if (!data_.eye_masks_loaded || data_.eye_mask_roi_count == 0) {
        return false;
    }

    bool found_roi = false;
    size_t roi_index = 0;
    if (data_.eye_masks_from_refined_subject_masks &&
        frame_id < data_.refined_subject_mask_rows_by_frame.size()) {
        for (size_t mask_row :
             data_.refined_subject_mask_rows_by_frame[frame_id]) {
            if (mask_row < data_.eye_mask_roi_count) {
                roi_index = mask_row;
                found_roi = true;
                break;
            }
        }
    }

    if (!found_roi && frame_id + 1 < data_.frame_offsets.size()) {
        const size_t start = data_.frame_offsets[frame_id];
        const size_t end = data_.frame_offsets[frame_id + 1];
        for (size_t detection_idx = start; detection_idx < end;
             ++detection_idx) {
            if (detection_idx >= data_.mask_roi_indices.size()) {
                break;
            }
            const int32_t candidate_roi = data_.mask_roi_indices[detection_idx];
            if (candidate_roi >= 0 &&
                static_cast<size_t>(candidate_roi) < data_.eye_mask_roi_count) {
                roi_index = static_cast<size_t>(candidate_roi);
                found_roi = true;
                break;
            }
        }
    }

    if (!found_roi) {
        return false;
    }

    if (data_.eye_mask_chunk_rows == 0) {
        return false;
    }
    return ensureEyeMaskChunk(roi_index / data_.eye_mask_chunk_rows);
}

bool ZarrDetectionLoader::readRefinedSubjectMaskComponentRow(
    size_t roi_index,
    const std::string& component_name,
    RefinedSubjectMaskComponentRow& out_row,
    std::string* error_message) const {
    out_row = RefinedSubjectMaskComponentRow{};

    auto fail = [&](const std::string& message) -> bool {
        if (error_message != nullptr) {
            *error_message = message;
        }
        return false;
    };

    if (!data_.eye_masks_loaded || !data_.eye_masks_from_refined_subject_masks) {
        return fail("Refined subject masks are not the active mask source.");
    }
    if (roi_index >= data_.eye_mask_roi_count) {
        std::ostringstream oss;
        oss << "ROI row " << roi_index
            << " is outside refined subject-mask row count "
            << data_.eye_mask_roi_count << ".";
        return fail(oss.str());
    }
    if (component_name.empty()) {
        return fail("No refined subject-mask component selected.");
    }
    if (data_.refined_subject_mask_rle_masks_used ||
        data_.refined_subject_mask_bitpacked_masks_used) {
        return fail("Compact refined subject-mask rows are display-only here; materialize dense masks_roi before editing this refined subject-mask run.");
    }

    ZarrDetectionData::RefinedSubjectMaskComponentInfo selected_component;
    bool found_component = false;
    {
        std::lock_guard<std::mutex> cache_lock(*data_.mask_chunk_cache_mutex);
        auto component_it = std::find_if(
            data_.refined_subject_mask_overlay_components.begin(),
            data_.refined_subject_mask_overlay_components.end(),
            [&](const ZarrDetectionData::RefinedSubjectMaskComponentInfo& component) {
                return component.label == component_name;
            });
        if (component_it != data_.refined_subject_mask_overlay_components.end()) {
            selected_component = *component_it;
            found_component = true;
        }
    }
    if (!found_component) {
        return fail("Selected refined subject-mask component is unavailable: " +
                    component_name);
    }

    const size_t channel_index = selected_component.channel_index;
    if (channel_index == kInvalidMaskChannel ||
        channel_index >= data_.refined_subject_mask_labels.size()) {
        return fail("Selected refined subject-mask component has no readable channel: " +
                    component_name);
    }
    if (channel_index >= data_.refined_subject_mask_available_channels.size() ||
        data_.refined_subject_mask_available_channels[channel_index] == 0) {
        return fail("Selected refined subject-mask component is not marked available: " +
                    component_name);
    }

    auto slice = data_.eye_masks_store |
                 ts::Dims(0).HalfOpenInterval(
                     static_cast<ts::Index>(roi_index),
                     static_cast<ts::Index>(roi_index + 1)) |
                 ts::Dims(1).HalfOpenInterval(
                     static_cast<ts::Index>(channel_index),
                     static_cast<ts::Index>(channel_index + 1));
    auto read_result = ts::Read(slice).result();
    if (!read_result.ok()) {
        return fail("Failed to read refined subject-mask component row: " +
                    read_result.status().ToString());
    }

    auto array = read_result.value();
    auto shape = array.shape();
    auto byte_strides = array.byte_strides();
    if (shape.size() != 4 || byte_strides.size() != 4 ||
        shape[0] != 1 || shape[1] != 1) {
        return fail("Unexpected refined subject-mask component row shape.");
    }

    const size_t rows = static_cast<size_t>(shape[2]);
    const size_t cols = static_cast<size_t>(shape[3]);
    if (rows == 0 || cols == 0) {
        return fail("Selected refined subject-mask component row is empty.");
    }

    std::vector<uint8_t> mask(rows * cols, 0);
    const uint8_t* origin =
        static_cast<const uint8_t*>(array.byte_strided_origin_pointer());
    const ts::Index stride_row = byte_strides[2];
    const ts::Index stride_col = byte_strides[3];
    for (size_t row = 0; row < rows; ++row) {
        const uint8_t* row_ptr =
            origin + stride_row * static_cast<ts::Index>(row);
        for (size_t col = 0; col < cols; ++col) {
            const uint8_t* value_ptr =
                row_ptr + stride_col * static_cast<ts::Index>(col);
            mask[row * cols + col] = *value_ptr != 0 ? 1 : 0;
        }
    }

    out_row.valid = true;
    out_row.run_name = data_.eye_masks_run_name;
    out_row.component_name = selected_component.label;
    out_row.channel_index = channel_index;
    out_row.roi_index = roi_index;
    out_row.rows = rows;
    out_row.cols = cols;
    out_row.mask = std::move(mask);
    return true;
}

bool ZarrDetectionLoader::populateEyeMaskEntry(
    size_t roi_index,
    FrameDetections::EyeMask& out_mask,
    bool allow_blocking_load,
    bool request_prefetch_on_miss) const {
    if (!data_.eye_masks_loaded || roi_index >= data_.eye_mask_roi_count) {
        return false;
    }
    size_t chunk_rows =
        data_.eye_mask_chunk_rows > 0 ? data_.eye_mask_chunk_rows : 512;
    size_t chunk_id = roi_index / chunk_rows;
    if (allow_blocking_load) {
        if (!ensureEyeMaskChunk(chunk_id)) {
            return false;
        }
    } else {
        bool cache_hit = false;
        {
            std::lock_guard<std::mutex> cache_lock(
                *data_.mask_chunk_cache_mutex);
            auto& cache = data_.mask_chunk_cache;
            auto it = std::find_if(
                cache.begin(),
                cache.end(),
                [&](const ZarrDetectionData::EyeMaskChunkCacheEntry& entry) {
                    return entry.chunk_id == chunk_id;
                });
            if (it != cache.end()) {
                if (std::next(it) != cache.end()) {
                    ZarrDetectionData::EyeMaskChunkCacheEntry entry =
                        std::move(*it);
                    cache.erase(it);
                    cache.push_back(std::move(entry));
                }
                cache_hit = true;
            }
        }
        if (!cache_hit) {
            if (request_prefetch_on_miss) {
                requestEyeMaskChunkPrefetch(chunk_id);
            }
            return false;
        }
    }
    out_mask.rows = static_cast<int>(data_.eye_mask_height);
    out_mask.cols = static_cast<int>(data_.eye_mask_width);
    out_mask.valid = false;
    out_mask.has_feret_axes = false;
    const float angle_nan = std::numeric_limits<float>::quiet_NaN();
    out_mask.feret_minor_angle_deg[0] = angle_nan;
    out_mask.feret_minor_angle_deg[1] = angle_nan;
    out_mask.feret_angle_valid = {0, 0};
    out_mask.has_eye_angles = false;
    out_mask.eye_frame_angle_deg[0] = angle_nan;
    out_mask.eye_frame_angle_deg[1] = angle_nan;
    out_mask.eye_frame_angle_valid = {0, 0};
    out_mask.eye_frame_vergence_deg = angle_nan;
    out_mask.eye_frame_vergence_valid = 0;
    out_mask.has_eye_frame_angles = false;
    out_mask.subject_mask_components.clear();
    out_mask.has_subject_mask_components = false;
    out_mask.roi_index = static_cast<int32_t>(roi_index);

    std::lock_guard<std::mutex> cache_lock(*data_.mask_chunk_cache_mutex);
    const auto* entry = findEyeMaskChunk(chunk_id);
    if (entry == nullptr) {
        return false;
    }
    size_t local_index = roi_index - entry->chunk_start;
    if (local_index >= entry->pixel_indices.size()) {
        return false;
    }

    if (local_index < entry->component_pixel_indices.size()) {
        const auto& component_pixels =
            entry->component_pixel_indices[local_index];
        const size_t component_count = std::min(
            component_pixels.size(),
            data_.refined_subject_mask_overlay_components.size());
        out_mask.subject_mask_components.reserve(component_count);
        for (size_t component = 0; component < component_count; ++component) {
            FrameDetections::EyeMask::SubjectMaskComponent component_entry;
            component_entry.label =
                data_.refined_subject_mask_overlay_components[component].label;
            component_entry.channel_index =
                data_.refined_subject_mask_overlay_components[component]
                    .channel_index;
            component_entry.pixel_indices = component_pixels[component];
            component_entry.valid = !component_entry.pixel_indices.empty();
            if (local_index < entry->component_contours_xy.size() &&
                component < entry->component_contours_xy[local_index].size()) {
                component_entry.contour_xy =
                    entry->component_contours_xy[local_index][component];
                component_entry.has_contour =
                    component_entry.contour_xy.size() > 1;
            }
            if (component_entry.valid) {
                out_mask.valid = true;
                out_mask.has_subject_mask_components = true;
            }
            out_mask.subject_mask_components.push_back(
                std::move(component_entry));
        }
    }

    for (size_t channel = 0; channel < 2; ++channel) {
        out_mask.pixel_indices[channel] =
            entry->pixel_indices[local_index][channel];
        if (!out_mask.pixel_indices[channel].empty()) {
            out_mask.valid = true;
        }

        if (roi_index < data_.eye_mask_feret_axes_major.size()) {
            const auto& axis_vals =
                data_.eye_mask_feret_axes_major[roi_index][channel];
            bool axis_valid = true;
            for (float value : axis_vals) {
                if (!std::isfinite(value)) {
                    axis_valid = false;
                    break;
                }
            }
            if (axis_valid) {
                float dx = axis_vals[0] - axis_vals[2];
                float dy = axis_vals[1] - axis_vals[3];
                if (std::fabs(dx) > 1e-5f || std::fabs(dy) > 1e-5f) {
                    auto& segment = out_mask.feret_major[channel];
                    segment.valid = true;
                    segment.x0 = axis_vals[0];
                    segment.y0 = axis_vals[1];
                    segment.x1 = axis_vals[2];
                    segment.y1 = axis_vals[3];
                    out_mask.has_feret_axes = true;
                }
            }
        }
        if (roi_index < data_.eye_mask_feret_axes_minor.size()) {
            const auto& axis_vals =
                data_.eye_mask_feret_axes_minor[roi_index][channel];
            bool axis_valid = true;
            for (float value : axis_vals) {
                if (!std::isfinite(value)) {
                    axis_valid = false;
                    break;
                }
            }
            if (axis_valid) {
                float dx = axis_vals[0] - axis_vals[2];
                float dy = axis_vals[1] - axis_vals[3];
                if (std::fabs(dx) > 1e-5f || std::fabs(dy) > 1e-5f) {
                    auto& segment = out_mask.feret_minor[channel];
                    segment.valid = true;
                    segment.x0 = axis_vals[0];
                    segment.y0 = axis_vals[1];
                    segment.x1 = axis_vals[2];
                    segment.y1 = axis_vals[3];
                    out_mask.has_feret_axes = true;
                }
            }
        }
    }
    return out_mask.valid;
}
