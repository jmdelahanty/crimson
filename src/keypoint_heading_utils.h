#pragma once

#include <array>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

enum class KeypointHeadingPointOp {
    None = 0,
    Keypoint,
    Midpoint,
};

struct KeypointHeadingPointSpec {
    KeypointHeadingPointOp op = KeypointHeadingPointOp::None;
    std::vector<std::string> labels;
    std::vector<int> indices;
    bool valid = false;
};

struct KeypointHeadingComputationSpec {
    bool available = false;
    bool enabled = false;
    bool legacy_fallback = false;
    std::string source;
    KeypointHeadingPointSpec origin;
    KeypointHeadingPointSpec direction_from;
    KeypointHeadingPointSpec direction_to;
    std::vector<std::string> dependent_labels;
    std::vector<int> dependent_indices;
};

KeypointHeadingComputationSpec resolveKeypointHeadingComputationSpec(
    const nlohmann::json* run_attrs,
    const std::vector<std::string>& keypoint_labels);

bool evaluateKeypointHeadingPoint(
    const KeypointHeadingPointSpec& point_spec,
    const std::vector<std::array<double, 2>>& positions,
    std::array<double, 2>& out_point);

bool evaluateKeypointHeadingOrigin(
    const KeypointHeadingComputationSpec& spec,
    const std::vector<std::array<double, 2>>& positions,
    std::array<double, 2>& out_origin);

bool evaluateKeypointHeadingDegrees(
    const KeypointHeadingComputationSpec& spec,
    const std::vector<std::array<double, 2>>& positions,
    double& out_heading_deg,
    std::array<double, 2>* out_origin = nullptr);

bool headingComputationDependsOnEditedIndices(
    const KeypointHeadingComputationSpec& spec,
    const std::vector<int>& edited_indices);

std::vector<int> collectEditedKeypointIndices(
    const std::vector<std::array<float, 2>>& baseline_positions,
    const std::vector<std::array<float, 2>>& current_positions,
    float epsilon = 0.01f);

std::vector<std::array<double, 2>> convertKeypointPositionsToDouble(
    const std::vector<std::array<float, 2>>& positions);
