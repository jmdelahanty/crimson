#include "zarr_loader_internal.h"
#include <iostream>

using json = nlohmann::json;

namespace {

std::string stimulusJsonStringAttr(const json& attrs, const char* key) {
    if (attrs.contains(key) && attrs[key].is_string()) {
        return attrs[key].get<std::string>();
    }
    return {};
}

std::string stimulusJsonScalarStringAttr(const json& attrs, const char* key) {
    if (!attrs.contains(key)) {
        return {};
    }
    const auto& value = attrs[key];
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<int64_t>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<uint64_t>());
    }
    return {};
}

int32_t stimulusJsonIntAttr(const json& attrs, const char* key) {
    if (attrs.contains(key) && attrs[key].is_number_integer()) {
        return attrs[key].get<int32_t>();
    }
    if (attrs.contains(key) && attrs[key].is_number()) {
        return clampToInt32(static_cast<int64_t>(attrs[key].get<double>()));
    }
    return -1;
}

double stimulusJsonDoubleAttr(const json& attrs, const char* key) {
    if (attrs.contains(key) && attrs[key].is_number()) {
        return attrs[key].get<double>();
    }
    return std::numeric_limits<double>::quiet_NaN();
}

bool stimulusJsonBoolAttr(const json& attrs,
                          const char* key,
                          bool& out_value) {
    if (!attrs.contains(key)) {
        return false;
    }
    if (attrs[key].is_boolean()) {
        out_value = attrs[key].get<bool>();
        return true;
    }
    if (attrs[key].is_number_integer()) {
        out_value = attrs[key].get<int>() != 0;
        return true;
    }
    return false;
}

bool stimulusJsonFiniteDoubleAttr(const json& attrs,
                                  const char* key,
                                  double& out_value) {
    const double value = stimulusJsonDoubleAttr(attrs, key);
    if (!std::isfinite(value)) {
        return false;
    }
    out_value = value;
    return true;
}

bool chaserUsesArenaRelativeCanvas(const ZarrDetectionData::ChaserCoordinateTransform& transform) {
    const std::string frame = toLowerCopy(transform.coordinate_frame);
    const std::string origin = toLowerCopy(transform.coordinate_origin);
    return frame == "arena_relative_canvas_px" ||
           origin == "top_left_of_active_arena";
}

void applyChaserArenaConfigMetadata(const json& attrs,
                                    ZarrDetectionData::ChaserCoordinateTransform& transform) {
    double value = 0.0;
    if (stimulusJsonFiniteDoubleAttr(attrs, "arena_origin_in_canvas_x_px", value)) {
        transform.arena_origin_canvas_x_px = value;
        transform.has_arena_canvas_origin = true;
    }
    if (stimulusJsonFiniteDoubleAttr(attrs, "arena_origin_in_canvas_y_px", value)) {
        transform.arena_origin_canvas_y_px = value;
        transform.has_arena_canvas_origin = true;
    }
    if (stimulusJsonFiniteDoubleAttr(attrs, "arena_region_width_px", value)) {
        transform.arena_region_width_px = value;
    }
    if (stimulusJsonFiniteDoubleAttr(attrs, "arena_region_height_px", value)) {
        transform.arena_region_height_px = value;
    }

    double center_x = std::numeric_limits<double>::quiet_NaN();
    double center_y = std::numeric_limits<double>::quiet_NaN();
    double width = std::numeric_limits<double>::quiet_NaN();
    double height = std::numeric_limits<double>::quiet_NaN();
    stimulusJsonFiniteDoubleAttr(attrs, "arena_center_x_px", center_x);
    stimulusJsonFiniteDoubleAttr(attrs, "arena_center_y_px", center_y);
    stimulusJsonFiniteDoubleAttr(attrs, "arena_width_px", width);
    stimulusJsonFiniteDoubleAttr(attrs, "arena_height_px", height);
    if (std::isfinite(width)) {
        transform.arena_region_width_px = width;
    }
    if (std::isfinite(height)) {
        transform.arena_region_height_px = height;
    }
    if (std::isfinite(center_x) && std::isfinite(width) && width > 0.0) {
        transform.arena_origin_canvas_x_px = center_x - width * 0.5;
        transform.has_arena_canvas_origin = true;
    }
    if (std::isfinite(center_y) && std::isfinite(height) && height > 0.0) {
        transform.arena_origin_canvas_y_px = center_y - height * 0.5;
        transform.has_arena_canvas_origin = true;
    }
}

void applyChaserStateCoordinateMetadata(const json& attrs,
                                        ZarrDetectionData::ChaserCoordinateTransform& transform) {
    const std::string coordinate_frame = stimulusJsonStringAttr(attrs, "coordinate_frame");
    const std::string coordinate_origin = stimulusJsonStringAttr(attrs, "coordinate_origin");
    if (!coordinate_frame.empty()) {
        transform.coordinate_frame = coordinate_frame;
    }
    if (!coordinate_origin.empty()) {
        transform.coordinate_origin = coordinate_origin;
    }
}

void applyChaserCoordinateTransformToRecord(
    const ZarrDetectionData::ChaserCoordinateTransform& transform,
    ZarrDetectionData::ChaserStateRecord& record) {
    record.coordinate_frame = transform.coordinate_frame;
    record.coordinate_origin = transform.coordinate_origin;

    if (!transform.valid) {
        return;
    }

    if (chaserUsesArenaRelativeCanvas(transform)) {
        if (transform.has_arena_canvas_origin) {
            record.stimulus_canvas_offset_x = transform.arena_origin_canvas_x_px;
            record.stimulus_canvas_offset_y = transform.arena_origin_canvas_y_px;
            record.has_stimulus_canvas_offset = true;
        }
        return;
    }

    auto convert = [&](float tex_x, float tex_y, double& cam_x, double& cam_y) -> bool {
        if (!std::isfinite(tex_x) || !std::isfinite(tex_y)) {
            return false;
        }
        cam_x = transform.offset_x_px + static_cast<double>(tex_x) * transform.scale;
        cam_y = transform.offset_y_px + static_cast<double>(tex_y) * transform.scale;
        return std::isfinite(cam_x) && std::isfinite(cam_y);
    };
    if (!record.has_camera_coords &&
        convert(record.chaser_pos_x, record.chaser_pos_y,
                record.chaser_camera_x, record.chaser_camera_y)) {
        record.has_camera_coords = true;
    }
    double target_cam_x = std::numeric_limits<double>::quiet_NaN();
    double target_cam_y = std::numeric_limits<double>::quiet_NaN();
    if (convert(record.target_pos_x, record.target_pos_y,
                target_cam_x, target_cam_y)) {
        record.target_camera_x = target_cam_x;
        record.target_camera_y = target_cam_y;
    }
}

bool stimulusJsonFiniteFloatColorAttr(const json& attrs,
                                      const char* key,
                                      float& out_value) {
    double value = 0.0;
    if (!stimulusJsonFiniteDoubleAttr(attrs, key, value)) {
        return false;
    }
    out_value = std::clamp(static_cast<float>(value), 0.0f, 1.0f);
    return true;
}

std::optional<std::array<float, 4>> parseStimulusRgbaColor(
    const json& attrs,
    const char* prefix) {
    const std::string base(prefix);
    std::array<float, 4> rgba = {1.0f, 0.0f, 0.0f, 1.0f};
    if (!stimulusJsonFiniteFloatColorAttr(
            attrs, (base + "_r").c_str(), rgba[0]) ||
        !stimulusJsonFiniteFloatColorAttr(
            attrs, (base + "_g").c_str(), rgba[1]) ||
        !stimulusJsonFiniteFloatColorAttr(
            attrs, (base + "_b").c_str(), rgba[2])) {
        return std::nullopt;
    }
    float alpha = 1.0f;
    if (stimulusJsonFiniteFloatColorAttr(
            attrs, (base + "_a").c_str(), alpha)) {
        rgba[3] = alpha;
    }
    return rgba;
}

std::optional<json> parseJsonObjectString(const json& value) {
    if (value.is_object()) {
        return value;
    }
    if (!value.is_string()) {
        return std::nullopt;
    }
    try {
        json parsed = json::parse(value.get<std::string>());
        if (parsed.is_object()) {
            return parsed;
        }
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

void collectChaserColorsFromParameters(
    const json& params,
    std::unordered_map<int32_t, std::array<float, 4>>& colors_by_index) {
    if (!params.is_object() || !params.contains("chasers") ||
        !params["chasers"].is_array()) {
        return;
    }

    const auto& chasers = params["chasers"];
    for (size_t i = 0; i < chasers.size(); ++i) {
        const auto& chaser = chasers[i];
        if (!chaser.is_object()) {
            continue;
        }
        auto color = parseStimulusRgbaColor(chaser, "color");
        if (!color.has_value()) {
            continue;
        }
        int32_t chaser_index = stimulusJsonIntAttr(chaser, "chaser_index");
        if (chaser_index < 0) {
            chaser_index = stimulusJsonIntAttr(chaser, "index");
        }
        if (chaser_index < 0) {
            chaser_index = static_cast<int32_t>(i);
        }
        if (colors_by_index.find(chaser_index) == colors_by_index.end()) {
            colors_by_index.emplace(chaser_index, *color);
        }
    }
}

int32_t chaserIndexFromProtocolEntry(const json& chaser, size_t fallback_index) {
    int32_t chaser_index = stimulusJsonIntAttr(chaser, "chaser_index");
    if (chaser_index < 0) {
        chaser_index = stimulusJsonIntAttr(chaser, "index");
    }
    if (chaser_index < 0) {
        chaser_index = static_cast<int32_t>(fallback_index);
    }
    return chaser_index;
}

void collectChaserBehaviorFromParameters(
    const json& params,
    std::unordered_map<int32_t, ZarrDetectionData::ChaserBehaviorMetadata>& behavior_by_index) {
    if (!params.is_object() || !params.contains("chasers") ||
        !params["chasers"].is_array()) {
        return;
    }

    const auto& chasers = params["chasers"];
    for (size_t i = 0; i < chasers.size(); ++i) {
        const auto& chaser = chasers[i];
        if (!chaser.is_object()) {
            continue;
        }

        ZarrDetectionData::ChaserBehaviorMetadata metadata;
        const int32_t behavior_mode = stimulusJsonIntAttr(chaser, "behavior_mode");
        if (behavior_mode >= 0) {
            metadata.behavior_mode = behavior_mode;
            metadata.has_behavior_mode = true;
        }
        bool value = false;
        if (stimulusJsonBoolAttr(chaser, "enable_chase", value)) {
            metadata.enable_chase = value;
            metadata.has_enable_chase = true;
        }
        if (stimulusJsonBoolAttr(chaser, "enable_random_movement", value)) {
            metadata.enable_random_movement = value;
            metadata.has_enable_random_movement = true;
        }
        if (!metadata.has_behavior_mode && !metadata.has_enable_chase &&
            !metadata.has_enable_random_movement) {
            continue;
        }

        const int32_t chaser_index = chaserIndexFromProtocolEntry(chaser, i);
        if (behavior_by_index.find(chaser_index) == behavior_by_index.end()) {
            behavior_by_index.emplace(chaser_index, metadata);
        }
    }
}

void collectChaserColorsFromProtocolObject(
    const json& protocol,
    std::unordered_map<int32_t, std::array<float, 4>>& colors_by_index) {
    if (!protocol.is_object()) {
        return;
    }

    collectChaserColorsFromParameters(protocol, colors_by_index);

    if (protocol.contains("parameters")) {
        auto parameters = parseJsonObjectString(protocol["parameters"]);
        if (parameters.has_value()) {
            collectChaserColorsFromParameters(*parameters, colors_by_index);
        }
    }

    if (protocol.contains("steps") && protocol["steps"].is_array()) {
        for (const auto& step : protocol["steps"]) {
            collectChaserColorsFromProtocolObject(step, colors_by_index);
        }
    }
}

void collectChaserBehaviorFromProtocolObject(
    const json& protocol,
    std::unordered_map<int32_t, ZarrDetectionData::ChaserBehaviorMetadata>& behavior_by_index) {
    if (!protocol.is_object()) {
        return;
    }

    collectChaserBehaviorFromParameters(protocol, behavior_by_index);

    if (protocol.contains("parameters")) {
        auto parameters = parseJsonObjectString(protocol["parameters"]);
        if (parameters.has_value()) {
            collectChaserBehaviorFromParameters(*parameters, behavior_by_index);
        }
    }

    if (protocol.contains("steps") && protocol["steps"].is_array()) {
        for (const auto& step : protocol["steps"]) {
            collectChaserBehaviorFromProtocolObject(step, behavior_by_index);
        }
    }
}

void mergeChaserColorMetadata(
    const std::unordered_map<int32_t, std::array<float, 4>>& parsed_colors,
    ZarrDetectionData& data) {
    for (const auto& [chaser_index, color] : parsed_colors) {
        if (data.chaser_rgba_by_index.find(chaser_index) ==
            data.chaser_rgba_by_index.end()) {
            data.chaser_rgba_by_index.emplace(chaser_index, color);
        }
    }
    data.has_chaser_rgba_metadata = !data.chaser_rgba_by_index.empty();
}

void mergeChaserBehaviorMetadata(
    const std::unordered_map<int32_t, ZarrDetectionData::ChaserBehaviorMetadata>& parsed_behavior,
    ZarrDetectionData& data) {
    for (const auto& [chaser_index, behavior] : parsed_behavior) {
        if (data.chaser_behavior_by_index.find(chaser_index) ==
            data.chaser_behavior_by_index.end()) {
            data.chaser_behavior_by_index.emplace(chaser_index, behavior);
        }
    }
    data.has_chaser_behavior_metadata = !data.chaser_behavior_by_index.empty();
}

void appendChaserProtocolMetadataFromProtocolString(const std::string& protocol_json,
                                                    const char* source,
                                                    ZarrDetectionData& data) {
    if (protocol_json.empty()) {
        return;
    }

    try {
        const json protocol = json::parse(protocol_json);
        const size_t color_before = data.chaser_rgba_by_index.size();
        const size_t behavior_before = data.chaser_behavior_by_index.size();
        std::unordered_map<int32_t, std::array<float, 4>> parsed_colors;
        std::unordered_map<int32_t, ZarrDetectionData::ChaserBehaviorMetadata>
            parsed_behavior;
        collectChaserColorsFromProtocolObject(protocol, parsed_colors);
        collectChaserBehaviorFromProtocolObject(protocol, parsed_behavior);
        mergeChaserColorMetadata(parsed_colors, data);
        mergeChaserBehaviorMetadata(parsed_behavior, data);
        const size_t color_added = data.chaser_rgba_by_index.size() - color_before;
        const size_t behavior_added =
            data.chaser_behavior_by_index.size() - behavior_before;
        if (color_added > 0) {
            std::cout << "  [Chaser] Loaded protocol color metadata for "
                      << color_added << " chaser(s) from " << source << std::endl;
        }
        if (behavior_added > 0) {
            std::cout << "  [Chaser] Loaded protocol behavior metadata for "
                      << behavior_added << " chaser(s) from " << source << std::endl;
        }
    } catch (const std::exception& e) {
        std::cout << "  [Chaser] Failed to parse protocol chaser metadata from "
                  << source << ": " << e.what() << std::endl;
    }
}

void applyChaserColorMetadataToRecord(const ZarrDetectionData& data,
                                      ZarrDetectionData::ChaserStateRecord& record) {
    if (!data.has_chaser_rgba_metadata || record.chaser_index < 0) {
        return;
    }
    auto it = data.chaser_rgba_by_index.find(record.chaser_index);
    if (it == data.chaser_rgba_by_index.end()) {
        return;
    }
    record.chaser_rgba = it->second;
    record.has_chaser_rgba = true;
}

void applyChaserBehaviorMetadataToRecord(const ZarrDetectionData& data,
                                         ZarrDetectionData::ChaserStateRecord& record) {
    if (!data.has_chaser_behavior_metadata || record.chaser_index < 0) {
        return;
    }
    auto it = data.chaser_behavior_by_index.find(record.chaser_index);
    if (it == data.chaser_behavior_by_index.end()) {
        return;
    }
    const auto& behavior = it->second;
    record.behavior_mode = behavior.behavior_mode;
    record.has_behavior_mode = behavior.has_behavior_mode;
    record.enable_chase = behavior.enable_chase;
    record.has_enable_chase = behavior.has_enable_chase;
    record.enable_random_movement = behavior.enable_random_movement;
    record.has_enable_random_movement = behavior.has_enable_random_movement;
}

bool parseStimulusStepDirectoryName(const std::string& name,
                                    int32_t& out_step_index) {
    constexpr const char* kPrefix = "step_";
    constexpr size_t kPrefixLen = 5;
    if (name.rfind(kPrefix, 0) != 0 || name.size() <= kPrefixLen) {
        return false;
    }
    int64_t value = 0;
    for (size_t i = kPrefixLen; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (!std::isdigit(c)) {
            return false;
        }
        value = value * 10 + static_cast<int64_t>(name[i] - '0');
        if (value > std::numeric_limits<int32_t>::max()) {
            return false;
        }
    }
    out_step_index = static_cast<int32_t>(value);
    return true;
}

std::vector<int32_t> collectStimulusStepIndicesFs(
    const std::string& root_path,
    const std::string& run_name) {
    namespace fs = std::filesystem;
    std::vector<int32_t> indices;
    if (root_path.empty() || run_name.empty()) {
        return indices;
    }

    const fs::path steps_dir =
        fs::path(root_path) / "analysis" / "stimulus_runs" / run_name /
        "steps";
    if (!fs::exists(steps_dir) || !fs::is_directory(steps_dir)) {
        return indices;
    }

    for (const auto& entry : fs::directory_iterator(steps_dir)) {
        if (!entry.is_directory()) {
            continue;
        }
        if (!fs::exists(entry.path() / "zarr.json")) {
            continue;
        }
        int32_t step_index = -1;
        if (parseStimulusStepDirectoryName(
                entry.path().filename().string(), step_index)) {
            indices.push_back(step_index);
        }
    }
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    return indices;
}

}  // namespace

bool ZarrDetectionLoader::loadStimulusAlignment(const ts::kvstore::KvStore& store) {
    std::optional<std::string> latest_run_opt;
    if (!requested_stimulus_run_name_.empty()) {
        latest_run_opt = requested_stimulus_run_name_;
        std::cout << "  [Stimulus] Requested run candidate: '"
                  << *latest_run_opt << "'" << std::endl;
    } else if (auto group_attrs = readAttrsAny(store, "analysis/stimulus_runs")) {
        auto latest = extractLatestRunName(*group_attrs);
        if (!latest.empty()) {
            std::cout << "  [Stimulus] Attr latest run candidate: '" << latest << "'" << std::endl;
            latest_run_opt = latest;
        } else {
            std::cout << "  [Stimulus] No 'latest' pointer in analysis/stimulus_runs attrs" << std::endl;
        }
    } else {
        std::cout << "  [Stimulus] analysis/stimulus_runs attrs missing" << std::endl;
    }

    if (!latest_run_opt.has_value() && !root_path_.empty()) {
        auto candidates = collect_runs_fs(root_path_, "analysis/stimulus_runs", {});
        if (!candidates.empty()) {
            latest_run_opt = candidates.back();
            std::cout << "  [Stimulus] Falling back to filesystem run '" << *latest_run_opt
                      << "'" << std::endl;
        }
    }

    if (!latest_run_opt.has_value()) {
        return false;
    }
    const std::string& latest_run = *latest_run_opt;

    std::string run_base = "analysis/stimulus_runs/" + latest_run + "/";
    std::cout << "  Loading stimulus alignment run '" << latest_run << "'" << std::endl;
    data_.has_stimulus_alignment_data = false;
    data_.stimulus_camera_frame_offset = 0;
    data_.stimulus_video_path.clear();
    data_.stimulus_source_h5.clear();
    data_.chaser_transform = ZarrDetectionData::ChaserCoordinateTransform();
    data_.chaser_rgba_by_index.clear();
    data_.has_chaser_rgba_metadata = false;
    data_.chaser_behavior_by_index.clear();
    data_.has_chaser_behavior_metadata = false;

    std::string stimulus_created_at;
    int64_t camera_frame_offset = 0;
    std::vector<int32_t> camera_to_metadata_index;
    std::vector<uint8_t> metadata_mask;
    std::vector<int64_t> camera_to_stimulus_frame_corrected64;
    std::vector<uint8_t> camera_stimulus_frame_interpolated;
    std::optional<json> run_attrs = readAttrsAny(store, run_base);

    if (run_attrs.has_value()) {
        if (run_attrs->contains("created_at_utc") && (*run_attrs)["created_at_utc"].is_string()) {
            stimulus_created_at = (*run_attrs)["created_at_utc"].get<std::string>();
        } else if (run_attrs->contains("created_at") && (*run_attrs)["created_at"].is_string()) {
            stimulus_created_at = (*run_attrs)["created_at"].get<std::string>();
        }

        if (run_attrs->contains("source_stimulus_video_path") &&
            (*run_attrs)["source_stimulus_video_path"].is_string()) {
            data_.stimulus_video_path = (*run_attrs)["source_stimulus_video_path"].get<std::string>();
        }
        if (run_attrs->contains("source_h5") &&
            (*run_attrs)["source_h5"].is_string()) {
            data_.stimulus_source_h5 = (*run_attrs)["source_h5"].get<std::string>();
        }
        if (run_attrs->contains("protocol_json") &&
            (*run_attrs)["protocol_json"].is_string()) {
            appendChaserProtocolMetadataFromProtocolString(
                (*run_attrs)["protocol_json"].get<std::string>(),
                "run protocol_json",
                data_);
        }
    }

    auto parseCoordinateTransform = [&]() {
        bool saw_coordinate_metadata = false;
        try {
            if (run_attrs.has_value() &&
                run_attrs->contains("coordinate_transform") &&
                (*run_attrs)["coordinate_transform"].is_string()) {
                saw_coordinate_metadata = true;
                auto transform_json =
                    json::parse((*run_attrs)["coordinate_transform"].get<std::string>());
                if (transform_json.contains("texture_dimensions") &&
                    transform_json["texture_dimensions"].is_array()) {
                    const auto& dims = transform_json["texture_dimensions"];
                    if (dims.size() == 2) {
                        data_.chaser_transform.texture_width = dims[0].get<double>();
                        data_.chaser_transform.texture_height = dims[1].get<double>();
                    }
                }
                if (transform_json.contains("camera_dimensions") &&
                    transform_json["camera_dimensions"].is_array()) {
                    const auto& dims = transform_json["camera_dimensions"];
                    if (dims.size() == 2) {
                        data_.chaser_transform.camera_width = dims[0].get<double>();
                        data_.chaser_transform.camera_height = dims[1].get<double>();
                    }
                }
                if (transform_json.contains("texture_to_camera_scale") &&
                    transform_json["texture_to_camera_scale"].is_number()) {
                    data_.chaser_transform.scale =
                        transform_json["texture_to_camera_scale"].get<double>();
                }
                applyChaserStateCoordinateMetadata(transform_json, data_.chaser_transform);
            }

            if (run_attrs.has_value() &&
                run_attrs->contains("arena_config_json") &&
                (*run_attrs)["arena_config_json"].is_string()) {
                saw_coordinate_metadata = true;
                auto arena_json =
                    json::parse((*run_attrs)["arena_config_json"].get<std::string>());
                std::string active_camera_id;
                if (arena_json.is_object()) {
                    active_camera_id =
                        stimulusJsonScalarStringAttr(arena_json, "active_camera_id");
                }
                if (active_camera_id.empty() && run_attrs.has_value()) {
                    active_camera_id =
                        stimulusJsonScalarStringAttr(*run_attrs, "active_camera_id");
                }
                auto apply_arena_config_candidate = [&](const json& candidate) {
                    double value = 0.0;
                    if (stimulusJsonFiniteDoubleAttr(candidate, "sub_arena_x_px", value)) {
                        data_.chaser_transform.offset_x_px = value;
                    }
                    if (stimulusJsonFiniteDoubleAttr(candidate, "sub_arena_y_px", value)) {
                        data_.chaser_transform.offset_y_px = value;
                    }
                    if (stimulusJsonFiniteDoubleAttr(candidate, "sub_arena_width_px", value) &&
                        value > 0.0 && data_.chaser_transform.texture_width > 0.0) {
                        data_.chaser_transform.scale =
                            value / data_.chaser_transform.texture_width;
                    }
                    applyChaserArenaConfigMetadata(candidate, data_.chaser_transform);
                };
                if (arena_json.is_object()) {
                    apply_arena_config_candidate(arena_json);
                    if (arena_json.contains("camera_calibrations") &&
                        arena_json["camera_calibrations"].is_array()) {
                        for (const auto& candidate : arena_json["camera_calibrations"]) {
                            if (!candidate.is_object()) {
                                continue;
                            }
                            const std::string candidate_camera_id =
                                stimulusJsonScalarStringAttr(candidate, "camera_id");
                            if (!active_camera_id.empty() &&
                                !candidate_camera_id.empty() &&
                                candidate_camera_id != active_camera_id) {
                                continue;
                            }
                            apply_arena_config_candidate(candidate);
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cout << "  [Chaser] Failed to parse coordinate_transform metadata: "
                      << e.what() << std::endl;
        }

        if (auto chaser_attrs =
                readAttrsAny(store, run_base + "tracking_data/chaser_states")) {
            saw_coordinate_metadata = true;
            applyChaserStateCoordinateMetadata(*chaser_attrs, data_.chaser_transform);
        } else if (auto interpolated_attrs =
                       readAttrsAny(store, run_base + "tracking_data/chaser_states_interpolated")) {
            saw_coordinate_metadata = true;
            applyChaserStateCoordinateMetadata(*interpolated_attrs, data_.chaser_transform);
        }

        if (auto arena_geometry_attrs =
                readAttrsAny(store, run_base + "calibration/arena_geometry")) {
            saw_coordinate_metadata = true;
            applyChaserStateCoordinateMetadata(*arena_geometry_attrs, data_.chaser_transform);
            applyChaserArenaConfigMetadata(*arena_geometry_attrs, data_.chaser_transform);
        }

        if (data_.chaser_transform.texture_width <= 0.0) {
            data_.chaser_transform.texture_width = 358.0;
        }
        if (data_.chaser_transform.texture_height <= 0.0) {
            data_.chaser_transform.texture_height = 358.0;
        }
        if (data_.chaser_transform.scale <= 0.0) {
            if (data_.chaser_transform.camera_width > 0.0) {
                data_.chaser_transform.scale =
                    data_.chaser_transform.camera_width / data_.chaser_transform.texture_width;
            } else {
                data_.chaser_transform.scale = 1.0;
            }
        }
        data_.chaser_transform.valid =
            saw_coordinate_metadata ||
            !data_.chaser_transform.coordinate_frame.empty() ||
            data_.chaser_transform.has_arena_canvas_origin;

        if (data_.chaser_transform.valid) {
            std::cout << "  [Chaser] Coordinate metadata frame='"
                      << data_.chaser_transform.coordinate_frame
                      << "' origin='" << data_.chaser_transform.coordinate_origin
                      << "' texture=" << data_.chaser_transform.texture_width
                      << "x" << data_.chaser_transform.texture_height
                      << " scale=" << data_.chaser_transform.scale;
            if (data_.chaser_transform.has_arena_canvas_origin) {
                std::cout << " arena_origin_canvas=("
                          << data_.chaser_transform.arena_origin_canvas_x_px
                          << "," << data_.chaser_transform.arena_origin_canvas_y_px
                          << ")";
            }
            std::cout << std::endl;
        }
    };

    parseCoordinateTransform();

    loadStimulusStepsForRun(store, run_base, latest_run);

    // Always attempt to load stimulus event metadata, even if frame alignment
    // data is missing. This keeps the event timeline available in the UI.
    loadStimulusEventEnums(store);
    bool events_loaded = loadStimulusEventsForRun(store, run_base);
    if (!events_loaded) {
        std::cout << "  Stimulus run '" << latest_run
                  << "' does not contain events metadata." << std::endl;
    } else {
        std::cout << "  Stimulus run '" << latest_run << "' events metadata loaded" << std::endl;
    }

    if (!loadChaserStates(store, run_base)) {
        std::cout << "  [Chaser] No chaser tracking data for run '" << latest_run << "'" << std::endl;
    }
    if (!loadChaserBoundingBoxes(store, run_base)) {
        std::cout << "  [ChaserBBox] No tracking bounding boxes for run '" << latest_run << "'" << std::endl;
    }
    if (!loadChaserStatesInterpolated(store, run_base)) {
        std::cout << "  [ChaserInterp] No interpolated chaser data for run '" << latest_run << "'" << std::endl;
    }

    if (auto align_attrs = readAttrsAny(store, run_base + "frame_alignment")) {
        if (align_attrs->contains("camera_frame_offset") &&
            (*align_attrs)["camera_frame_offset"].is_number_integer()) {
            camera_frame_offset = (*align_attrs)["camera_frame_offset"].get<int64_t>();
        }
    }

    std::vector<uint8_t> camera_mask;
    bool has_camera_mask = readBoolArray(
        store,
        run_base + "frame_alignment/camera_interpolation_mask",
        camera_mask
    );

    std::vector<int64_t> camera_to_metadata_raw;
    bool has_camera_mapping = readInt64Array(
        store,
        run_base + "frame_alignment/camera_to_metadata_index",
        camera_to_metadata_raw
    );

    std::vector<int64_t> camera_to_metadata_corrected_raw;
    bool has_camera_mapping_corrected = readInt64Array(
        store,
        run_base + "frame_alignment/camera_to_metadata_index_corrected",
        camera_to_metadata_corrected_raw
    );

    bool has_metadata_mask = readBoolArray(
        store,
        run_base + "interpolation_mask",
        metadata_mask
    );

    bool has_direct_stimulus_lookup = readInt64Array(
        store,
        run_base + "frame_alignment/camera_to_stimulus_frame_corrected",
        camera_to_stimulus_frame_corrected64
    );
    bool has_camera_stimulus_interp_mask = readBoolArray(
        store,
        run_base + "frame_alignment/camera_stimulus_frame_interpolated",
        camera_stimulus_frame_interpolated
    );

    if (!has_camera_mask || camera_mask.empty()) {
        std::cout << "  Stimulus run '" << latest_run
                  << "' missing camera_interpolation_mask data" << std::endl;
        return false;
    }

    if (!has_camera_mapping || camera_to_metadata_raw.empty()) {
        std::cout << "  Stimulus run '" << latest_run
                  << "' missing camera_to_metadata_index data" << std::endl;
        return false;
    }

    if (has_camera_mapping) {
        camera_to_metadata_index.reserve(camera_to_metadata_raw.size());
        for (auto value : camera_to_metadata_raw) {
            camera_to_metadata_index.push_back(static_cast<int32_t>(value));
        }
    }

    std::vector<int32_t> camera_to_metadata_index_corrected;
    if (has_camera_mapping_corrected && !camera_to_metadata_corrected_raw.empty()) {
        camera_to_metadata_index_corrected.reserve(camera_to_metadata_corrected_raw.size());
        for (auto value : camera_to_metadata_corrected_raw) {
            camera_to_metadata_index_corrected.push_back(static_cast<int32_t>(value));
        }
        std::cout << "  Stimulus alignment '" << latest_run
                  << "' includes corrected camera_to_metadata_index" << std::endl;
    } else if (has_camera_mapping_corrected) {
        std::cout << "  Stimulus run '" << latest_run
                  << "' has empty camera_to_metadata_index_corrected" << std::endl;
    } else {
        std::cout << "  Stimulus run '" << latest_run
                  << "' does not include camera_to_metadata_index_corrected (using legacy order)"
                  << std::endl;
    }

    int32_t first_camera_frame = -1;
    int32_t first_metadata_index = -1;
    for (size_t i = 0; i < camera_to_metadata_index.size(); ++i) {
        int32_t meta_index = camera_to_metadata_index[i];
        if (meta_index >= 0) {
            first_camera_frame = static_cast<int32_t>(i);
            first_metadata_index = meta_index;
            break;
        }
    }

    int32_t first_camera_frame_corrected = -1;
    int32_t first_metadata_index_corrected = -1;
    for (size_t i = 0; i < camera_to_metadata_index_corrected.size(); ++i) {
        int32_t meta_index = camera_to_metadata_index_corrected[i];
        if (meta_index >= 0) {
            first_camera_frame_corrected = static_cast<int32_t>(i);
            first_metadata_index_corrected = meta_index;
            break;
        }
    }

    size_t frame_count = camera_mask.size();
    std::vector<uint8_t> stored_mask = camera_mask;

    if (frame_count == 0) {
        std::cout << "  Stimulus alignment has zero frames" << std::endl;
        return false;
    }

    std::vector<uint8_t> new_mask(frame_count, 0);
    for (size_t i = 0; i < frame_count; ++i) {
        bool original = camera_mask[i] != 0;
        new_mask[i] = original ? 0 : 1;
    }

    std::vector<int32_t> stimulus_frame_numbers;
    bool metadata_frames_loaded = readInt32Array(
        store,
        run_base + "video_metadata/frame_metadata/stimulus_frame_num",
        stimulus_frame_numbers
    );
    if (!metadata_frames_loaded) {
        std::cout << "  Stimulus alignment '" << latest_run
                  << "' missing video_metadata/frame_metadata/stimulus_frame_num data" << std::endl;
    }

    std::vector<int32_t> stimulus_frame_numbers_corrected;
    bool metadata_frames_corrected_loaded = readInt32Array(
        store,
        run_base + "video_metadata/frame_metadata/stimulus_frame_num_corrected",
        stimulus_frame_numbers_corrected
    );
    if (metadata_frames_corrected_loaded) {
        std::cout << "  Stimulus alignment '" << latest_run
                  << "' includes stimulus_frame_num_corrected" << std::endl;
    } else {
        std::cout << "  Stimulus alignment '" << latest_run
                  << "' missing video_metadata/frame_metadata/stimulus_frame_num_corrected data"
                  << std::endl;
    }

    auto& interp = data_.latest_interpolation;
    if (interp.frame_mask.size() < frame_count) {
        interp.frame_mask.resize(frame_count, 0);
    }
    for (size_t i = 0; i < frame_count; ++i) {
        if (i < interp.frame_mask.size()) {
            interp.frame_mask[i] = std::max(interp.frame_mask[i], new_mask[i]);
        }
    }

    interp.stimulus_run_name = latest_run;
    interp.stimulus_created_at = stimulus_created_at;
    interp.camera_frame_offset = camera_frame_offset;
    interp.camera_to_metadata_index = std::move(camera_to_metadata_index);
    interp.first_camera_frame_with_stimulus = first_camera_frame;
    interp.first_metadata_index_with_stimulus = first_metadata_index;
    if (!camera_to_metadata_index_corrected.empty()) {
        interp.camera_to_metadata_index_corrected = std::move(camera_to_metadata_index_corrected);
        interp.first_camera_frame_with_stimulus_corrected = first_camera_frame_corrected;
        interp.first_metadata_index_with_stimulus_corrected = first_metadata_index_corrected;
    } else {
        interp.camera_to_metadata_index_corrected.clear();
        interp.first_camera_frame_with_stimulus_corrected = -1;
        interp.first_metadata_index_with_stimulus_corrected = -1;
    }
    if (metadata_frames_loaded) {
        interp.frame_metadata_stimulus_frames = std::move(stimulus_frame_numbers);
        interp.frame_metadata_loaded = true;
        if (first_metadata_index >= 0 &&
            static_cast<size_t>(first_metadata_index) < interp.frame_metadata_stimulus_frames.size()) {
            interp.first_stimulus_frame =
                interp.frame_metadata_stimulus_frames[static_cast<size_t>(first_metadata_index)];
        } else {
            interp.first_stimulus_frame = -1;
        }
    } else {
        interp.frame_metadata_stimulus_frames.clear();
        interp.frame_metadata_loaded = false;
        interp.first_stimulus_frame = -1;
    }
    if (metadata_frames_corrected_loaded) {
        interp.frame_metadata_stimulus_frames_corrected = std::move(stimulus_frame_numbers_corrected);
        interp.frame_metadata_corrected_loaded = true;
        if (first_metadata_index_corrected >= 0 &&
            static_cast<size_t>(first_metadata_index_corrected) <
                interp.frame_metadata_stimulus_frames_corrected.size()) {
            interp.first_stimulus_frame_corrected =
                interp.frame_metadata_stimulus_frames_corrected[
                    static_cast<size_t>(first_metadata_index_corrected)];
        } else {
            interp.first_stimulus_frame_corrected = -1;
        }
    } else {
        interp.frame_metadata_stimulus_frames_corrected.clear();
        interp.frame_metadata_corrected_loaded = false;
        interp.first_stimulus_frame_corrected = -1;
    }
    if (has_metadata_mask) {
        stored_mask = metadata_mask;
    }
    if (has_direct_stimulus_lookup && !camera_to_stimulus_frame_corrected64.empty()) {
        interp.camera_to_stimulus_frame_corrected.resize(camera_to_stimulus_frame_corrected64.size());
        for (size_t i = 0; i < camera_to_stimulus_frame_corrected64.size(); ++i) {
            interp.camera_to_stimulus_frame_corrected[i] =
                clampToInt32(camera_to_stimulus_frame_corrected64[i]);
        }
        interp.has_direct_stimulus_lookup = true;
        if (!camera_stimulus_frame_interpolated.empty()) {
            if (camera_stimulus_frame_interpolated.size() < interp.camera_to_stimulus_frame_corrected.size()) {
                camera_stimulus_frame_interpolated.resize(interp.camera_to_stimulus_frame_corrected.size(), 0);
            }
            interp.camera_stimulus_frame_interpolated = std::move(camera_stimulus_frame_interpolated);
        } else {
            interp.camera_stimulus_frame_interpolated.clear();
        }
        if (interp.first_camera_frame_with_stimulus_corrected < 0 &&
            !interp.camera_to_stimulus_frame_corrected.empty()) {
            for (size_t i = 0; i < interp.camera_to_stimulus_frame_corrected.size(); ++i) {
                if (interp.camera_to_stimulus_frame_corrected[i] >= 0) {
                    interp.first_camera_frame_with_stimulus_corrected = static_cast<int32_t>(i);
                    interp.first_stimulus_frame_corrected = interp.camera_to_stimulus_frame_corrected[i];
                    break;
                }
            }
        }
        std::cout << "  Stimulus alignment '" << latest_run
                  << "' provides direct camera_to_stimulus_frame_corrected array (" 
                  << interp.camera_to_stimulus_frame_corrected.size() << " entries)"
                  << std::endl;
    } else {
        interp.camera_to_stimulus_frame_corrected.clear();
        interp.camera_stimulus_frame_interpolated.clear();
        interp.has_direct_stimulus_lookup = false;
        if (!has_direct_stimulus_lookup) {
            std::cout << "  Stimulus run '" << latest_run
                      << "' missing camera_to_stimulus_frame_corrected array (falling back to metadata mapping)"
                      << std::endl;
        } else {
            std::cout << "  Stimulus run '" << latest_run
                      << "' camera_to_stimulus_frame_corrected array empty" << std::endl;
        }
    }
    interp.stimulus_interpolation_mask = std::move(stored_mask);
   interp.has_stimulus_alignment = true;
   data_.has_interpolation = true;
   data_.has_stimulus_alignment_data = true;
   data_.stimulus_camera_frame_offset = camera_frame_offset;
   std::cout << "  Stimulus alignment '" << latest_run
             << "' loaded with frame mask for " << frame_count << " frames" << std::endl;

    updateChaserCameraFramesFromAlignment();

   return true;
}

bool ZarrDetectionLoader::loadStimulusStepsForRun(
    const ts::kvstore::KvStore& store,
    const std::string& run_base,
    const std::string& run_name) {
    data_.stimulus_steps.clear();
    data_.has_stimulus_steps = false;
    data_.stimulus_steps_run_name.clear();

    if (run_name.empty()) {
        return false;
    }

    const std::vector<int32_t> step_indices =
        collectStimulusStepIndicesFs(root_path_, run_name);
    if (step_indices.empty()) {
        std::cout << "  [StimulusSteps] No canonical step groups found for run '"
                  << run_name << "'" << std::endl;
        return false;
    }

    auto read_step_attrs = [&](const std::string& relative_path)
        -> std::optional<json> {
        if (!root_path_.empty()) {
            const auto zarr_json_path =
                std::filesystem::path(root_path_) / relative_path /
                "zarr.json";
            if (!std::filesystem::exists(zarr_json_path)) {
                return std::nullopt;
            }
            return readAttrsFromZarrJsonFile(zarr_json_path);
        }
        return readAttrsAny(store, relative_path);
    };

    std::vector<ZarrDetectionData::StimulusStep> steps;
    steps.reserve(step_indices.size());

    for (const int32_t discovered_step_index : step_indices) {
        const std::string step_name =
            "step_" + std::to_string(discovered_step_index);
        const std::string step_base =
            appendPath(appendPath(run_base, "steps"), step_name);
        auto step_attrs = read_step_attrs(step_base);
        if (!step_attrs) {
            continue;
        }

        ZarrDetectionData::StimulusStep step;
        step.step_index =
            stimulusJsonIntAttr(*step_attrs, "step_index");
        if (step.step_index < 0) {
            step.step_index = discovered_step_index;
        }
        step.step_name = stimulusJsonStringAttr(*step_attrs, "step_name");
        step.stimulus_mode_id =
            stimulusJsonIntAttr(*step_attrs, "stimulus_mode_id");
        step.stimulus_mode =
            stimulusJsonStringAttr(*step_attrs, "stimulus_mode");
        step.start_camera_frame =
            stimulusJsonIntAttr(*step_attrs, "start_camera_frame");
        step.end_camera_frame =
            stimulusJsonIntAttr(*step_attrs, "end_camera_frame");
        step.duration_s = stimulusJsonDoubleAttr(*step_attrs, "duration_s");
        step.raw_protocol_params_json =
            stimulusJsonStringAttr(*step_attrs, "raw_protocol_params_json");
        appendChaserProtocolMetadataFromProtocolString(
            step.raw_protocol_params_json,
            "step raw_protocol_params_json",
            data_);

        if (auto moving_attrs =
                read_step_attrs(appendPath(step_base, "moving_grating"))) {
            auto& moving = step.moving_grating;
            moving.present = true;
            moving.grating_direction_camera_deg = stimulusJsonDoubleAttr(
                *moving_attrs, "grating_direction_camera_deg");
            moving.orientation_degrees_authored = stimulusJsonDoubleAttr(
                *moving_attrs, "orientation_degrees_authored");
            moving.camera_to_projector_offset_deg = stimulusJsonDoubleAttr(
                *moving_attrs, "camera_to_projector_offset_deg");
            moving.direction_mapping_status = stimulusJsonStringAttr(
                *moving_attrs, "direction_mapping_status");
            moving.has_direction_mapping_validated = stimulusJsonBoolAttr(
                *moving_attrs,
                "direction_mapping_validated",
                moving.direction_mapping_validated);
            moving.speed_mm_s =
                stimulusJsonDoubleAttr(*moving_attrs, "speed_mm_s");
            moving.temporal_frequency_hz = stimulusJsonDoubleAttr(
                *moving_attrs, "temporal_frequency_hz");
        }

        if (auto concentric_attrs =
                read_step_attrs(appendPath(step_base, "concentric_grating"))) {
            auto& concentric = step.concentric_grating;
            concentric.present = true;
            concentric.stimulus_role = stimulusJsonStringAttr(
                *concentric_attrs, "stimulus_role");
            concentric.radial_polarity_authored = stimulusJsonStringAttr(
                *concentric_attrs, "radial_polarity_authored");
            concentric.radial_sign_authored = stimulusJsonDoubleAttr(
                *concentric_attrs, "radial_sign_authored");
            concentric.has_radial_polarity_validated = stimulusJsonBoolAttr(
                *concentric_attrs,
                "radial_polarity_validated",
                concentric.radial_polarity_validated);
            concentric.center_x_px =
                stimulusJsonDoubleAttr(*concentric_attrs, "center_x_px");
            concentric.center_y_px =
                stimulusJsonDoubleAttr(*concentric_attrs, "center_y_px");
            concentric.center_x_mm =
                stimulusJsonDoubleAttr(*concentric_attrs, "center_x_mm");
            concentric.center_y_mm =
                stimulusJsonDoubleAttr(*concentric_attrs, "center_y_mm");
            concentric.target_radius_min_mm = stimulusJsonDoubleAttr(
                *concentric_attrs, "target_radius_min_mm");
            concentric.target_radius_max_mm = stimulusJsonDoubleAttr(
                *concentric_attrs, "target_radius_max_mm");
            concentric.speed_mm_s =
                stimulusJsonDoubleAttr(*concentric_attrs, "speed_mm_s");
            concentric.temporal_frequency_hz = stimulusJsonDoubleAttr(
                *concentric_attrs, "temporal_frequency_hz");
        }

        steps.push_back(std::move(step));
    }

    std::sort(steps.begin(),
              steps.end(),
              [](const auto& a, const auto& b) {
                  if (a.start_camera_frame != b.start_camera_frame) {
                      return a.start_camera_frame < b.start_camera_frame;
                  }
                  return a.step_index < b.step_index;
              });

    data_.stimulus_steps = std::move(steps);
    data_.has_stimulus_steps = !data_.stimulus_steps.empty();
    if (data_.has_stimulus_steps) {
        data_.stimulus_steps_run_name = run_name;
        std::cout << "  [StimulusSteps] Loaded "
                  << data_.stimulus_steps.size()
                  << " canonical steps for run '" << run_name << "'"
                  << std::endl;
    }
    return data_.has_stimulus_steps;
}

void ZarrDetectionLoader::loadStimulusEventEnums(const ts::kvstore::KvStore& store) {
    if (!data_.event_type_names.empty()) {
        return;
    }

    const std::vector<std::pair<std::string, std::string>> candidates = {
        {"analysis/enums/events/event_type_id", "analysis/enums/events/name"},
        {"analysis/enums/events/event_type_id", "analysis/enums/events/value"},
        {"analysis/enums/events/id", "analysis/enums/events/name"},
        {"analysis/enums/events/id", "analysis/enums/events/value"},
        {"analysis/enums/events/ids", "analysis/enums/events/names"}
    };

    for (const auto& candidate : candidates) {
        std::vector<int32_t> ids32;
        if (!readInt32Array(store, candidate.first, ids32)) {
            std::vector<int64_t> ids64;
            if (!readInt64Array(store, candidate.first, ids64)) {
                continue;
            }
            ids32.resize(ids64.size());
            for (size_t i = 0; i < ids64.size(); ++i) {
                ids32[i] = static_cast<int32_t>(ids64[i]);
            }
        }

        std::vector<std::string> names;
        if (!readStringArray(store, candidate.second, names)) {
            continue;
        }

        size_t count = std::min(ids32.size(), names.size());
        if (count == 0) {
            continue;
        }

        data_.event_type_names.clear();
        for (size_t i = 0; i < count; ++i) {
            data_.event_type_names[ids32[i]] = names[i];
        }
        std::cout << "  Loaded " << data_.event_type_names.size()
                  << " stimulus event type labels" << std::endl;
        return;
    }

    std::cout << "  [Stimulus] No stimulus event enum labels loaded from known paths" << std::endl;
}

bool ZarrDetectionLoader::loadStimulusEventsForRun(const ts::kvstore::KvStore& store,
                                                   const std::string& run_base) {
    std::string events_base = run_base + "events/";
    bool has_column_layout = arrayExists(store, events_base + "stimulus_frame_num");
    bool has_structured_layout = arrayExists(store, run_base + "events");
    clearStimulusEventTimelineCache();

    std::cout << "  [StimulusEvents] Inspecting run '" << run_base << "'" << std::endl;
    std::cout << "    events_base='" << events_base << "'" << std::endl;
    std::cout << "    column layout present: " << (has_column_layout ? "yes" : "no")
              << ", structured layout present: " << (has_structured_layout ? "yes" : "no")
              << std::endl;

    if ((!has_column_layout || !has_structured_layout) && !root_path_.empty()) {
        namespace fs = std::filesystem;
        fs::path root(root_path_);
        if (!has_column_layout) {
            fs::path column_probe =
                root / fs::path(events_base) / "stimulus_frame_num" / "zarr.json";
            if (fs::exists(column_probe)) {
                has_column_layout = true;
                std::cout << "    column layout detected via filesystem probe at "
                          << column_probe << std::endl;
            }
        }
        if (!has_structured_layout) {
            fs::path structured_probe =
                root / fs::path(run_base) / "events" / "zarr.json";
            if (fs::exists(structured_probe)) {
                has_structured_layout = true;
                std::cout << "    structured layout detected via filesystem probe at "
                          << structured_probe << std::endl;
            }
        }
    }

    if (!has_column_layout && !has_structured_layout) {
        std::cout << "  [Stimulus] No events dataset at " << events_base << " or "
                  << run_base << "events" << std::endl;
    } else if (has_column_layout && has_structured_layout) {
        std::cout << "  [Stimulus] Events available in both column and structured layouts; "
                     "using column arrays."
                  << std::endl;
    } else if (has_structured_layout) {
        std::cout << "  [Stimulus] Events stored as structured array." << std::endl;
    } else if (has_column_layout) {
        std::cout << "  [Stimulus] Events stored as column arrays." << std::endl;
    }

    std::vector<int32_t> stimulus_to_camera_map;
    bool has_frame_mapping =
        loadStimulusFrameMetadataMapping(store, run_base, stimulus_to_camera_map) &&
        !stimulus_to_camera_map.empty();

    auto finalize_events = [&](size_t max_frame) {
        std::cout << "  [StimulusEvents] Finalizing events (max_frame=" << max_frame << ")"
                  << std::endl;
        size_t size_needed = std::max<size_t>(data_.total_frames,
                                              max_frame + 1);
        if (data_.stimulus_events_by_frame.size() < size_needed) {
            data_.stimulus_events_by_frame.resize(size_needed);
        }
        for (auto& vec : data_.stimulus_events_by_frame) {
            vec.clear();
        }
        if (data_.stimulus_events_by_camera_frame.size() < data_.total_frames) {
            data_.stimulus_events_by_camera_frame.resize(data_.total_frames);
        }
        for (auto& vec : data_.stimulus_events_by_camera_frame) {
            vec.clear();
        }
        for (size_t idx = 0; idx < data_.stimulus_events.size(); ++idx) {
            int32_t frame = data_.stimulus_events[idx].stimulus_frame_num;
            if (frame < 0) continue;
            size_t frame_index = static_cast<size_t>(frame);
            if (frame_index >= data_.stimulus_events_by_frame.size()) {
                data_.stimulus_events_by_frame.resize(frame_index + 1);
            }
            data_.stimulus_events_by_frame[frame_index].push_back(idx);

            int32_t camera_frame = data_.stimulus_events[idx].camera_frame_id;
            if (camera_frame < 0 && has_frame_mapping &&
                frame_index < stimulus_to_camera_map.size()) {
                camera_frame = stimulus_to_camera_map[frame_index];
            }
            if (camera_frame >= 0) {
                size_t camera_index = static_cast<size_t>(camera_frame);
                if (camera_index >= data_.stimulus_events_by_camera_frame.size()) {
                    data_.stimulus_events_by_camera_frame.resize(camera_index + 1);
                }
                data_.stimulus_events_by_camera_frame[camera_index].push_back(idx);
            }
        }

        data_.has_stimulus_events = !data_.stimulus_events.empty();
        if (data_.has_stimulus_events) {
            std::cout << "  Loaded " << data_.stimulus_events.size()
                      << " stimulus events" << std::endl;
            const auto& sample = data_.stimulus_events.front();
            std::cout << "  [StimulusEvents] Sample entry: stimulus_frame="
                      << sample.stimulus_frame_num << ", camera_frame="
                      << sample.camera_frame_id << ", event_type="
                      << sample.event_type_id << ", name='"
                      << sample.name_or_context << "'" << std::endl;
            size_t missing_camera = 0;
            for (const auto& entry : data_.stimulus_events) {
                if (entry.camera_frame_id < 0) {
                    ++missing_camera;
                }
            }
            if (missing_camera > 0) {
                std::cout << "  [StimulusEvents] Entries lacking camera_frame_id: "
                          << missing_camera << std::endl;
            }
        } else {
            std::cout << "  No stimulus events found for run '" << run_base
                      << "'" << std::endl;
        }
        rebuildStimulusEventTimelineCache(has_frame_mapping
                                              ? &stimulus_to_camera_map
                                              : nullptr);
        return data_.has_stimulus_events;
    };

    if (has_column_layout) {
        auto readInt32Or64 = [&](const std::string& path, std::vector<int32_t>& dest) -> bool {
            std::cout << "  [StimulusEvents] Reading column '" << path << "'" << std::endl;
            if (readInt32Array(store, path, dest)) {
                return true;
            }
            std::cout << "  [StimulusEvents] Falling back to int64_t reader for '" << path
                      << "'" << std::endl;
            std::vector<int64_t> tmp64;
            if (!readInt64Array(store, path, tmp64)) {
                std::cout << "  [StimulusEvents] Failed to read '" << path
                          << "' as int64_t" << std::endl;
                return false;
            }
            dest.resize(tmp64.size());
            bool overflow = false;
            for (size_t i = 0; i < tmp64.size(); ++i) {
                int32_t converted = clampToInt32(tmp64[i]);
                if (converted != static_cast<int32_t>(tmp64[i])) {
                    overflow = true;
                }
                dest[i] = converted;
            }
            if (overflow) {
                std::cout << "  [StimulusEvents] '" << path
                          << "' required clamp during int64->int32 conversion" << std::endl;
            }
            return true;
        };

        std::vector<int32_t> stimulus_frames;
        if (!readInt32Or64(events_base + "stimulus_frame_num", stimulus_frames)) {
            return false;
        }
        size_t count = stimulus_frames.size();
        if (count == 0) {
            std::cout << "  [StimulusEvents] Column layout contained zero rows" << std::endl;
            data_.stimulus_events.clear();
            data_.stimulus_events_by_frame.clear();
            data_.stimulus_events_by_camera_frame.clear();
            data_.has_stimulus_events = false;
            clearStimulusEventTimelineCache();
            return true;
        }

        std::vector<int32_t> camera_frames;
        if (!readInt32Or64(events_base + "camera_frame_id", camera_frames)) {
            std::cout << "  [StimulusEvents] camera_frame_id missing; defaulting to -1"
                      << std::endl;
            camera_frames.assign(count, -1);
        }

        std::vector<int32_t> event_type_ids;
        if (!readInt32Or64(events_base + "event_type_id", event_type_ids)) {
            std::cout << "  [StimulusEvents] event_type_id missing; defaulting to -1"
                      << std::endl;
            event_type_ids.assign(count, -1);
        }

        std::vector<int64_t> timestamps;
        if (!readInt64Array(store, events_base + "timestamp_ns_session", timestamps)) {
            std::cout << "  [StimulusEvents] timestamp_ns_session missing; defaulting to 0"
                      << std::endl;
            timestamps.assign(count, 0);
        }
        if (timestamps.size() != count) {
            timestamps.resize(count, 0);
        }

        std::vector<std::string> names;
        if (!readStringArray(store, events_base + "name_or_context", names)) {
            std::cout << "  [StimulusEvents] name_or_context missing; defaulting to empty string"
                      << std::endl;
            names.assign(count, std::string());
        }

        std::vector<std::string> details;
        if (!readStringArray(store, events_base + "details_json", details)) {
            std::cout << "  [StimulusEvents] details_json missing; defaulting to empty string"
                      << std::endl;
            details.assign(count, std::string());
        }

        auto sync_size = [&](auto& vec, const auto& default_value) {
            if (vec.size() != count) {
                vec.resize(count, default_value);
            }
        };
        sync_size(camera_frames, int32_t{-1});
        sync_size(event_type_ids, int32_t{-1});
        sync_size(names, std::string());
        sync_size(details, std::string());

        data_.stimulus_events.clear();
        data_.stimulus_events.reserve(count);

        size_t max_frame = 0;
        for (size_t i = 0; i < count; ++i) {
            ZarrDetectionData::EventLogEntry entry;
            entry.stimulus_frame_num = stimulus_frames[i];
            entry.camera_frame_id = (i < camera_frames.size()) ? camera_frames[i] : -1;
            entry.timestamp_ns_session = (i < timestamps.size()) ? timestamps[i] : 0;
            entry.event_type_id = (i < event_type_ids.size()) ? event_type_ids[i] : -1;
            if (i < names.size()) entry.name_or_context = names[i];
            if (i < details.size()) entry.details_json = details[i];
            if (entry.stimulus_frame_num >= 0) {
                max_frame = std::max(max_frame,
                                     static_cast<size_t>(entry.stimulus_frame_num));
            }
            data_.stimulus_events.push_back(std::move(entry));
        }
        std::cout << "  [StimulusEvents] Column layout produced "
                  << data_.stimulus_events.size() << " entries" << std::endl;
        return finalize_events(max_frame);
    }

    if (!has_structured_layout) {
        data_.stimulus_events.clear();
        data_.stimulus_events_by_frame.clear();
        data_.stimulus_events_by_camera_frame.clear();
        data_.has_stimulus_events = false;
        clearStimulusEventTimelineCache();
        std::cout << "  [StimulusEvents] Structured layout absent and column layout failed"
                  << std::endl;
        return false;
    }

    auto open_result = openArrayAny<StimulusEventRowV3, 1>(
        store, run_base + "events", context_);
    if (!open_result.ok()) {
        std::cerr << "Failed to open structured stimulus events array: "
                  << open_result.status() << std::endl;
        return false;
    }

    auto read_result = ts::Read(open_result.value()).result();
    if (!read_result.ok()) {
        std::cerr << "Failed to read structured stimulus events array: "
                  << read_result.status() << std::endl;
        return false;
    }

    auto array = read_result.value();
    if (array.rank() != 1) {
        std::cerr << "Structured stimulus events array has unexpected rank "
                  << array.rank() << std::endl;
        return false;
    }

    size_t count = static_cast<size_t>(array.shape()[0]);
    data_.stimulus_events.clear();
    data_.stimulus_events.reserve(count);
    std::cout << "  [StimulusEvents] Structured layout contains " << count << " rows"
              << std::endl;

    const auto* rows = static_cast<const StimulusEventRowV3*>(array.data());
    size_t max_frame = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto& row = rows[i];
        ZarrDetectionData::EventLogEntry entry;
        entry.stimulus_frame_num = clampUint64ToInt32(row.stimulus_frame_num);
        entry.camera_frame_id = clampUint64ToInt32(row.camera_frame_id);
        entry.timestamp_ns_session = row.timestamp_ns_session;
        entry.event_type_id = clampToInt32(row.event_type_id);
        entry.name_or_context = stringFromFixedBuffer(row.name_or_context, sizeof(row.name_or_context));
        entry.details_json = stringFromFixedBuffer(row.details_json, sizeof(row.details_json));
        if (entry.stimulus_frame_num >= 0) {
            max_frame = std::max(max_frame,
                                 static_cast<size_t>(entry.stimulus_frame_num));
        }
        data_.stimulus_events.push_back(std::move(entry));
    }
    std::cout << "  [StimulusEvents] Structured layout parsed "
              << data_.stimulus_events.size() << " events" << std::endl;

    return finalize_events(max_frame);
}

bool ZarrDetectionLoader::loadChaserBoundingBoxes(
    const ts::kvstore::KvStore& store,
    const std::string& run_base) {
    const std::string base = run_base + "tracking_data/bounding_boxes/";
    data_.chaser_bounding_boxes.clear();
    data_.chaser_bboxes_by_camera_frame.clear();
    data_.has_chaser_bboxes = false;

    auto column_exists = [&](const std::string& name) {
        if (arrayExists(store, base + name)) {
            return true;
        }
        if (root_path_.empty()) {
            return false;
        }
        namespace fs = std::filesystem;
        fs::path probe = fs::path(root_path_) / base / name / "zarr.json";
        return fs::exists(probe);
    };

    auto readIntColumn = [&](const std::vector<std::string>& names,
                             std::vector<int32_t>& dest) -> bool {
        for (const auto& name : names) {
            if (readInt32Array(store, base + name, dest) && !dest.empty()) {
                return true;
            }
            std::vector<int64_t> tmp64;
            if (readInt64Array(store, base + name, tmp64) && !tmp64.empty()) {
                dest.resize(tmp64.size());
                for (size_t i = 0; i < tmp64.size(); ++i) {
                    dest[i] = clampToInt32(tmp64[i]);
                }
                return true;
            }
        }
        return false;
    };

    auto readFloatColumn = [&](const std::vector<std::string>& names,
                               std::vector<float>& dest) -> bool {
        for (const auto& name : names) {
            if (readFloatArray(store, base + name, dest) && !dest.empty()) {
                return true;
            }
        }
        dest.clear();
        return false;
    };

    if (!column_exists("x_px") && !column_exists("x_min")) {
        std::cout << "  [ChaserBBox] No bounding box coordinates found at '" << base << "'" << std::endl;
        return false;
    }

    std::vector<float> x_vals;
    std::vector<float> y_vals;
    std::vector<float> width_vals;
    std::vector<float> height_vals;
    if (!readFloatColumn({"x_px", "x_min"}, x_vals) || x_vals.empty()) {
        std::cout << "  [ChaserBBox] Failed to read x coordinates from '" << base << "'" << std::endl;
        return false;
    }
    if (!readFloatColumn({"y_px", "y_min"}, y_vals) || y_vals.empty()) {
        std::cout << "  [ChaserBBox] Failed to read y coordinates from '" << base << "'" << std::endl;
        return false;
    }
    if (!readFloatColumn({"width_px", "width"}, width_vals) || width_vals.empty()) {
        std::cout << "  [ChaserBBox] Failed to read width from '" << base << "'" << std::endl;
        return false;
    }
    if (!readFloatColumn({"height_px", "height"}, height_vals) || height_vals.empty()) {
        std::cout << "  [ChaserBBox] Failed to read height from '" << base << "'" << std::endl;
        return false;
    }

    size_t count = x_vals.size();
    auto ensure_size = [&](auto& vec, const auto& default_value) {
        if (vec.empty()) {
            vec.assign(count, default_value);
        } else if (vec.size() != count) {
            vec.resize(count, default_value);
        }
    };

    ensure_size(y_vals, std::numeric_limits<float>::quiet_NaN());
    ensure_size(width_vals, std::numeric_limits<float>::quiet_NaN());
    ensure_size(height_vals, std::numeric_limits<float>::quiet_NaN());

    std::vector<float> centroid_x_vals;
    std::vector<float> centroid_y_vals;
    if (!readFloatColumn({"centroid_x", "cx_px"}, centroid_x_vals)) {
        centroid_x_vals.assign(count, std::numeric_limits<float>::quiet_NaN());
    } else {
        ensure_size(centroid_x_vals, std::numeric_limits<float>::quiet_NaN());
    }
    if (!readFloatColumn({"centroid_y", "cy_px"}, centroid_y_vals)) {
        centroid_y_vals.assign(count, std::numeric_limits<float>::quiet_NaN());
    } else {
        ensure_size(centroid_y_vals, std::numeric_limits<float>::quiet_NaN());
    }

    std::vector<float> confidence_vals;
    if (readFloatColumn({"confidence", "score"}, confidence_vals)) {
        ensure_size(confidence_vals, std::numeric_limits<float>::quiet_NaN());
    } else {
        confidence_vals.assign(count, std::numeric_limits<float>::quiet_NaN());
    }

    std::vector<int32_t> camera_frames;
    std::vector<int32_t> stimulus_frames;
    std::vector<int32_t> fish_ids;
    std::vector<int32_t> chaser_indices;

    readIntColumn({"camera_frame_id", "payload_frame_id", "frame_id"}, camera_frames);
    readIntColumn({"stimulus_frame_num", "stimulus_frame_id"}, stimulus_frames);
    readIntColumn({"fish_id", "roi_index", "box_index_in_payload"}, fish_ids);
    readIntColumn({"chaser_index"}, chaser_indices);

    ensure_size(camera_frames, -1);
    ensure_size(stimulus_frames, -1);
    ensure_size(fish_ids, -1);
    ensure_size(chaser_indices, -1);

    std::vector<uint8_t> target_mask_raw;
    bool has_target_mask = false;
    for (const auto& name : {"is_target", "target_flag", "is_target_roi"}) {
        if (readBoolArray(store, base + name, target_mask_raw) && !target_mask_raw.empty()) {
            has_target_mask = true;
            ensure_size(target_mask_raw, static_cast<uint8_t>(0));
            break;
        }
    }
    if (!has_target_mask) {
        target_mask_raw.assign(count, 0);
    }

    data_.chaser_bounding_boxes.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        ZarrDetectionData::ChaserBoundingBoxRecord record;
        record.camera_frame_id = camera_frames[i];
        record.stimulus_frame_num = stimulus_frames[i];
        record.fish_id = fish_ids[i];
        record.chaser_index = chaser_indices[i];
        record.x_px = x_vals[i];
        record.y_px = y_vals[i];
        record.width_px = width_vals[i];
        record.height_px = height_vals[i];
        record.centroid_x = centroid_x_vals[i];
        record.centroid_y = centroid_y_vals[i];
        record.confidence = confidence_vals[i];
        bool target_flag = has_target_mask ? (target_mask_raw[i] != 0) : false;
        if (!target_flag && record.fish_id < 0) {
            target_flag = true;
        }
        record.is_target = target_flag;
        data_.chaser_bounding_boxes.push_back(record);
    }

    bool any_camera_frames = false;
    for (const auto& record : data_.chaser_bounding_boxes) {
        if (record.camera_frame_id >= 0) {
            any_camera_frames = true;
            break;
        }
    }

    if (!any_camera_frames) {
        std::vector<int32_t> stimulus_to_camera;
        if (loadStimulusFrameMetadataMapping(store, run_base, stimulus_to_camera) &&
            !stimulus_to_camera.empty()) {
            size_t updated = 0;
            for (auto& record : data_.chaser_bounding_boxes) {
                int32_t stim_index = record.stimulus_frame_num;
                if (stim_index < 0 || static_cast<size_t>(stim_index) >= stimulus_to_camera.size()) {
                    continue;
                }
                int32_t frame = stimulus_to_camera[stim_index];
                if (frame >= 0) {
                    record.camera_frame_id = frame;
                    ++updated;
                }
            }
            if (updated > 0) {
                std::cout << "  [ChaserBBox] Camera frames inferred from frame metadata ("
                          << updated << " records)" << std::endl;
            }
        } else {
            std::cout << "  [ChaserBBox] Unable to resolve camera frames for run '"
                      << run_base << "'" << std::endl;
        }
    }

    data_.has_chaser_bboxes = !data_.chaser_bounding_boxes.empty();
    if (!data_.has_chaser_bboxes) {
        data_.chaser_bounding_boxes.clear();
        return false;
    }

    rebuildChaserBoundingBoxIndices();

    int32_t min_camera_frame = std::numeric_limits<int32_t>::max();
    int32_t max_camera_frame = std::numeric_limits<int32_t>::min();
    for (const auto& record : data_.chaser_bounding_boxes) {
        if (record.camera_frame_id >= 0) {
            min_camera_frame = std::min(min_camera_frame, record.camera_frame_id);
            max_camera_frame = std::max(max_camera_frame, record.camera_frame_id);
        }
    }

    std::cout << "  [ChaserBBox] Loaded " << data_.chaser_bounding_boxes.size()
              << " bounding box records";
    if (max_camera_frame >= min_camera_frame &&
        min_camera_frame != std::numeric_limits<int32_t>::max()) {
        std::cout << " (camera frames " << min_camera_frame << "-"
                  << max_camera_frame << ")";
    }
    std::cout << std::endl;

    if (!data_.chaser_bounding_boxes.empty()) {
        const auto& sample = data_.chaser_bounding_boxes.front();
        std::cout << "  [ChaserBBox] Sample: camera_frame=" << sample.camera_frame_id
                  << ", stimulus_frame=" << sample.stimulus_frame_num
                  << ", fish_id=" << sample.fish_id
                  << ", x=" << sample.x_px << ", y=" << sample.y_px
                  << ", w=" << sample.width_px << ", h=" << sample.height_px
                  << (sample.is_target ? " [target]" : "")
                  << std::endl;
    }

    return true;
}

bool ZarrDetectionLoader::loadChaserStates(const ts::kvstore::KvStore& store,
                                           const std::string& run_base) {
    const std::string base = run_base + "tracking_data/chaser_states/";
    data_.chaser_states.clear();
    data_.chaser_states_by_camera_frame.clear();
    data_.chaser_states_by_stimulus_frame.clear();
    data_.has_chaser_states = false;

    auto column_exists = [&](const std::string& name) {
        if (arrayExists(store, base + name)) {
            return true;
        }
        if (root_path_.empty()) {
            return false;
        }
        namespace fs = std::filesystem;
        fs::path probe = fs::path(root_path_) / base / name / "zarr.json";
        return fs::exists(probe);
    };

    if (!column_exists("stimulus_frame_num") || !column_exists("chaser_pos_x")) {
        if (!column_exists("chaser_pos_x")) {
            std::cout << "  [Chaser] No chaser state arrays found at '" << base << "'" << std::endl;
        }
        return false;
    }

    auto readInt32Or64Column = [&](const std::vector<std::string>& names,
                                   std::vector<int32_t>& dest) -> bool {
        for (const auto& name : names) {
            if (readInt32Array(store, base + name, dest) && !dest.empty()) {
                return true;
            }
            std::vector<int64_t> tmp64;
            if (readInt64Array(store, base + name, tmp64) && !tmp64.empty()) {
                dest.resize(tmp64.size());
                for (size_t i = 0; i < tmp64.size(); ++i) {
                    dest[i] = clampToInt32(tmp64[i]);
                }
                return true;
            }
        }
        return false;
    };

    std::vector<int32_t> stimulus_frames;
    if (!readInt32Or64Column({"stimulus_frame_num"}, stimulus_frames) || stimulus_frames.empty()) {
        std::cout << "  [Chaser] Failed to read stimulus_frame_num column at '" << base << "'" << std::endl;
        return false;
    }
    size_t count = stimulus_frames.size();

    std::vector<float> chaser_pos_x;
    std::vector<float> chaser_pos_y;
    if (!readFloatArray(store, base + "chaser_pos_x", chaser_pos_x) || chaser_pos_x.empty()) {
        std::cout << "  [Chaser] Missing required column 'chaser_pos_x' at '" << base << "'" << std::endl;
        return false;
    }
    if (!readFloatArray(store, base + "chaser_pos_y", chaser_pos_y) || chaser_pos_y.empty()) {
        std::cout << "  [Chaser] Missing required column 'chaser_pos_y' at '" << base << "'" << std::endl;
        return false;
    }

    auto readOptionalFloatColumn = [&](const std::string& name, std::vector<float>& out) {
        if (!readFloatArray(store, base + name, out)) {
            out.clear();
            return false;
        }
        return true;
    };

    std::vector<float> target_pos_x;
    std::vector<float> target_pos_y;
    readOptionalFloatColumn("target_pos_x", target_pos_x);
    readOptionalFloatColumn("target_pos_y", target_pos_y);

    std::vector<float> chaser_radius_px;
    std::vector<float> distance_to_target_px;
    std::vector<float> target_speed_px_per_s;
    readOptionalFloatColumn("chaser_radius_px", chaser_radius_px);
    readOptionalFloatColumn("distance_to_target_px", distance_to_target_px);
    readOptionalFloatColumn("target_speed_px_per_s", target_speed_px_per_s);

    std::vector<int32_t> camera_frames;
    if (column_exists("camera_frame_id") || column_exists("triggering_camera_frame_id")) {
        readInt32Or64Column({"camera_frame_id", "triggering_camera_frame_id"}, camera_frames);
    }

    std::vector<int32_t> chaser_indices;
    readInt32Or64Column({"chaser_index"}, chaser_indices);

    std::vector<int64_t> timestamps_ns;
    readInt64Array(store, base + "timestamp_ns_session", timestamps_ns);

    std::vector<uint8_t> is_chasing_raw;
    readBoolArray(store, base + "is_chasing", is_chasing_raw);

    auto sync_size = [&](auto& vec, const auto& default_value) {
        if (vec.empty()) {
            vec.assign(count, default_value);
        } else if (vec.size() != count) {
            vec.resize(count, default_value);
        }
    };

    sync_size(chaser_pos_x, std::numeric_limits<float>::quiet_NaN());
    sync_size(chaser_pos_y, std::numeric_limits<float>::quiet_NaN());
    sync_size(target_pos_x, std::numeric_limits<float>::quiet_NaN());
    sync_size(target_pos_y, std::numeric_limits<float>::quiet_NaN());
    sync_size(chaser_radius_px, std::numeric_limits<float>::quiet_NaN());
    sync_size(distance_to_target_px, std::numeric_limits<float>::quiet_NaN());
    sync_size(target_speed_px_per_s, std::numeric_limits<float>::quiet_NaN());
    sync_size(camera_frames, int32_t{-1});
    sync_size(chaser_indices, int32_t{-1});
    sync_size(timestamps_ns, int64_t{0});
    sync_size(is_chasing_raw, uint8_t{0});

    data_.chaser_states.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        ZarrDetectionData::ChaserStateRecord record;
        record.stimulus_frame_num = stimulus_frames[i];
        record.camera_frame_id = camera_frames[i];
        record.chaser_index = chaser_indices[i];
        record.chaser_pos_x = chaser_pos_x[i];
        record.chaser_pos_y = chaser_pos_y[i];
        record.target_pos_x = target_pos_x[i];
        record.target_pos_y = target_pos_y[i];
        record.chaser_radius_px = chaser_radius_px[i];
        record.distance_to_target_px = distance_to_target_px[i];
        record.target_speed_px_per_s = target_speed_px_per_s[i];
        record.timestamp_ns_session = timestamps_ns[i];
        record.is_chasing = is_chasing_raw[i];
        record.texture_space = true;
        applyChaserCoordinateTransformToRecord(data_.chaser_transform, record);
        applyChaserColorMetadataToRecord(data_, record);
        applyChaserBehaviorMetadataToRecord(data_, record);
        data_.chaser_states.push_back(std::move(record));
    }

    data_.has_chaser_states = !data_.chaser_states.empty();
    if (!data_.has_chaser_states) {
        return false;
    }

    bool any_camera_frames = false;
    for (const auto& record : data_.chaser_states) {
        if (record.camera_frame_id >= 0) {
            any_camera_frames = true;
            break;
        }
    }

    if (!any_camera_frames) {
        std::vector<int32_t> stimulus_to_camera;
        if (loadStimulusFrameMetadataMapping(store, run_base, stimulus_to_camera) &&
            !stimulus_to_camera.empty()) {
            size_t updated = 0;
            for (auto& record : data_.chaser_states) {
                int32_t stim_index = record.stimulus_frame_num;
                if (stim_index < 0 || static_cast<size_t>(stim_index) >= stimulus_to_camera.size()) {
                    continue;
                }
                int32_t frame = stimulus_to_camera[stim_index];
                if (frame >= 0) {
                    record.camera_frame_id = frame;
                    ++updated;
                }
            }
            if (updated > 0) {
                std::cout << "  [Chaser] Camera frames inferred from video metadata ("
                          << updated << " records)" << std::endl;
            }
        } else {
            std::cout << "  [Chaser] Unable to resolve camera frames from video_metadata for run '"
                      << run_base << "'" << std::endl;
        }
    }

    rebuildChaserStateIndices();

    int32_t min_camera_frame = std::numeric_limits<int32_t>::max();
    int32_t max_cam_frame_observed = std::numeric_limits<int32_t>::min();
    for (const auto& record : data_.chaser_states) {
        if (record.camera_frame_id >= 0) {
            min_camera_frame = std::min(min_camera_frame, record.camera_frame_id);
            max_cam_frame_observed = std::max(max_cam_frame_observed, record.camera_frame_id);
        }
    }

    std::cout << "  [Chaser] Loaded " << data_.chaser_states.size()
              << " chaser state records";
    if (max_cam_frame_observed >= min_camera_frame && min_camera_frame != std::numeric_limits<int32_t>::max()) {
        std::cout << " (camera frames " << min_camera_frame << "-" << max_cam_frame_observed << ")";
    }
    std::cout << std::endl;

    if (!data_.chaser_states.empty()) {
        const auto& sample = data_.chaser_states.front();
        std::cout << "  [Chaser] Sample: stim_frame=" << sample.stimulus_frame_num
                  << ", camera_frame=" << sample.camera_frame_id
                  << ", chaser=(" << sample.chaser_pos_x << "," << sample.chaser_pos_y << ")"
                  << ", target=(" << sample.target_pos_x << "," << sample.target_pos_y << ")"
                  << std::endl;
    }

    return true;
}

bool ZarrDetectionLoader::loadChaserStatesInterpolated(
    const ts::kvstore::KvStore& store,
    const std::string& run_base) {

    const std::string base = run_base + "tracking_data/chaser_states_interpolated/";
    data_.chaser_states_interpolated.clear();
    data_.chaser_states_interpolated_by_stimulus_frame.clear();
    data_.has_chaser_states_interpolated = false;

    auto column_exists = [&](const std::string& name) {
        if (arrayExists(store, base + name)) {
            return true;
        }
        if (root_path_.empty()) {
            return false;
        }
        namespace fs = std::filesystem;
        fs::path probe = fs::path(root_path_) / base / name / "zarr.json";
        return fs::exists(probe);
    };

    if (!column_exists("stimulus_frame_num")) {
        return false;
    }

    auto readInt32Or64Column = [&](const std::vector<std::string>& names,
                                   std::vector<int32_t>& dest) -> bool {
        for (const auto& name : names) {
            if (readInt32Array(store, base + name, dest) && !dest.empty()) {
                return true;
            }
            std::vector<int64_t> tmp64;
            if (readInt64Array(store, base + name, tmp64) && !tmp64.empty()) {
                dest.resize(tmp64.size());
                for (size_t i = 0; i < tmp64.size(); ++i) {
                    dest[i] = clampToInt32(tmp64[i]);
                }
                return true;
            }
        }
        return false;
    };

    auto resize_or_fill = [](std::vector<int32_t>& vec, size_t count, int32_t fill) {
        if (vec.empty()) {
            vec.assign(count, fill);
        } else if (vec.size() != count) {
            vec.resize(count, fill);
        }
    };

    std::vector<int32_t> stimulus_frames;
    if (!readInt32Or64Column({"stimulus_frame_num"}, stimulus_frames) ||
        stimulus_frames.empty()) {
        return false;
    }
    size_t count = stimulus_frames.size();

    std::vector<float> chaser_pos_x;
    std::vector<float> chaser_pos_y;
    if (!readFloatArray(store, base + "chaser_pos_x", chaser_pos_x) || chaser_pos_x.size() != count) {
        return false;
    }
    if (!readFloatArray(store, base + "chaser_pos_y", chaser_pos_y) || chaser_pos_y.size() != count) {
        return false;
    }

    auto readOptionalFloatColumn = [&](const std::string& name,
                                       std::vector<float>& out) {
        if (!readFloatArray(store, base + name, out) || out.size() != count) {
            out.assign(count, std::numeric_limits<float>::quiet_NaN());
            return false;
        }
        return true;
    };

    std::vector<float> target_pos_x;
    std::vector<float> target_pos_y;
    std::vector<float> chaser_radius_px;
    std::vector<float> distance_to_target_px;
    std::vector<float> target_speed_px_per_s;

    readOptionalFloatColumn("target_pos_x", target_pos_x);
    readOptionalFloatColumn("target_pos_y", target_pos_y);
    readOptionalFloatColumn("chaser_radius_px", chaser_radius_px);
    readOptionalFloatColumn("distance_to_target_px", distance_to_target_px);
    readOptionalFloatColumn("target_speed_px_per_s", target_speed_px_per_s);

    std::vector<int32_t> chaser_indices;
    std::vector<int32_t> camera_frame_ids;
    std::vector<int32_t> is_chasing;
    std::vector<int64_t> timestamp_ns_session;

    readInt32Or64Column({"chaser_index"}, chaser_indices);
    resize_or_fill(chaser_indices, count, -1);

    readInt32Or64Column({"camera_frame_id", "payload_frame_id"}, camera_frame_ids);
    resize_or_fill(camera_frame_ids, count, -1);

    readInt32Or64Column({"is_chasing"}, is_chasing);
    resize_or_fill(is_chasing, count, 0);

    std::vector<int64_t> timestamps64;
    if (!readInt64Array(store, base + "timestamp_ns_session", timestamps64) ||
        timestamps64.size() != count) {
        timestamps64.assign(count, 0);
    }

    std::vector<uint8_t> texture_space;
    if (!readBoolArray(store, base + "texture_space", texture_space) ||
        texture_space.size() != count) {
        texture_space.assign(count, 1);
    }

    std::vector<double> chaser_camera_x;
    std::vector<double> chaser_camera_y;
    std::vector<double> target_camera_x;
    std::vector<double> target_camera_y;
    std::vector<uint8_t> has_camera_coords;

    auto readFloat64Column = [&](const std::string& name,
                                 std::vector<double>& out) {
        std::vector<float> tmp;
        if (readFloatArray(store, base + name, tmp) && tmp.size() == count) {
            out.assign(tmp.begin(), tmp.end());
            return true;
        }
        out.assign(count, std::numeric_limits<double>::quiet_NaN());
        return false;
    };

    readFloat64Column("chaser_camera_x", chaser_camera_x);
    readFloat64Column("chaser_camera_y", chaser_camera_y);
    readFloat64Column("target_camera_x", target_camera_x);
    readFloat64Column("target_camera_y", target_camera_y);

    if (!readBoolArray(store, base + "has_camera_coords", has_camera_coords) ||
        has_camera_coords.size() != count) {
        has_camera_coords.assign(count, 0);
    }

    data_.chaser_states_interpolated.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        ZarrDetectionData::ChaserStateRecord record;
        record.stimulus_frame_num = stimulus_frames[i];
        record.camera_frame_id = camera_frame_ids[i];
        record.chaser_index = chaser_indices[i];
        record.chaser_pos_x = chaser_pos_x[i];
        record.chaser_pos_y = chaser_pos_y[i];
        record.target_pos_x = target_pos_x[i];
        record.target_pos_y = target_pos_y[i];
        record.chaser_radius_px = chaser_radius_px[i];
        record.distance_to_target_px = distance_to_target_px[i];
        record.target_speed_px_per_s = target_speed_px_per_s[i];
        record.timestamp_ns_session = timestamps64[i];
        record.is_chasing = static_cast<uint8_t>(is_chasing[i] != 0);
        record.texture_space = texture_space[i] != 0;
        record.chaser_camera_x = chaser_camera_x[i];
        record.chaser_camera_y = chaser_camera_y[i];
        record.target_camera_x = target_camera_x[i];
        record.target_camera_y = target_camera_y[i];
        record.has_camera_coords = has_camera_coords[i] != 0;
        applyChaserCoordinateTransformToRecord(data_.chaser_transform, record);
        applyChaserColorMetadataToRecord(data_, record);
        applyChaserBehaviorMetadataToRecord(data_, record);
        data_.chaser_states_interpolated.push_back(record);
    }

    rebuildInterpolatedChaserStateIndices();
    data_.has_chaser_states_interpolated = !data_.chaser_states_interpolated.empty();

    if (data_.has_chaser_states_interpolated) {
        std::cout << "  [ChaserInterp] Loaded " << data_.chaser_states_interpolated.size()
                  << " interpolated chaser records" << std::endl;
    }

    return data_.has_chaser_states_interpolated;
}

void ZarrDetectionLoader::rebuildChaserStateIndices() {
    data_.chaser_states_by_camera_frame.clear();
    data_.chaser_states_by_stimulus_frame.clear();

    if (data_.chaser_states.empty()) {
        return;
    }

    int32_t max_camera_frame = -1;
    int32_t max_stimulus_frame = -1;
    for (const auto& record : data_.chaser_states) {
        if (record.camera_frame_id >= 0) {
            max_camera_frame = std::max(max_camera_frame, record.camera_frame_id);
        }
        if (record.stimulus_frame_num >= 0) {
            max_stimulus_frame = std::max(max_stimulus_frame, record.stimulus_frame_num);
        }
    }

    if (max_camera_frame >= 0) {
        data_.chaser_states_by_camera_frame.resize(static_cast<size_t>(max_camera_frame) + 1);
    } else {
        data_.chaser_states_by_camera_frame.resize(data_.total_frames);
    }

    if (max_stimulus_frame >= 0) {
        data_.chaser_states_by_stimulus_frame.resize(static_cast<size_t>(max_stimulus_frame) + 1);
    }

    for (size_t idx = 0; idx < data_.chaser_states.size(); ++idx) {
        const auto& record = data_.chaser_states[idx];
        if (record.camera_frame_id >= 0) {
            size_t frame_index = static_cast<size_t>(record.camera_frame_id);
            if (frame_index >= data_.chaser_states_by_camera_frame.size()) {
                data_.chaser_states_by_camera_frame.resize(frame_index + 1);
            }
            data_.chaser_states_by_camera_frame[frame_index].push_back(idx);
        }
        if (record.stimulus_frame_num >= 0) {
            size_t stim_index = static_cast<size_t>(record.stimulus_frame_num);
            if (stim_index >= data_.chaser_states_by_stimulus_frame.size()) {
                data_.chaser_states_by_stimulus_frame.resize(stim_index + 1);
            }
            data_.chaser_states_by_stimulus_frame[stim_index].push_back(idx);
        }
    }
}

void ZarrDetectionLoader::rebuildInterpolatedChaserStateIndices() {
    data_.chaser_states_interpolated_by_stimulus_frame.clear();
    if (data_.chaser_states_interpolated.empty()) {
        return;
    }

    int32_t max_stimulus_frame = -1;
    for (const auto& record : data_.chaser_states_interpolated) {
        if (record.stimulus_frame_num >= 0) {
            max_stimulus_frame = std::max(max_stimulus_frame, record.stimulus_frame_num);
        }
    }

    if (max_stimulus_frame >= 0) {
        data_.chaser_states_interpolated_by_stimulus_frame.resize(static_cast<size_t>(max_stimulus_frame) + 1);
    }

    for (size_t idx = 0; idx < data_.chaser_states_interpolated.size(); ++idx) {
        const auto& record = data_.chaser_states_interpolated[idx];
        if (record.stimulus_frame_num < 0) {
            continue;
        }
        size_t stim_index = static_cast<size_t>(record.stimulus_frame_num);
        if (stim_index >= data_.chaser_states_interpolated_by_stimulus_frame.size()) {
            data_.chaser_states_interpolated_by_stimulus_frame.resize(stim_index + 1);
        }
        data_.chaser_states_interpolated_by_stimulus_frame[stim_index].push_back(idx);
    }
}

void ZarrDetectionLoader::rebuildChaserBoundingBoxIndices() {
    data_.chaser_bboxes_by_camera_frame.clear();

    if (data_.chaser_bounding_boxes.empty()) {
        return;
    }

    int32_t max_camera_frame = -1;
    for (const auto& record : data_.chaser_bounding_boxes) {
        if (record.camera_frame_id >= 0) {
            max_camera_frame = std::max(max_camera_frame, record.camera_frame_id);
        }
    }

    if (max_camera_frame >= 0) {
        data_.chaser_bboxes_by_camera_frame.resize(static_cast<size_t>(max_camera_frame) + 1);
    } else if (data_.total_frames > 0) {
        data_.chaser_bboxes_by_camera_frame.resize(data_.total_frames);
    }

    for (size_t idx = 0; idx < data_.chaser_bounding_boxes.size(); ++idx) {
        const auto& record = data_.chaser_bounding_boxes[idx];
        if (record.camera_frame_id < 0) {
            continue;
        }
        size_t frame_index = static_cast<size_t>(record.camera_frame_id);
        if (frame_index >= data_.chaser_bboxes_by_camera_frame.size()) {
            data_.chaser_bboxes_by_camera_frame.resize(frame_index + 1);
        }
        data_.chaser_bboxes_by_camera_frame[frame_index].push_back(idx);
    }
}

void ZarrDetectionLoader::updateChaserCameraFramesFromAlignment() {
    if (!data_.has_chaser_states) {
        return;
    }
    const auto& mapping = data_.latest_interpolation.camera_to_metadata_index;
    if (mapping.empty()) {
        return;
    }

    size_t stim_count = 0;
    for (const auto& record : data_.chaser_states) {
        if (record.stimulus_frame_num >= 0) {
            stim_count = std::max(stim_count,
                                  static_cast<size_t>(record.stimulus_frame_num + 1));
        }
    }
    if (stim_count == 0) {
        return;
    }

    std::vector<int32_t> stimulus_to_camera(stim_count, -1);
    int32_t offset = static_cast<int32_t>(data_.stimulus_camera_frame_offset);
    for (size_t camera_frame = 0; camera_frame < mapping.size(); ++camera_frame) {
        int32_t stim_index = mapping[camera_frame];
        if (stim_index < 0 || static_cast<size_t>(stim_index) >= stimulus_to_camera.size()) {
            continue;
        }
        if (stimulus_to_camera[stim_index] == -1) {
            int32_t resolved = static_cast<int32_t>(camera_frame) + offset;
            stimulus_to_camera[stim_index] = resolved;
        }
    }

    bool updated_any = false;
    size_t updated_count = 0;
    for (auto& record : data_.chaser_states) {
        if (record.camera_frame_id >= 0) {
            continue;
        }
        int32_t stim_index = record.stimulus_frame_num;
        if (stim_index < 0 || static_cast<size_t>(stim_index) >= stimulus_to_camera.size()) {
            continue;
        }
        int32_t camera_frame = stimulus_to_camera[stim_index];
        if (camera_frame >= 0) {
            record.camera_frame_id = camera_frame;
            updated_any = true;
            ++updated_count;
        }
    }

    if (updated_any) {
        rebuildChaserStateIndices();
        std::cout << "  [Chaser] Camera frames inferred from alignment mapping "
                  << "(offset " << offset << ", updated " << updated_count << " records)"
                  << std::endl;
    }
}

bool ZarrDetectionLoader::loadStimulusFrameMetadataMapping(
    const ts::kvstore::KvStore& store,
    const std::string& run_base,
    std::vector<int32_t>& stimulus_to_camera) {

    const std::string meta_base = run_base + "video_metadata/frame_metadata/";
    auto has_column = [&](const std::string& name) {
        if (arrayExists(store, meta_base + name)) {
            return true;
        }
        if (root_path_.empty()) {
            return false;
        }
        namespace fs = std::filesystem;
        fs::path probe = fs::path(root_path_) / meta_base / name / "zarr.json";
        return fs::exists(probe);
    };

    if (!has_column("stimulus_frame_num") ||
        !has_column("triggering_camera_frame_id")) {
        return false;
    }

    std::vector<int64_t> stimulus_frames64;
    std::vector<int64_t> camera_frames64;
    if (!readInt64Array(store, meta_base + "stimulus_frame_num", stimulus_frames64) ||
        stimulus_frames64.empty()) {
        return false;
    }
    if (!readInt64Array(store, meta_base + "triggering_camera_frame_id", camera_frames64) ||
        camera_frames64.size() != stimulus_frames64.size()) {
        return false;
    }

    int64_t max_stimulus = -1;
    for (int64_t value : stimulus_frames64) {
        if (value > max_stimulus) {
            max_stimulus = value;
        }
    }
    if (max_stimulus < 0) {
        return false;
    }

    stimulus_to_camera.assign(static_cast<size_t>(max_stimulus) + 1, -1);
    size_t applied = 0;
    for (size_t i = 0; i < stimulus_frames64.size(); ++i) {
        int64_t stim64 = stimulus_frames64[i];
        int64_t cam64 = camera_frames64[i];
        if (stim64 < 0 || cam64 < 0) {
            continue;
        }
        size_t stim_index = static_cast<size_t>(stim64);
        if (stim_index >= stimulus_to_camera.size()) {
            continue;
        }
        int32_t cam32 = clampToInt32(cam64);
        if (stimulus_to_camera[stim_index] == -1) {
            stimulus_to_camera[stim_index] = cam32;
            ++applied;
        }
    }

    if (applied == 0) {
        stimulus_to_camera.clear();
        return false;
    }

    std::cout << "  [Chaser] Frame metadata mapping loaded (" << applied
              << " stimulus frames)" << std::endl;
    return true;
}

std::string ZarrDetectionLoader::formatStimulusEvent(
    const ZarrDetectionData::EventLogEntry& entry) const {
    auto trim = [](std::string text) -> std::string {
        auto begin = std::find_if(text.begin(), text.end(),
                                  [](unsigned char ch) { return !std::isspace(ch); });
        auto end = std::find_if(text.rbegin(), text.rend(),
                                [](unsigned char ch) { return !std::isspace(ch); }).base();
        if (begin >= end) {
            return std::string();
        }
        return std::string(begin, end);
    };

    std::string label;
    auto it = data_.event_type_names.find(entry.event_type_id);
    if (it != data_.event_type_names.end()) {
        label = it->second;
    }
    std::string context = trim(entry.name_or_context);
    if (label.empty()) {
        label = !context.empty() ? context
                                 : ("Event " + std::to_string(entry.event_type_id));
    } else if (!context.empty() && context != label) {
        label += " - " + context;
    }

    std::string details = trim(entry.details_json);
    if (!details.empty() && details != "{}" && details != "null") {
        bool suppress_details = false;
        if ((details.front() == '[' && details.back() == ']') ||
            (details.front() == '{' && details.back() == '}')) {
            try {
                auto parsed = json::parse(details);
                if (parsed.is_structured()) {
                    suppress_details = true;
                }
            } catch (const json::parse_error&) {
                // leave suppress_details false
            }
        }
        if (!suppress_details) {
            if (details.size() > 96) {
                details.resize(93);
                details += "...";
            }
            label += " [" + details + "]";
        }
    }
    return label;
}

void ZarrDetectionLoader::clearStimulusEventTimelineCache() {
    data_.stimulus_event_timeline.clear();
    ++data_.stimulus_event_timeline_generation;
}

void ZarrDetectionLoader::rebuildStimulusEventTimelineCache(
    const std::vector<int32_t>* stimulus_to_camera_map) {
    data_.stimulus_event_timeline.clear();
    if (!data_.has_stimulus_events) {
        ++data_.stimulus_event_timeline_generation;
        return;
    }

    data_.stimulus_event_timeline.reserve(data_.stimulus_events.size());
    for (size_t idx = 0; idx < data_.stimulus_events.size(); ++idx) {
        const auto& entry = data_.stimulus_events[idx];
        int32_t camera_frame = entry.camera_frame_id;
        if (camera_frame < 0 && stimulus_to_camera_map != nullptr &&
            entry.stimulus_frame_num >= 0) {
            const auto frame_index =
                static_cast<size_t>(entry.stimulus_frame_num);
            if (frame_index < stimulus_to_camera_map->size()) {
                camera_frame = (*stimulus_to_camera_map)[frame_index];
            }
        }

        ZarrDetectionData::StimulusEventSummary summary;
        summary.source_event_index = idx;
        summary.stimulus_frame_num = entry.stimulus_frame_num;
        summary.camera_frame_id = camera_frame;
        summary.event_type_id = entry.event_type_id;
        summary.label = formatStimulusEvent(entry);
        data_.stimulus_event_timeline.push_back(std::move(summary));
    }

    std::sort(data_.stimulus_event_timeline.begin(),
              data_.stimulus_event_timeline.end(),
              [](const ZarrDetectionData::StimulusEventSummary& a,
                 const ZarrDetectionData::StimulusEventSummary& b) {
                  if (a.stimulus_frame_num != b.stimulus_frame_num) {
                      return a.stimulus_frame_num < b.stimulus_frame_num;
                  }
                  if (a.camera_frame_id != b.camera_frame_id) {
                      return a.camera_frame_id < b.camera_frame_id;
                  }
                  if (a.event_type_id != b.event_type_id) {
                      return a.event_type_id < b.event_type_id;
                  }
                  return a.source_event_index < b.source_event_index;
              });
    ++data_.stimulus_event_timeline_generation;
}

std::vector<std::string> ZarrDetectionLoader::getStimulusEventsForFrame(
    size_t frame_id) const {
    std::vector<std::string> result;
    if (!data_.has_stimulus_events) {
        return result;
    }
    if (frame_id < data_.stimulus_events_by_camera_frame.size()) {
        for (size_t idx : data_.stimulus_events_by_camera_frame[frame_id]) {
            if (idx >= data_.stimulus_events.size()) {
                continue;
            }
            result.push_back(formatStimulusEvent(data_.stimulus_events[idx]));
        }
    }
    if (!result.empty()) {
        return result;
    }
    if (frame_id < data_.stimulus_events_by_frame.size()) {
        for (size_t idx : data_.stimulus_events_by_frame[frame_id]) {
            if (idx >= data_.stimulus_events.size()) {
                continue;
            }
            result.push_back(formatStimulusEvent(data_.stimulus_events[idx]));
        }
    }
    return result;
}

const std::vector<ZarrDetectionLoader::StimulusEventSummary>&
ZarrDetectionLoader::getStimulusEventTimeline() const {
    return data_.stimulus_event_timeline;
}

const ZarrDetectionData::StimulusStep*
ZarrDetectionLoader::getStimulusStepForFrame(int32_t camera_frame) const {
    if (camera_frame < 0 || data_.stimulus_steps.empty()) {
        return nullptr;
    }

    // Step boundaries can share a frame at handoff points. Prefer the later
    // step when a frame is both the previous end and next start.
    for (auto it = data_.stimulus_steps.rbegin();
         it != data_.stimulus_steps.rend();
         ++it) {
        if (it->start_camera_frame < 0 || it->end_camera_frame < 0) {
            continue;
        }
        if (camera_frame >= it->start_camera_frame &&
            camera_frame <= it->end_camera_frame) {
            return &(*it);
        }
    }
    return nullptr;
}

bool ZarrDetectionLoader::hasStimulusFrameMapping() const {
    if (!data_.has_stimulus_alignment_data) {
        return false;
    }
    const auto& interp = data_.latest_interpolation;
    return interp.has_direct_stimulus_lookup ||
           !interp.camera_to_metadata_index_corrected.empty() ||
           !interp.camera_to_metadata_index.empty();
}

bool ZarrDetectionLoader::hasCorrectedStimulusFrameMapping() const {
    if (!data_.has_stimulus_alignment_data) {
        return false;
    }
    const auto& interp = data_.latest_interpolation;
    if (interp.has_direct_stimulus_lookup &&
        !interp.camera_to_stimulus_frame_corrected.empty()) {
        return true;
    }
    return interp.frame_metadata_corrected_loaded &&
           !interp.camera_to_metadata_index_corrected.empty() &&
           !interp.frame_metadata_stimulus_frames_corrected.empty();
}

int64_t ZarrDetectionLoader::getStimulusCameraFrameOffset() const {
    return data_.stimulus_camera_frame_offset;
}

std::optional<int32_t> ZarrDetectionLoader::resolveStimulusMetadataIndex(
    const std::vector<int32_t>& mapping,
    int32_t camera_frame) const {
    if (mapping.empty() || camera_frame < 0) {
        return std::nullopt;
    }
    size_t index = static_cast<size_t>(camera_frame);
    if (index >= mapping.size()) {
        return std::nullopt;
    }
    int32_t metadata_index = mapping[index];
    if (metadata_index < 0) {
        return std::nullopt;
    }
    return metadata_index;
}

std::optional<int32_t> ZarrDetectionLoader::resolveStimulusFrame(
    const std::vector<int32_t>& mapping,
    const std::vector<int32_t>& frame_numbers,
    int32_t camera_frame) const {
    if (frame_numbers.empty()) {
        return std::nullopt;
    }
    auto metadata_index = resolveStimulusMetadataIndex(mapping, camera_frame);
    if (!metadata_index) {
        return std::nullopt;
    }
    size_t meta_idx = static_cast<size_t>(*metadata_index);
    if (meta_idx >= frame_numbers.size()) {
        return std::nullopt;
    }
    int32_t stimulus_frame = frame_numbers[meta_idx];
    if (stimulus_frame < 0) {
        return std::nullopt;
    }
    return stimulus_frame;
}

std::optional<int32_t> ZarrDetectionLoader::resolveDirectStimulusFrame(
    int32_t camera_frame) const {
    const auto& interp = data_.latest_interpolation;
    if (!interp.has_direct_stimulus_lookup || camera_frame < 0) {
        return std::nullopt;
    }
    const auto& direct = interp.camera_to_stimulus_frame_corrected;
    size_t index = static_cast<size_t>(camera_frame);
    if (index >= direct.size()) {
        return std::nullopt;
    }
    int32_t stimulus_frame = direct[index];
    if (stimulus_frame < 0) {
        return std::nullopt;
    }
    return stimulus_frame;
}

std::optional<int32_t>
ZarrDetectionLoader::getStimulusMetadataIndexForCameraFrame(int32_t camera_frame,
                                                            bool prefer_corrected) const {
    if (!hasStimulusFrameMapping() || camera_frame < 0) {
        return std::nullopt;
    }
    const auto& interp = data_.latest_interpolation;
    if (prefer_corrected) {
        if (auto corrected =
                resolveStimulusMetadataIndex(interp.camera_to_metadata_index_corrected,
                                             camera_frame)) {
            return corrected;
        }
    }
    return resolveStimulusMetadataIndex(interp.camera_to_metadata_index, camera_frame);
}

std::optional<int32_t>
ZarrDetectionLoader::getStimulusFrameForCameraFrame(int32_t camera_frame,
                                                    bool prefer_corrected) const {
    const auto& interp = data_.latest_interpolation;
    if (prefer_corrected) {
        if (auto direct = resolveDirectStimulusFrame(camera_frame)) {
            return direct;
        }
    }
    if (prefer_corrected) {
        if (interp.frame_metadata_corrected_loaded) {
            if (auto corrected =
                    resolveStimulusFrame(interp.camera_to_metadata_index_corrected,
                                         interp.frame_metadata_stimulus_frames_corrected,
                                         camera_frame)) {
                return corrected;
            }
        }
    }
    if (!interp.frame_metadata_loaded) {
        return std::nullopt;
    }
    return resolveStimulusFrame(interp.camera_to_metadata_index,
                                interp.frame_metadata_stimulus_frames,
                                camera_frame);
}

std::optional<int32_t>
ZarrDetectionLoader::getCameraFrameForStimulusFrame(int32_t stimulus_frame,
                                                    bool prefer_corrected) const {
    if (!hasStimulusFrameMapping() || stimulus_frame < 0) {
        return std::nullopt;
    }

    const auto& interp = data_.latest_interpolation;

    auto resolve_from_direct = [&](const std::vector<int32_t>& direct)
            -> std::optional<int32_t> {
        if (direct.empty()) {
            return std::nullopt;
        }
        for (size_t camera_idx = 0; camera_idx < direct.size(); ++camera_idx) {
            if (direct[camera_idx] == stimulus_frame) {
                return static_cast<int32_t>(camera_idx);
            }
        }
        return std::nullopt;
    };

    auto resolve_from_metadata = [&](const std::vector<int32_t>& mapping,
                                     const std::vector<int32_t>& frame_numbers)
            -> std::optional<int32_t> {
        if (mapping.empty() || frame_numbers.empty()) {
            return std::nullopt;
        }
        for (size_t camera_idx = 0; camera_idx < mapping.size(); ++camera_idx) {
            int32_t metadata_idx = mapping[camera_idx];
            if (metadata_idx < 0) {
                continue;
            }
            size_t meta_index = static_cast<size_t>(metadata_idx);
            if (meta_index >= frame_numbers.size()) {
                continue;
            }
            if (frame_numbers[meta_index] == stimulus_frame) {
                return static_cast<int32_t>(camera_idx);
            }
        }
        return std::nullopt;
    };

    if (prefer_corrected) {
        if (interp.has_direct_stimulus_lookup) {
            if (auto camera = resolve_from_direct(
                    interp.camera_to_stimulus_frame_corrected)) {
                return camera;
            }
        }
        if (interp.frame_metadata_corrected_loaded) {
            if (auto camera = resolve_from_metadata(
                    interp.camera_to_metadata_index_corrected,
                    interp.frame_metadata_stimulus_frames_corrected)) {
                return camera;
            }
        }
    }

    if (auto camera = resolve_from_metadata(interp.camera_to_metadata_index,
                                            interp.frame_metadata_stimulus_frames)) {
        return camera;
    }

    return std::nullopt;
}

std::optional<int32_t> ZarrDetectionLoader::getFirstCameraFrameWithStimulus(
    bool prefer_corrected) const {
    if (!hasStimulusFrameMapping()) {
        return std::nullopt;
    }
    const auto& interp = data_.latest_interpolation;
    if (prefer_corrected && interp.has_direct_stimulus_lookup) {
        if (interp.first_camera_frame_with_stimulus_corrected >= 0) {
            return interp.first_camera_frame_with_stimulus_corrected;
        }
        const auto& direct = interp.camera_to_stimulus_frame_corrected;
        for (size_t i = 0; i < direct.size(); ++i) {
            if (direct[i] >= 0) {
                return static_cast<int32_t>(i);
            }
        }
    }
    auto find_first = [](const std::vector<int32_t>& mapping,
                         int32_t cached) -> std::optional<int32_t> {
        if (mapping.empty()) {
            return std::nullopt;
        }
        if (cached >= 0) {
            return cached;
        }
        for (size_t i = 0; i < mapping.size(); ++i) {
            if (mapping[i] >= 0) {
                return static_cast<int32_t>(i);
            }
        }
        return std::nullopt;
    };

    if (prefer_corrected) {
        if (auto corrected = find_first(interp.camera_to_metadata_index_corrected,
                                        interp.first_camera_frame_with_stimulus_corrected)) {
            return corrected;
        }
    }
    return find_first(interp.camera_to_metadata_index,
                      interp.first_camera_frame_with_stimulus);
}

std::optional<int32_t> ZarrDetectionLoader::getFirstStimulusFrameNumber(
    bool prefer_corrected) const {
    const auto& interp = data_.latest_interpolation;
    if (prefer_corrected && interp.has_direct_stimulus_lookup) {
        if (interp.first_stimulus_frame_corrected >= 0) {
            return interp.first_stimulus_frame_corrected;
        }
        const auto& direct = interp.camera_to_stimulus_frame_corrected;
        for (int32_t value : direct) {
            if (value >= 0) {
                return value;
            }
        }
    }
    auto resolve_first = [](const std::vector<int32_t>& frame_numbers,
                            int32_t cached_frame,
                            int32_t cached_meta,
                            const std::vector<int32_t>& mapping) -> std::optional<int32_t> {
        if (frame_numbers.empty()) {
            return std::nullopt;
        }
        if (cached_frame >= 0) {
            return cached_frame;
        }
        int32_t meta_idx = cached_meta;
        if (meta_idx < 0 && !mapping.empty()) {
            for (size_t i = 0; i < mapping.size(); ++i) {
                if (mapping[i] >= 0) {
                    meta_idx = mapping[i];
                    break;
                }
            }
        }
        if (meta_idx >= 0 &&
            static_cast<size_t>(meta_idx) < frame_numbers.size() &&
            frame_numbers[static_cast<size_t>(meta_idx)] >= 0) {
            return frame_numbers[static_cast<size_t>(meta_idx)];
        }
        return std::nullopt;
    };

    if (prefer_corrected && interp.frame_metadata_corrected_loaded) {
        if (auto corrected = resolve_first(
                interp.frame_metadata_stimulus_frames_corrected,
                interp.first_stimulus_frame_corrected,
                interp.first_metadata_index_with_stimulus_corrected,
                interp.camera_to_metadata_index_corrected)) {
            return corrected;
        }
    }
    if (!interp.frame_metadata_loaded) {
        return std::nullopt;
    }
    return resolve_first(interp.frame_metadata_stimulus_frames,
                         interp.first_stimulus_frame,
                         interp.first_metadata_index_with_stimulus,
                         interp.camera_to_metadata_index);
}

std::vector<ZarrDetectionLoader::ChaserBoundingBox>
ZarrDetectionLoader::getChaserBoundingBoxesForFrame(size_t frame_id) const {
    std::vector<ChaserBoundingBox> result;
    if (!data_.has_chaser_bboxes ||
        frame_id >= data_.chaser_bboxes_by_camera_frame.size()) {
        return result;
    }

    // Use a map to keep only the latest bbox for each chaser_index
    // Key: chaser_index, Value: index in result vector
    std::unordered_map<int32_t, size_t> latest_by_chaser_index;

    for (size_t idx : data_.chaser_bboxes_by_camera_frame[frame_id]) {
        if (idx >= data_.chaser_bounding_boxes.size()) {
            continue;
        }
        const auto& src = data_.chaser_bounding_boxes[idx];
        ChaserBoundingBox box;
        box.fish_id = src.fish_id;
        box.x_px = src.x_px;
        box.y_px = src.y_px;
        box.width_px = src.width_px;
        box.height_px = src.height_px;
        box.centroid_x = src.centroid_x;
        box.centroid_y = src.centroid_y;
        box.confidence = src.confidence;
        box.camera_frame_id = src.camera_frame_id;
        box.stimulus_frame_num = src.stimulus_frame_num;
        box.chaser_index = src.chaser_index;
        box.is_target = src.is_target;

        // Deduplicate: keep only the latest stimulus frame for each chaser_index
        int32_t key = src.chaser_index;
        auto it = latest_by_chaser_index.find(key);
        if (it == latest_by_chaser_index.end()) {
            // First bbox with this chaser_index
            latest_by_chaser_index[key] = result.size();
            result.push_back(box);
        } else {
            // Already have a bbox with this chaser_index, check if this one is newer
            ChaserBoundingBox& existing = result[it->second];
            // Prefer the one with higher stimulus_frame_num (more recent)
            // If tied, prefer is_target=true, then higher confidence
            bool should_replace = false;
            if (box.stimulus_frame_num > existing.stimulus_frame_num) {
                should_replace = true;
            } else if (box.stimulus_frame_num == existing.stimulus_frame_num) {
                // Same stimulus frame, use other criteria
                if (box.is_target && !existing.is_target) {
                    should_replace = true;
                } else if (box.is_target == existing.is_target) {
                    // Both target or both not target, prefer higher confidence
                    if (std::isfinite(box.confidence) && std::isfinite(existing.confidence)) {
                        if (box.confidence > existing.confidence) {
                            should_replace = true;
                        }
                    } else if (std::isfinite(box.confidence) && !std::isfinite(existing.confidence)) {
                        should_replace = true;
                    }
                }
            }

            if (should_replace) {
                existing = box;
            }
        }
    }

    return result;
}

std::vector<ZarrDetectionLoader::ChaserState>
ZarrDetectionLoader::getChaserStatesForFrame(size_t frame_id) const {
    std::vector<ChaserState> result;
    if (!data_.has_chaser_states ||
        frame_id >= data_.chaser_states_by_camera_frame.size()) {
        if (kChaserDebugLoggingEnabled) {
            std::cout << "  [ChaserDebug] Frame " << frame_id
                      << ": 0 chaser state(s)" << std::endl;
            if (!data_.has_chaser_states) {
                std::cout << "    (Chaser data not loaded)" << std::endl;
            }
        }
        return result;
    }

    std::unordered_map<int32_t, size_t> latest_by_index;
    for (size_t idx : data_.chaser_states_by_camera_frame[frame_id]) {
        if (idx >= data_.chaser_states.size()) {
            continue;
        }
        const auto& src = data_.chaser_states[idx];
        ChaserState candidate;
        candidate.stimulus_frame_num = src.stimulus_frame_num;
        candidate.camera_frame_id = src.camera_frame_id;
        candidate.chaser_index = src.chaser_index;
        candidate.chaser_pos_x = src.chaser_pos_x;
        candidate.chaser_pos_y = src.chaser_pos_y;
        candidate.target_pos_x = src.target_pos_x;
        candidate.target_pos_y = src.target_pos_y;
        candidate.chaser_radius_px = src.chaser_radius_px;
        candidate.distance_to_target_px = src.distance_to_target_px;
        candidate.target_speed_px_per_s = src.target_speed_px_per_s;
        candidate.is_chasing = src.is_chasing != 0;
        candidate.timestamp_ns_session = src.timestamp_ns_session;
        candidate.texture_space = src.texture_space;
        candidate.coordinate_frame = src.coordinate_frame;
        candidate.coordinate_origin = src.coordinate_origin;
        candidate.stimulus_canvas_offset_x = src.stimulus_canvas_offset_x;
        candidate.stimulus_canvas_offset_y = src.stimulus_canvas_offset_y;
        candidate.has_stimulus_canvas_offset = src.has_stimulus_canvas_offset;
        candidate.chaser_camera_x = src.chaser_camera_x;
        candidate.chaser_camera_y = src.chaser_camera_y;
        candidate.target_camera_x = src.target_camera_x;
        candidate.target_camera_y = src.target_camera_y;
        candidate.has_camera_coords = src.has_camera_coords;
        candidate.chaser_rgba = src.chaser_rgba;
        candidate.has_chaser_rgba = src.has_chaser_rgba;
        candidate.behavior_mode = src.behavior_mode;
        candidate.has_behavior_mode = src.has_behavior_mode;
        candidate.enable_chase = src.enable_chase;
        candidate.has_enable_chase = src.has_enable_chase;
        candidate.enable_random_movement = src.enable_random_movement;
        candidate.has_enable_random_movement = src.has_enable_random_movement;

        int32_t index_key = candidate.chaser_index;
        auto it = latest_by_index.find(index_key);
        bool keep = true;
        if (it != latest_by_index.end()) {
            auto& existing = result[it->second];
            if (candidate.stimulus_frame_num < existing.stimulus_frame_num) {
                keep = false;
            } else if (candidate.stimulus_frame_num == existing.stimulus_frame_num &&
                       candidate.timestamp_ns_session <= existing.timestamp_ns_session) {
                keep = false;
            }
            if (keep) {
                existing = candidate;
            }
        } else if (keep) {
            latest_by_index[index_key] = result.size();
            result.push_back(candidate);
        }
    }

#if defined(CRIMSON_CHASER_DEBUG_LOGS)
    if (frame_id < 360 || frame_id % 3000 == 0) {
        std::cout << "  [ChaserDebug] Frame " << frame_id << " returning "
                  << result.size() << " states" << std::endl;
        for (const auto& state : result) {
            std::cout << "    idx=" << state.chaser_index
                      << " cam_frame=" << state.camera_frame_id
                      << " stim=" << state.stimulus_frame_num
                      << " chaser=(" << state.chaser_pos_x << "," << state.chaser_pos_y << ")"
                      << " target=(" << state.target_pos_x << "," << state.target_pos_y << ")"
                      << " frame='" << state.coordinate_frame << "'"
                      << " origin='" << state.coordinate_origin << "'"
                      << " canvas_offset=(" << state.stimulus_canvas_offset_x
                      << "," << state.stimulus_canvas_offset_y << ")"
                      << " cam=(" << state.chaser_camera_x << "," << state.chaser_camera_y << ")"
                      << " has_cam=" << (state.has_camera_coords ? "Y" : "N")
                      << std::endl;
        }
    }
#endif

    if (kChaserDebugLoggingEnabled) {
        std::cout << "  [ChaserDebug] Frame " << frame_id << ": "
                  << result.size() << " chaser state(s)" << std::endl;
        for (const auto& state : result) {
            std::cout << "    idx=" << state.chaser_index
                      << " camera_frame=" << state.camera_frame_id
                      << " stim_frame=" << state.stimulus_frame_num
                      << " chaser=(" << state.chaser_pos_x << "," << state.chaser_pos_y << ")"
                      << " target=(" << state.target_pos_x << "," << state.target_pos_y << ")"
                      << " radius_px=" << state.chaser_radius_px
                      << " distance_px=" << state.distance_to_target_px
                      << " target_speed=" << state.target_speed_px_per_s
                      << " is_chasing=" << (state.is_chasing ? "true" : "false")
                      << " timestamp_ns=" << state.timestamp_ns_session
                      << " space=" << (state.texture_space ? "texture" : "camera")
                      << " frame='" << state.coordinate_frame << "'"
                      << " origin='" << state.coordinate_origin << "'";
            if (state.has_stimulus_canvas_offset) {
                std::cout << " canvas_offset=(" << state.stimulus_canvas_offset_x
                          << "," << state.stimulus_canvas_offset_y << ")";
            }
            std::cout
                      << std::endl;
            if (state.has_camera_coords) {
                std::cout << "      camera_chaser=(" << state.chaser_camera_x << "," << state.chaser_camera_y << ")"
                          << " camera_target=(" << state.target_camera_x << "," << state.target_camera_y << ")"
                          << std::endl;
            }
        }
    }

    return result;
}

std::vector<ZarrDetectionLoader::ChaserState>
ZarrDetectionLoader::getChaserStatesForStimulusFrame(int32_t stimulus_frame) const {
    std::vector<ChaserState> result;
    if (!data_.has_chaser_states || stimulus_frame < 0) {
        return result;
    }
    size_t stim_index = static_cast<size_t>(stimulus_frame);
    if (stim_index >= data_.chaser_states_by_stimulus_frame.size()) {
        return result;
    }

    std::unordered_map<int32_t, size_t> latest_by_index;
    for (size_t idx : data_.chaser_states_by_stimulus_frame[stim_index]) {
        if (idx >= data_.chaser_states.size()) {
            continue;
        }
        const auto& src = data_.chaser_states[idx];
        ChaserState candidate;
        candidate.stimulus_frame_num = src.stimulus_frame_num;
        candidate.camera_frame_id = src.camera_frame_id;
        candidate.chaser_index = src.chaser_index;
        candidate.chaser_pos_x = src.chaser_pos_x;
        candidate.chaser_pos_y = src.chaser_pos_y;
        candidate.target_pos_x = src.target_pos_x;
        candidate.target_pos_y = src.target_pos_y;
        candidate.chaser_radius_px = src.chaser_radius_px;
        candidate.distance_to_target_px = src.distance_to_target_px;
        candidate.target_speed_px_per_s = src.target_speed_px_per_s;
        candidate.is_chasing = src.is_chasing != 0;
        candidate.timestamp_ns_session = src.timestamp_ns_session;
        candidate.texture_space = src.texture_space;
        candidate.coordinate_frame = src.coordinate_frame;
        candidate.coordinate_origin = src.coordinate_origin;
        candidate.stimulus_canvas_offset_x = src.stimulus_canvas_offset_x;
        candidate.stimulus_canvas_offset_y = src.stimulus_canvas_offset_y;
        candidate.has_stimulus_canvas_offset = src.has_stimulus_canvas_offset;
        candidate.chaser_camera_x = src.chaser_camera_x;
        candidate.chaser_camera_y = src.chaser_camera_y;
        candidate.target_camera_x = src.target_camera_x;
        candidate.target_camera_y = src.target_camera_y;
        candidate.has_camera_coords = src.has_camera_coords;
        candidate.chaser_rgba = src.chaser_rgba;
        candidate.has_chaser_rgba = src.has_chaser_rgba;
        candidate.behavior_mode = src.behavior_mode;
        candidate.has_behavior_mode = src.has_behavior_mode;
        candidate.enable_chase = src.enable_chase;
        candidate.has_enable_chase = src.has_enable_chase;
        candidate.enable_random_movement = src.enable_random_movement;
        candidate.has_enable_random_movement = src.has_enable_random_movement;

        int32_t key = candidate.chaser_index;
        auto it = latest_by_index.find(key);
        bool keep = true;
        if (it != latest_by_index.end()) {
            auto& existing = result[it->second];
            if (candidate.stimulus_frame_num < existing.stimulus_frame_num) {
                keep = false;
            } else if (candidate.stimulus_frame_num == existing.stimulus_frame_num &&
                       candidate.timestamp_ns_session <= existing.timestamp_ns_session) {
                keep = false;
            }
            if (keep) {
                existing = candidate;
            }
        } else if (keep) {
            latest_by_index[key] = result.size();
            result.push_back(candidate);
        }
    }
    return result;
}

std::vector<ZarrDetectionLoader::ChaserState>
ZarrDetectionLoader::getChaserInterpolatedStatesForCameraFrame(int32_t camera_frame) const {
    if (!data_.has_chaser_states_interpolated) {
        return {};
    }
    auto stim = resolveDirectStimulusFrame(camera_frame);
    if (!stim) {
        return {};
    }
    return getChaserInterpolatedStatesForStimulusFrame(*stim);
}

std::vector<ZarrDetectionLoader::ChaserState>
ZarrDetectionLoader::getChaserInterpolatedStatesForStimulusFrame(int32_t stimulus_frame) const {
    std::vector<ChaserState> result;
    if (!data_.has_chaser_states_interpolated || stimulus_frame < 0) {
        return result;
    }
    size_t stim_index = static_cast<size_t>(stimulus_frame);
    if (stim_index >= data_.chaser_states_interpolated_by_stimulus_frame.size()) {
        return result;
    }
    const auto& entries = data_.chaser_states_interpolated_by_stimulus_frame[stim_index];
    if (entries.empty()) {
        return result;
    }

    std::unordered_map<int32_t, size_t> latest_by_index;
    for (size_t idx : entries) {
        if (idx >= data_.chaser_states_interpolated.size()) {
            continue;
        }
        const auto& src = data_.chaser_states_interpolated[idx];
        ChaserState candidate;
        candidate.stimulus_frame_num = src.stimulus_frame_num;
        candidate.camera_frame_id = src.camera_frame_id;
        candidate.chaser_index = src.chaser_index;
        candidate.chaser_pos_x = src.chaser_pos_x;
        candidate.chaser_pos_y = src.chaser_pos_y;
        candidate.target_pos_x = src.target_pos_x;
        candidate.target_pos_y = src.target_pos_y;
        candidate.chaser_radius_px = src.chaser_radius_px;
        candidate.distance_to_target_px = src.distance_to_target_px;
        candidate.target_speed_px_per_s = src.target_speed_px_per_s;
        candidate.is_chasing = src.is_chasing != 0;
        candidate.timestamp_ns_session = src.timestamp_ns_session;
        candidate.texture_space = src.texture_space;
        candidate.coordinate_frame = src.coordinate_frame;
        candidate.coordinate_origin = src.coordinate_origin;
        candidate.stimulus_canvas_offset_x = src.stimulus_canvas_offset_x;
        candidate.stimulus_canvas_offset_y = src.stimulus_canvas_offset_y;
        candidate.has_stimulus_canvas_offset = src.has_stimulus_canvas_offset;
        candidate.chaser_camera_x = src.chaser_camera_x;
        candidate.chaser_camera_y = src.chaser_camera_y;
        candidate.target_camera_x = src.target_camera_x;
        candidate.target_camera_y = src.target_camera_y;
        candidate.has_camera_coords = src.has_camera_coords;
        candidate.chaser_rgba = src.chaser_rgba;
        candidate.has_chaser_rgba = src.has_chaser_rgba;
        candidate.behavior_mode = src.behavior_mode;
        candidate.has_behavior_mode = src.has_behavior_mode;
        candidate.enable_chase = src.enable_chase;
        candidate.has_enable_chase = src.has_enable_chase;
        candidate.enable_random_movement = src.enable_random_movement;
        candidate.has_enable_random_movement = src.has_enable_random_movement;

        int32_t key = candidate.chaser_index;
        auto it = latest_by_index.find(key);
        bool keep = true;
        if (it != latest_by_index.end()) {
            auto& existing = result[it->second];
            if (candidate.timestamp_ns_session < existing.timestamp_ns_session) {
                keep = false;
            } else if (candidate.timestamp_ns_session == existing.timestamp_ns_session &&
                       candidate.camera_frame_id <= existing.camera_frame_id) {
                keep = false;
            }
            if (keep) {
                existing = candidate;
            }
        } else if (keep) {
            latest_by_index[key] = result.size();
            result.push_back(candidate);
        }
    }
    return result;
}
