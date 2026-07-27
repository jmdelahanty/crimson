#include "zarr/tensorstore_analysis_crop_geometry_repository.h"

#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "zarr/archive_context_internal.h"

namespace crimson::zarr {
namespace ts = tensorstore;
using json = nlohmann::json;

namespace {

bool ValidRunName(const std::string& run_name) {
  return !run_name.empty() && run_name != "." && run_name != ".." &&
         run_name.find('/') == std::string::npos;
}

std::string NormalizeRunName(std::string run_name, const std::string& group) {
  const std::string prefix = group + "/";
  if (run_name.rfind(prefix, 0) == 0) {
    run_name.erase(0, prefix.size());
  }
  return ValidRunName(run_name) ? run_name : std::string{};
}

std::string LatestRun(const ArchiveContext::Impl& archive,
                      const std::string& group) {
  const auto attributes = internal::ReadArchiveAttributes(archive, group);
  if (!attributes) {
    return {};
  }
  constexpr std::array<const char*, 6> latest_keys = {
      "latest",          "latest_completed",
      "latest_complete", "latest_success",
      "latest_any",      "refined_subject_mask_review_status_latest"};
  for (const char* key : latest_keys) {
    const auto found = attributes->find(key);
    if (found != attributes->end() && found->is_string()) {
      const std::string run_name =
          NormalizeRunName(found->get<std::string>(), group);
      if (!run_name.empty()) {
        return run_name;
      }
    }
  }
  return {};
}

std::string CropRunFromLatestLineage(const ArchiveContext::Impl& archive,
                                     const std::string& group) {
  const std::string lineage_run = LatestRun(archive, group);
  if (lineage_run.empty()) {
    return {};
  }
  const auto attributes =
      internal::ReadArchiveAttributes(archive, group + "/" + lineage_run);
  if (!attributes) {
    return {};
  }
  const auto source_crop_run = attributes->find("source_crop_run");
  if (source_crop_run == attributes->end() || !source_crop_run->is_string()) {
    return {};
  }
  return NormalizeRunName(source_crop_run->get<std::string>(), "crop_runs");
}

std::string DiscoverCropRun(const ArchiveContext::Impl& archive) {
  std::string run_name = LatestRun(archive, "crop_runs");
  if (!run_name.empty()) {
    return run_name;
  }

  // Subject masks are the coordinate lineage shared by masks, subject shape,
  // and eye geometry, so prefer them when independently selected products
  // reference a different crop run.
  constexpr std::array<const char*, 3> lineage_groups = {
      "refined_subject_masks_runs", "refined_keypoints_runs", "keypoints_runs"};
  for (const char* group : lineage_groups) {
    run_name = CropRunFromLatestLineage(archive, group);
    if (!run_name.empty()) {
      return run_name;
    }
  }
  return {};
}

template <typename Source>
bool ReadIntegerArray(const ArchiveContext::Impl& archive,
                      const std::string& path, std::vector<int64_t>* output) {
  const auto spec = internal::MakeReadOnlyArraySpec(archive, path);
  if (!spec) {
    return false;
  }
  auto open_result =
      ts::Open<Source, 1>(*spec, ts::OpenMode::open, ts::ReadWriteMode::read,
                          archive.context)
          .result();
  if (!open_result.ok()) {
    return false;
  }
  auto read_result = ts::Read(*open_result).result();
  if (!read_result.ok() || read_result->rank() != 1) {
    return false;
  }
  const size_t count = static_cast<size_t>(read_result->shape()[0]);
  const Source* values = static_cast<const Source*>(read_result->data());
  output->resize(count);
  for (size_t index = 0; index < count; ++index) {
    (*output)[index] = static_cast<int64_t>(values[index]);
  }
  return true;
}

bool ReadFrameIndices(const ArchiveContext::Impl& archive,
                      const std::string& path, std::vector<int64_t>* output) {
  return ReadIntegerArray<int64_t>(archive, path, output) ||
         ReadIntegerArray<uint64_t>(archive, path, output) ||
         ReadIntegerArray<int32_t>(archive, path, output) ||
         ReadIntegerArray<uint32_t>(archive, path, output) ||
         ReadIntegerArray<int16_t>(archive, path, output) ||
         ReadIntegerArray<uint16_t>(archive, path, output) ||
         ReadIntegerArray<int8_t>(archive, path, output) ||
         ReadIntegerArray<uint8_t>(archive, path, output);
}

template <typename Source>
bool ReadMatrix(const ArchiveContext::Impl& archive, const std::string& path,
                size_t minimum_columns,
                std::vector<std::vector<double>>* output) {
  const auto spec = internal::MakeReadOnlyArraySpec(archive, path);
  if (!spec) {
    return false;
  }
  auto open_result =
      ts::Open<Source, 2>(*spec, ts::OpenMode::open, ts::ReadWriteMode::read,
                          archive.context)
          .result();
  if (!open_result.ok()) {
    return false;
  }
  auto read_result = ts::Read(*open_result).result();
  if (!read_result.ok() || read_result->rank() != 2 ||
      read_result->shape()[1] < static_cast<ts::Index>(minimum_columns)) {
    return false;
  }
  const size_t rows = static_cast<size_t>(read_result->shape()[0]);
  const size_t columns = static_cast<size_t>(read_result->shape()[1]);
  const Source* values = static_cast<const Source*>(read_result->data());
  output->assign(rows, std::vector<double>(columns));
  for (size_t row = 0; row < rows; ++row) {
    for (size_t column = 0; column < columns; ++column) {
      (*output)[row][column] =
          static_cast<double>(values[row * columns + column]);
    }
  }
  return true;
}

bool ReadNumericMatrix(const ArchiveContext::Impl& archive,
                       const std::string& path, size_t minimum_columns,
                       std::vector<std::vector<double>>* output) {
  return ReadMatrix<double>(archive, path, minimum_columns, output) ||
         ReadMatrix<float>(archive, path, minimum_columns, output) ||
         ReadMatrix<int64_t>(archive, path, minimum_columns, output) ||
         ReadMatrix<int32_t>(archive, path, minimum_columns, output);
}

bool ReadRoiSize(const ArchiveContext::Impl& archive,
                 const std::string& run_base, int* output_width,
                 int* output_height) {
  if (auto attributes = internal::ReadArchiveAttributes(archive, run_base)) {
    const auto found = attributes->find("roi_size");
    if (found != attributes->end() && found->is_array() && found->size() >= 2 &&
        (*found)[0].is_number() && (*found)[1].is_number()) {
      const double height = (*found)[0].get<double>();
      const double width = (*found)[1].get<double>();
      if (std::isfinite(width) && std::isfinite(height) && width >= 1.0 &&
          height >= 1.0 && width <= std::numeric_limits<int>::max() &&
          height <= std::numeric_limits<int>::max()) {
        *output_width = static_cast<int>(std::llround(width));
        *output_height = static_cast<int>(std::llround(height));
        return true;
      }
    }
  }

  const auto spec =
      internal::MakeReadOnlyArraySpec(archive, run_base + "/roi_images");
  if (!spec) {
    return false;
  }
  auto rank3 = ts::Open<uint8_t, 3>(*spec, ts::OpenMode::open,
                                    ts::ReadWriteMode::read, archive.context)
                   .result();
  if (rank3.ok()) {
    const auto shape = rank3->domain().shape();
    if (shape[1] > 0 && shape[2] > 0 &&
        shape[1] <= std::numeric_limits<int>::max() &&
        shape[2] <= std::numeric_limits<int>::max()) {
      *output_height = static_cast<int>(shape[1]);
      *output_width = static_cast<int>(shape[2]);
      return true;
    }
  }
  auto rank4 = ts::Open<uint8_t, 4>(*spec, ts::OpenMode::open,
                                    ts::ReadWriteMode::read, archive.context)
                   .result();
  if (rank4.ok()) {
    const auto shape = rank4->domain().shape();
    if (shape[1] > 0 && shape[2] > 0 &&
        shape[1] <= std::numeric_limits<int>::max() &&
        shape[2] <= std::numeric_limits<int>::max()) {
      *output_height = static_cast<int>(shape[1]);
      *output_width = static_cast<int>(shape[2]);
      return true;
    }
  }
  return false;
}

}  // namespace

std::unique_ptr<AnalysisCropGeometryRepository>
OpenAnalysisCropGeometryRepository(
    const std::shared_ptr<ArchiveContext>& archive,
    const std::string& requested_run, std::string* error_message) {
  if (!archive || !archive->impl_) {
    internal::SetArchiveError(error_message, "Archive context is not open");
    return nullptr;
  }
  const auto& impl = *archive->impl_;

  std::string run_name;
  if (!requested_run.empty()) {
    run_name = NormalizeRunName(requested_run, "crop_runs");
    if (run_name.empty()) {
      internal::SetArchiveError(error_message,
                                "Requested crop run name is invalid");
      return nullptr;
    }
  } else {
    run_name = DiscoverCropRun(impl);
  }
  if (run_name.empty()) {
    internal::SetArchiveError(
        error_message,
        "No valid crop run was requested, selected by crop_runs, or "
        "referenced by the maintained mask/keypoint lineage");
    return nullptr;
  }

  const std::string run_base = "crop_runs/" + run_name;
  std::vector<int64_t> frame_indices;
  std::vector<std::vector<double>> offsets;
  if (!ReadFrameIndices(impl, run_base + "/frame_indices", &frame_indices) ||
      frame_indices.empty()) {
    internal::SetArchiveError(error_message,
                              "Crop run has no readable frame_indices");
    return nullptr;
  }
  if (!ReadNumericMatrix(impl, run_base + "/roi_coordinates_full", 2,
                         &offsets) ||
      offsets.size() != frame_indices.size()) {
    internal::SetArchiveError(
        error_message,
        "Crop run roi_coordinates_full does not match frame_indices");
    return nullptr;
  }

  AnalysisCropGeometryDescriptor descriptor;
  descriptor.run_name = run_name;
  if (!ReadRoiSize(impl, run_base, &descriptor.output_width,
                   &descriptor.output_height)) {
    internal::SetArchiveError(
        error_message,
        "Crop run has neither a valid roi_size attribute nor readable "
        "roi_images metadata");
    return nullptr;
  }

  std::vector<std::vector<double>> boxes;
  const bool boxes_available =
      ReadNumericMatrix(impl, run_base + "/bbox_norm_coords", 4, &boxes) &&
      boxes.size() == frame_indices.size();

  std::vector<AnalysisCropGeometryRow> rows;
  rows.reserve(frame_indices.size());
  for (size_t index = 0; index < frame_indices.size(); ++index) {
    if (frame_indices[index] < 0 || !std::isfinite(offsets[index][0]) ||
        !std::isfinite(offsets[index][1])) {
      internal::SetArchiveError(
          error_message, "Crop run contains invalid frame or ROI coordinates");
      return nullptr;
    }
    AnalysisCropGeometryRow row;
    row.camera_frame = frame_indices[index];
    row.roi_index = static_cast<int64_t>(index);
    row.offset_x = offsets[index][0];
    row.offset_y = offsets[index][1];
    if (boxes_available) {
      std::array<double, 4> box = {boxes[index][0], boxes[index][1],
                                   boxes[index][2], boxes[index][3]};
      if (std::all_of(box.begin(), box.end(),
                      [](double value) { return std::isfinite(value); }) &&
          box[2] > 0.0 && box[3] > 0.0) {
        row.normalized_detection_cxcywh = box;
      }
    }
    rows.push_back(std::move(row));
  }

  return MakeAnalysisCropGeometryRepository(std::move(descriptor),
                                            std::move(rows));
}

}  // namespace crimson::zarr
