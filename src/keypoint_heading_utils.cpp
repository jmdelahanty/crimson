#include "keypoint_heading_utils.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <unordered_map>

namespace {

using json = nlohmann::json;

constexpr double kPi = 3.141592653589793238462643383279502884;

std::unordered_map<std::string, int> buildLabelIndex(
    const std::vector<std::string>& keypoint_labels) {
    std::unordered_map<std::string, int> label_to_index;
    for (size_t i = 0; i < keypoint_labels.size(); ++i) {
        label_to_index.emplace(keypoint_labels[i], static_cast<int>(i));
    }
    return label_to_index;
}

bool parsePointSpec(const json& expr,
                    const std::unordered_map<std::string, int>& label_to_index,
                    KeypointHeadingPointSpec& out_spec) {
    out_spec = KeypointHeadingPointSpec{};
    if (!expr.is_object()) {
        return false;
    }
    const std::string op = expr.value("op", "");
    if (op == "keypoint") {
        const std::string label = expr.value("label", "");
        auto it = label_to_index.find(label);
        if (label.empty() || it == label_to_index.end()) {
            return false;
        }
        out_spec.op = KeypointHeadingPointOp::Keypoint;
        out_spec.labels = {label};
        out_spec.indices = {it->second};
        out_spec.valid = true;
        return true;
    }
    if (op == "midpoint") {
        if (!expr.contains("labels") || !expr["labels"].is_array() ||
            expr["labels"].size() != 2) {
            return false;
        }
        std::vector<std::string> labels;
        std::vector<int> indices;
        labels.reserve(2);
        indices.reserve(2);
        for (const auto& label_json : expr["labels"]) {
            if (!label_json.is_string()) {
                return false;
            }
            const std::string label = label_json.get<std::string>();
            auto it = label_to_index.find(label);
            if (label.empty() || it == label_to_index.end()) {
                return false;
            }
            labels.push_back(label);
            indices.push_back(it->second);
        }
        out_spec.op = KeypointHeadingPointOp::Midpoint;
        out_spec.labels = std::move(labels);
        out_spec.indices = std::move(indices);
        out_spec.valid = true;
        return true;
    }
    return false;
}

KeypointHeadingComputationSpec parseHeadingSpecPayload(
    const json& payload,
    const std::unordered_map<std::string, int>& label_to_index,
    const std::string& source,
    bool legacy_fallback) {
    KeypointHeadingComputationSpec spec;
    spec.source = source;
    spec.legacy_fallback = legacy_fallback;

    if (!payload.is_object()) {
        return spec;
    }

    if (payload.contains("enabled") && payload["enabled"].is_boolean() &&
        payload["enabled"].get<bool>() == false) {
        spec.enabled = false;
        return spec;
    }

    KeypointHeadingPointSpec origin_spec;
    KeypointHeadingPointSpec from_spec;
    KeypointHeadingPointSpec to_spec;
    if (!payload.contains("origin") ||
        !parsePointSpec(payload["origin"], label_to_index, origin_spec) ||
        !payload.contains("direction_from") ||
        !parsePointSpec(payload["direction_from"], label_to_index, from_spec) ||
        !payload.contains("direction_to") ||
        !parsePointSpec(payload["direction_to"], label_to_index, to_spec)) {
        return spec;
    }

    if (!payload.contains("dependent_keypoints") ||
        !payload["dependent_keypoints"].is_array()) {
        return spec;
    }
    std::vector<std::string> dependent_labels;
    std::vector<int> dependent_indices;
    for (const auto& label_json : payload["dependent_keypoints"]) {
        if (!label_json.is_string()) {
            return KeypointHeadingComputationSpec{};
        }
        const std::string label = label_json.get<std::string>();
        auto it = label_to_index.find(label);
        if (label.empty() || it == label_to_index.end()) {
            return KeypointHeadingComputationSpec{};
        }
        dependent_labels.push_back(label);
        dependent_indices.push_back(it->second);
    }

    spec.available = true;
    spec.enabled = true;
    spec.origin = std::move(origin_spec);
    spec.direction_from = std::move(from_spec);
    spec.direction_to = std::move(to_spec);
    spec.dependent_labels = std::move(dependent_labels);
    spec.dependent_indices = std::move(dependent_indices);
    return spec;
}

KeypointHeadingComputationSpec buildLegacyFallbackSpec(
    const std::vector<std::string>& keypoint_labels) {
    int swim_index = -1;
    int left_index = -1;
    int right_index = -1;

    for (size_t i = 0; i < keypoint_labels.size(); ++i) {
        std::string lowered = keypoint_labels[i];
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        if (swim_index < 0 &&
            (lowered.find("swim") != std::string::npos ||
             lowered.find("bladder") != std::string::npos)) {
            swim_index = static_cast<int>(i);
        }
        if (left_index < 0 && lowered.find("left") != std::string::npos) {
            left_index = static_cast<int>(i);
        }
        if (right_index < 0 && lowered.find("right") != std::string::npos) {
            right_index = static_cast<int>(i);
        }
    }

    if (swim_index < 0 || left_index < 0 || right_index < 0) {
        return KeypointHeadingComputationSpec{};
    }

    KeypointHeadingComputationSpec spec;
    spec.available = true;
    spec.enabled = true;
    spec.legacy_fallback = true;
    spec.source = "legacy_3point";
    spec.origin.op = KeypointHeadingPointOp::Midpoint;
    spec.origin.labels = {keypoint_labels[static_cast<size_t>(left_index)],
                          keypoint_labels[static_cast<size_t>(right_index)]};
    spec.origin.indices = {left_index, right_index};
    spec.origin.valid = true;
    spec.direction_from.op = KeypointHeadingPointOp::Keypoint;
    spec.direction_from.labels = {keypoint_labels[static_cast<size_t>(swim_index)]};
    spec.direction_from.indices = {swim_index};
    spec.direction_from.valid = true;
    spec.direction_to = spec.origin;
    spec.dependent_labels = {
        keypoint_labels[static_cast<size_t>(swim_index)],
        keypoint_labels[static_cast<size_t>(left_index)],
        keypoint_labels[static_cast<size_t>(right_index)],
    };
    spec.dependent_indices = {swim_index, left_index, right_index};
    return spec;
}

std::optional<json> getPoseSchemaHeadingSpec(const json& run_attrs) {
    if (!run_attrs.contains("pose_schema") || !run_attrs["pose_schema"].is_object()) {
        return std::nullopt;
    }
    const auto& pose_schema = run_attrs["pose_schema"];
    if (!pose_schema.contains("metadata") || !pose_schema["metadata"].is_object()) {
        return std::nullopt;
    }
    const auto& metadata = pose_schema["metadata"];
    if (!metadata.contains("heading_computation") ||
        !metadata["heading_computation"].is_object()) {
        return std::nullopt;
    }
    return metadata["heading_computation"];
}

}  // namespace

KeypointHeadingComputationSpec resolveKeypointHeadingComputationSpec(
    const json* run_attrs,
    const std::vector<std::string>& keypoint_labels) {
    const auto label_to_index = buildLabelIndex(keypoint_labels);
    if (run_attrs != nullptr) {
        if (run_attrs->contains("heading_computation_override") &&
            (*run_attrs)["heading_computation_override"].is_object()) {
            return parseHeadingSpecPayload((*run_attrs)["heading_computation_override"],
                                           label_to_index,
                                           "run_override",
                                           false);
        }
        if (const auto pose_heading = getPoseSchemaHeadingSpec(*run_attrs);
            pose_heading.has_value()) {
            return parseHeadingSpecPayload(*pose_heading,
                                           label_to_index,
                                           "pose_schema",
                                           false);
        }
        if (run_attrs->contains("heading_computation") &&
            (*run_attrs)["heading_computation"].is_object()) {
            return parseHeadingSpecPayload((*run_attrs)["heading_computation"],
                                           label_to_index,
                                           "deprecated_run_alias",
                                           false);
        }
    }
    return buildLegacyFallbackSpec(keypoint_labels);
}

bool evaluateKeypointHeadingPoint(
    const KeypointHeadingPointSpec& point_spec,
    const std::vector<std::array<double, 2>>& positions,
    std::array<double, 2>& out_point) {
    if (!point_spec.valid) {
        return false;
    }
    if (point_spec.op == KeypointHeadingPointOp::Keypoint) {
        if (point_spec.indices.size() != 1 || point_spec.indices[0] < 0 ||
            static_cast<size_t>(point_spec.indices[0]) >= positions.size()) {
            return false;
        }
        const auto& point = positions[static_cast<size_t>(point_spec.indices[0])];
        if (!std::isfinite(point[0]) || !std::isfinite(point[1])) {
            return false;
        }
        out_point = point;
        return true;
    }
    if (point_spec.op == KeypointHeadingPointOp::Midpoint) {
        if (point_spec.indices.size() != 2) {
            return false;
        }
        const int a = point_spec.indices[0];
        const int b = point_spec.indices[1];
        if (a < 0 || b < 0 || static_cast<size_t>(a) >= positions.size() ||
            static_cast<size_t>(b) >= positions.size()) {
            return false;
        }
        const auto& pa = positions[static_cast<size_t>(a)];
        const auto& pb = positions[static_cast<size_t>(b)];
        if (!std::isfinite(pa[0]) || !std::isfinite(pa[1]) ||
            !std::isfinite(pb[0]) || !std::isfinite(pb[1])) {
            return false;
        }
        out_point = {(pa[0] + pb[0]) * 0.5, (pa[1] + pb[1]) * 0.5};
        return true;
    }
    return false;
}

bool evaluateKeypointHeadingOrigin(
    const KeypointHeadingComputationSpec& spec,
    const std::vector<std::array<double, 2>>& positions,
    std::array<double, 2>& out_origin) {
    return spec.available && spec.enabled &&
           evaluateKeypointHeadingPoint(spec.origin, positions, out_origin);
}

bool evaluateKeypointHeadingDegrees(
    const KeypointHeadingComputationSpec& spec,
    const std::vector<std::array<double, 2>>& positions,
    double& out_heading_deg,
    std::array<double, 2>* out_origin) {
    if (!(spec.available && spec.enabled)) {
        return false;
    }

    std::array<double, 2> origin{};
    std::array<double, 2> from{};
    std::array<double, 2> to{};
    if (!evaluateKeypointHeadingPoint(spec.origin, positions, origin) ||
        !evaluateKeypointHeadingPoint(spec.direction_from, positions, from) ||
        !evaluateKeypointHeadingPoint(spec.direction_to, positions, to)) {
        return false;
    }

    const double dx = to[0] - from[0];
    const double dy = to[1] - from[1];
    if (!std::isfinite(dx) || !std::isfinite(dy) || std::hypot(dx, dy) == 0.0) {
        return false;
    }

    out_heading_deg = std::atan2(-dy, dx) * 180.0 / kPi;
    if (out_origin != nullptr) {
        *out_origin = origin;
    }
    return true;
}

bool headingComputationDependsOnEditedIndices(
    const KeypointHeadingComputationSpec& spec,
    const std::vector<int>& edited_indices) {
    if (!(spec.available && spec.enabled) || edited_indices.empty()) {
        return false;
    }
    for (int edited_index : edited_indices) {
        if (std::find(spec.dependent_indices.begin(),
                      spec.dependent_indices.end(),
                      edited_index) != spec.dependent_indices.end()) {
            return true;
        }
    }
    return false;
}

std::vector<int> collectEditedKeypointIndices(
    const std::vector<std::array<float, 2>>& baseline_positions,
    const std::vector<std::array<float, 2>>& current_positions,
    float epsilon) {
    std::vector<int> indices;
    const size_t count = std::min(baseline_positions.size(), current_positions.size());
    for (size_t i = 0; i < count; ++i) {
        const auto& baseline = baseline_positions[i];
        const auto& current = current_positions[i];
        const bool baseline_finite = std::isfinite(baseline[0]) && std::isfinite(baseline[1]);
        const bool current_finite = std::isfinite(current[0]) && std::isfinite(current[1]);
        if (baseline_finite != current_finite) {
            indices.push_back(static_cast<int>(i));
            continue;
        }
        if (!baseline_finite) {
            continue;
        }
        if (std::abs(baseline[0] - current[0]) > epsilon ||
            std::abs(baseline[1] - current[1]) > epsilon) {
            indices.push_back(static_cast<int>(i));
        }
    }
    return indices;
}

std::vector<std::array<double, 2>> convertKeypointPositionsToDouble(
    const std::vector<std::array<float, 2>>& positions) {
    std::vector<std::array<double, 2>> converted;
    converted.reserve(positions.size());
    for (const auto& point : positions) {
        converted.push_back({static_cast<double>(point[0]),
                             static_cast<double>(point[1])});
    }
    return converted;
}
