#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace crimson::zarr {

enum class SubjectMaskV1Extent {
  Rows,
  FrameBoundaries,
  Channels,
  Two,
  Four,
  Height,
  Width,
};

struct SubjectMaskV1ArrayDeclaration {
  std::string_view path;
  std::string_view dtype;
  size_t rank = 0;
  std::array<SubjectMaskV1Extent, 4> extents{};
};

inline constexpr std::array<SubjectMaskV1ArrayDeclaration, 13>
    kSubjectMaskV1ArrayDeclarations = {{
        {"source_crop_row_ids", "int64", 1, {SubjectMaskV1Extent::Rows}},
        {"instance_key", "uint64", 1, {SubjectMaskV1Extent::Rows}},
        {"source_acquisition_frame_index",
         "int64",
         1,
         {SubjectMaskV1Extent::Rows}},
        {"frame_row_offsets",
         "int64",
         1,
         {SubjectMaskV1Extent::FrameBoundaries}},
        {"source_crop_xywh",
         "float32",
         2,
         {SubjectMaskV1Extent::Rows, SubjectMaskV1Extent::Four}},
        {"masks_roi",
         "uint8",
         4,
         {SubjectMaskV1Extent::Rows, SubjectMaskV1Extent::Channels,
          SubjectMaskV1Extent::Height, SubjectMaskV1Extent::Width}},
        {"available_channels", "bool", 1, {SubjectMaskV1Extent::Channels}},
        {"metrics/mask_present",
         "bool",
         2,
         {SubjectMaskV1Extent::Rows, SubjectMaskV1Extent::Channels}},
        {"metrics/area_px",
         "float32",
         2,
         {SubjectMaskV1Extent::Rows, SubjectMaskV1Extent::Channels}},
        {"metrics/centroid_xy",
         "float32",
         3,
         {SubjectMaskV1Extent::Rows, SubjectMaskV1Extent::Channels,
          SubjectMaskV1Extent::Two}},
        {"metrics/centroid_valid",
         "bool",
         2,
         {SubjectMaskV1Extent::Rows, SubjectMaskV1Extent::Channels}},
        {"metrics/bbox_xyxy",
         "float32",
         3,
         {SubjectMaskV1Extent::Rows, SubjectMaskV1Extent::Channels,
          SubjectMaskV1Extent::Four}},
        {"metrics/bbox_valid",
         "bool",
         2,
         {SubjectMaskV1Extent::Rows, SubjectMaskV1Extent::Channels}},
    }};

struct SubjectMaskV1ManifestSummary {
  std::string run_id;
  std::string payload_digest;
  std::string manifest_digest;
  std::string metadata_digest;
  std::string metadata_digest_scope;
  bool selector_eligible = false;
  size_t frame_count = 0;
  size_t row_count = 0;
  size_t channel_count = 0;
  size_t mask_height = 0;
  size_t mask_width = 0;
  std::vector<std::string> component_labels;
};

std::vector<size_t>
ExpectedSubjectMaskV1Shape(const SubjectMaskV1ArrayDeclaration &declaration,
                           const SubjectMaskV1ManifestSummary &summary);

bool ValidateSubjectMaskV1Manifest(const nlohmann::json &manifest,
                                   std::string_view requested_run,
                                   SubjectMaskV1ManifestSummary *summary,
                                   std::string *error = nullptr);

bool ValidateSubjectMaskV1FrameIndex(const std::vector<int64_t> &offsets,
                                     const std::vector<int64_t> &frames,
                                     size_t frame_count, size_t row_count,
                                     std::string *error = nullptr);

bool ValidateSubjectMaskV1InstanceKeys(const std::vector<uint64_t> &keys,
                                       size_t row_count,
                                       std::string *error = nullptr);

} // namespace crimson::zarr
