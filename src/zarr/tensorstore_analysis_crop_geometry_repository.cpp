#include "zarr/tensorstore_analysis_crop_geometry_repository.h"

#include <tensorstore/index_space/dim_expression.h>
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
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "zarr/archive_context_internal.h"
#include "zarr/crop_geometry_contract.h"
#include "zarr/zarr_metadata_equivalence.h"

namespace crimson::zarr {
namespace ts = tensorstore;
using json = nlohmann::json;

namespace {

struct ArrayDeclaration {
  const char *relative_path;
  const char *dtype;
  size_t rank;
  size_t columns;
};

constexpr ArrayDeclaration kCoordinateAwareDeclarations[] = {
    {"bbox_img_xyxy", "float32", 2, 4},
    {"bbox_norm_coords", "float32", 2, 4},
    {"bbox_roi_xyxy", "float32", 2, 4},
    {"centers_img_xy", "float32", 2, 2},
    {"frame_indices", "int64", 1, 1},
    {"frame_row_offsets", "int64", 1, 1},
    {"instance_key", "uint64", 1, 1},
    {"roi_coordinates_full", "int32", 2, 2},
    {"roi_sizes_full", "int32", 2, 2},
    {"source_acquisition_frame_index", "int64", 1, 1},
    {"source_crop_xywh", "float32", 2, 4},
    {"source_refined_row_ids", "int64", 1, 1},
    {"source_row_signature", "uint8", 2, 32},
};

const json *ConsolidatedEntry(const json &root, const std::string &path) {
  try {
    const auto &metadata = root.at("consolidated_metadata").at("metadata");
    const auto found = metadata.find(path);
    return found == metadata.end() ? nullptr : &*found;
  } catch (const json::exception &) {
    return nullptr;
  }
}

template <typename T, ts::DimensionIndex Rank>
std::optional<ts::TensorStore<T, Rank>>
OpenExact(const ArchiveContext::Impl &archive, const json &root,
          const std::string &path, std::string *error) {
  auto spec = internal::MakeReadOnlyArraySpec(archive, path);
  const auto *metadata = ConsolidatedEntry(root, path);
  if (!spec || !metadata) {
    internal::SetArchiveError(error, "Missing exact array metadata: " + path);
    return std::nullopt;
  }
  (*spec)["metadata"] = *metadata;
  auto opened = ts::Open<T, Rank>(
                    *spec, ts::OpenMode::open | ts::OpenMode::assume_metadata,
                    ts::ReadWriteMode::read, archive.context)
                    .result();
  if (!opened.ok()) {
    internal::SetArchiveError(error, path + ": " + opened.status().ToString());
    return std::nullopt;
  }
  return *opened;
}

template <typename T, ts::DimensionIndex Rank>
bool ReadExact(const ts::TensorStore<T, Rank> &store, std::vector<T> *output,
               std::string *error) {
  auto read = ts::Read(store).result();
  if (!read.ok()) {
    internal::SetArchiveError(error, read.status().ToString());
    return false;
  }
  size_t count = 1;
  for (const auto extent : read->shape()) {
    count *= static_cast<size_t>(extent);
  }
  const T *values = static_cast<const T *>(read->data());
  output->assign(values, values + count);
  return true;
}

bool ValidateCoordinateAwareDeclarations(
    const json &root, const std::string &base,
    const CropGeometryManifestSummary &manifest, std::string *error) {
  try {
    if (root.at("zarr_format") != 3 || root.at("node_type") != "group" ||
        root.at("consolidated_metadata").at("kind") != "inline") {
      internal::SetArchiveError(
          error, "Crop archive lacks inline Zarr v3 consolidated metadata");
      return false;
    }
    for (const auto &declaration : kCoordinateAwareDeclarations) {
      const std::string path = base + "/" + declaration.relative_path;
      const auto *metadata = ConsolidatedEntry(root, path);
      if (!metadata) {
        internal::SetArchiveError(
            error, "Missing consolidated crop array declaration: " + path);
        return false;
      }
      const auto shape = metadata->at("shape").get<std::vector<size_t>>();
      const size_t expected_rows =
          std::string_view(declaration.relative_path) == "frame_row_offsets"
              ? manifest.frame_count + 1
              : manifest.instance_count;
      if (metadata->value("node_type", "") != "array" ||
          metadata->contains("consolidated_metadata") ||
          metadata->value("data_type", "") != declaration.dtype ||
          shape.size() != declaration.rank || shape[0] != expected_rows ||
          (declaration.rank == 2 && shape[1] != declaration.columns)) {
        internal::SetArchiveError(error, "Crop array schema mismatch: " + path);
        return false;
      }
    }
    return true;
  } catch (const json::exception &exception) {
    internal::SetArchiveError(error,
                              "Invalid coordinate-aware crop metadata: " +
                                  std::string(exception.what()));
    return false;
  }
}

bool ValidateOffsets(const std::vector<int64_t> &frame_indices,
                     const std::vector<int64_t> &offsets,
                     const CropGeometryManifestSummary &manifest,
                     std::string *error) {
  if (offsets.size() != manifest.frame_count + 1 || offsets.empty() ||
      offsets.front() != 0 ||
      offsets.back() != static_cast<int64_t>(manifest.instance_count) ||
      !std::is_sorted(offsets.begin(), offsets.end()) ||
      frame_indices.size() != manifest.instance_count ||
      !std::is_sorted(frame_indices.begin(), frame_indices.end())) {
    internal::SetArchiveError(error,
                              "Crop frame_row_offsets invariants failed");
    return false;
  }
  for (size_t frame = 0; frame < manifest.frame_count; ++frame) {
    const auto first = offsets[frame];
    const auto last = offsets[frame + 1];
    if (first < 0 || last < first ||
        last > static_cast<int64_t>(frame_indices.size())) {
      internal::SetArchiveError(error,
                                "Crop frame_row_offsets range is invalid");
      return false;
    }
    for (int64_t row = first; row < last; ++row) {
      if (frame_indices[static_cast<size_t>(row)] !=
          static_cast<int64_t>(frame)) {
        internal::SetArchiveError(
            error, "Crop frame_row_offsets disagree with frame_indices");
        return false;
      }
    }
  }
  return true;
}

std::unique_ptr<AnalysisCropGeometryRepository>
OpenCoordinateAwareCrop(const ArchiveContext::Impl &archive, const json &root,
                        const json &direct, const std::string &run_name,
                        const std::string &run_base, std::string *error) {
  const auto *consolidated = ConsolidatedEntry(root, run_base);
  if (!consolidated || !internal::EquivalentDirectAndConsolidatedZarrNode(
                           direct, *consolidated)) {
    internal::SetArchiveError(
        error, "Crop direct and consolidated run metadata disagree");
    return nullptr;
  }
  CropGeometryManifestSummary manifest;
  try {
    if (!ValidateCropGeometryRunManifest(
            consolidated->at("attributes").at("run_manifest"), run_name,
            &manifest, error)) {
      return nullptr;
    }
  } catch (const json::exception &exception) {
    internal::SetArchiveError(error, "Crop run_manifest is missing: " +
                                         std::string(exception.what()));
    return nullptr;
  }
  if (!ValidateCoordinateAwareDeclarations(root, run_base, manifest, error)) {
    return nullptr;
  }

  auto frame_store =
      OpenExact<int64_t, 1>(archive, root, run_base + "/frame_indices", error);
  auto offset_store = OpenExact<int64_t, 1>(
      archive, root, run_base + "/frame_row_offsets", error);
  auto roi_store = OpenExact<int32_t, 2>(
      archive, root, run_base + "/roi_coordinates_full", error);
  auto size_store =
      OpenExact<int32_t, 2>(archive, root, run_base + "/roi_sizes_full", error);
  auto box_store =
      OpenExact<float, 2>(archive, root, run_base + "/bbox_norm_coords", error);
  auto roi_box_store =
      OpenExact<float, 2>(archive, root, run_base + "/bbox_roi_xyxy", error);
  auto key_store =
      OpenExact<uint64_t, 1>(archive, root, run_base + "/instance_key", error);
  if (!frame_store || !offset_store || !roi_store || !size_store ||
      !box_store || !roi_box_store || !key_store) {
    return nullptr;
  }
  AnalysisCropGeometryDescriptor descriptor;
  descriptor.run_name = run_name;
  descriptor.run_manifest_digest = manifest.payload_digest;
  descriptor.crop_policy_digest = manifest.crop_policy_digest;
  descriptor.source_refined_run = manifest.source_refined_run;
  descriptor.source_refined_manifest_digest =
      manifest.source_refined_manifest_digest;
  descriptor.source_pixel_authority_manifest_digest =
      manifest.source_pixel_authority_manifest_digest;
  descriptor.output_width = static_cast<int>(manifest.output_width);
  descriptor.output_height = static_cast<int>(manifest.output_height);
  descriptor.row_count = manifest.instance_count;
  descriptor.camera_frame_count = manifest.frame_count;
  descriptor.source_width = manifest.source_width;
  descriptor.source_height = manifest.source_height;
  descriptor.consolidated_metadata = true;
  descriptor.coordinate_catalog_validated =
      manifest.coordinate_catalog_validated;

  AnalysisCropGeometryColumns columns;
  if (!ReadExact(*frame_store, &columns.frame_indices, error) ||
      !ReadExact(*offset_store, &columns.frame_row_offsets, error) ||
      !ValidateOffsets(columns.frame_indices, columns.frame_row_offsets,
                       manifest, error)) {
    return nullptr;
  }

  std::vector<int32_t> roi_coordinates;
  if (!ReadExact(*roi_store, &roi_coordinates, error) ||
      roi_coordinates.size() != manifest.instance_count * 2) {
    if (error && error->empty()) {
      *error = "Crop roi_coordinates_full has an inconsistent shape";
    }
    return nullptr;
  }
  columns.offsets_xy.resize(manifest.instance_count);
  for (size_t index = 0; index < manifest.instance_count; ++index) {
    columns.offsets_xy[index] = {
        static_cast<double>(roi_coordinates[index * 2]),
        static_cast<double>(roi_coordinates[index * 2 + 1])};
  }
  std::vector<int32_t>{}.swap(roi_coordinates);

  std::vector<int32_t> roi_sizes;
  if (!ReadExact(*size_store, &roi_sizes, error) ||
      roi_sizes.size() != manifest.instance_count * 2) {
    if (error && error->empty()) {
      *error = "Crop roi_sizes_full has an inconsistent shape";
    }
    return nullptr;
  }
  for (size_t index = 0; index < manifest.instance_count; ++index) {
    if (roi_sizes[index * 2] != descriptor.output_width ||
        roi_sizes[index * 2 + 1] != descriptor.output_height) {
      internal::SetArchiveError(
          error, "Crop roi_sizes_full disagrees with the fixed policy size");
      return nullptr;
    }
  }
  std::vector<int32_t>{}.swap(roi_sizes);

  std::vector<float> boxes;
  if (!ReadExact(*box_store, &boxes, error) ||
      boxes.size() != manifest.instance_count * 4) {
    if (error && error->empty()) {
      *error = "Crop bbox_norm_coords has an inconsistent shape";
    }
    return nullptr;
  }
  columns.normalized_detection_cxcywh.resize(manifest.instance_count);
  for (size_t index = 0; index < manifest.instance_count; ++index) {
    std::array<double, 4> box = {static_cast<double>(boxes[index * 4]),
                                 static_cast<double>(boxes[index * 4 + 1]),
                                 static_cast<double>(boxes[index * 4 + 2]),
                                 static_cast<double>(boxes[index * 4 + 3])};
    if (!std::all_of(box.begin(), box.end(),
                     [](double value) { return std::isfinite(value); }) ||
        box[2] <= 0.0 || box[3] <= 0.0) {
      internal::SetArchiveError(error,
                                "Crop bbox_norm_coords contains invalid data");
      return nullptr;
    }
    columns.normalized_detection_cxcywh[index] = box;
  }
  std::vector<float>{}.swap(boxes);

  std::vector<float> roi_boxes;
  if (!ReadExact(*roi_box_store, &roi_boxes, error) ||
      roi_boxes.size() != manifest.instance_count * 4) {
    if (error && error->empty()) {
      *error = "Crop bbox_roi_xyxy has an inconsistent shape";
    }
    return nullptr;
  }
  columns.roi_bbox_xyxy.resize(manifest.instance_count);
  for (size_t index = 0; index < manifest.instance_count; ++index) {
    std::array<double, 4> roi_box = {
        static_cast<double>(roi_boxes[index * 4]),
        static_cast<double>(roi_boxes[index * 4 + 1]),
        static_cast<double>(roi_boxes[index * 4 + 2]),
        static_cast<double>(roi_boxes[index * 4 + 3])};
    if (!std::all_of(roi_box.begin(), roi_box.end(),
                     [](double value) { return std::isfinite(value); }) ||
        roi_box[2] <= roi_box[0] || roi_box[3] <= roi_box[1]) {
      internal::SetArchiveError(error,
                                "Crop bbox_roi_xyxy contains invalid data");
      return nullptr;
    }
    columns.roi_bbox_xyxy[index] = roi_box;
  }
  std::vector<float>{}.swap(roi_boxes);

  if (!ReadExact(*key_store, &columns.instance_keys, error) ||
      columns.instance_keys.size() != manifest.instance_count) {
    if (error && error->empty()) {
      *error = "Crop instance_key has an inconsistent shape";
    }
    return nullptr;
  }
  auto repository = MakeAnalysisCropGeometryRepository(std::move(descriptor),
                                                       std::move(columns));
  if (!repository) {
    internal::SetArchiveError(error,
                              "Coordinate-aware crop columns are inconsistent");
  }
  return repository;
}

bool ValidRunName(const std::string &run_name) {
  return !run_name.empty() && run_name != "." && run_name != ".." &&
         run_name.find('/') == std::string::npos;
}

std::string NormalizeRunName(std::string run_name, const std::string &group) {
  const std::string prefix = group + "/";
  if (run_name.rfind(prefix, 0) == 0) {
    run_name.erase(0, prefix.size());
  }
  return ValidRunName(run_name) ? run_name : std::string{};
}

std::string LatestRun(const ArchiveContext::Impl &archive,
                      const std::string &group) {
  const auto attributes = internal::ReadArchiveAttributes(archive, group);
  if (!attributes) {
    return {};
  }
  constexpr std::array<const char *, 6> latest_keys = {
      "latest",          "latest_completed",
      "latest_complete", "latest_success",
      "latest_any",      "refined_subject_mask_review_status_latest"};
  for (const char *key : latest_keys) {
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

std::string CropRunFromLatestLineage(const ArchiveContext::Impl &archive,
                                     const std::string &group) {
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

std::string DiscoverCropRun(const ArchiveContext::Impl &archive) {
  std::string run_name = LatestRun(archive, "crop_runs");
  if (!run_name.empty()) {
    return run_name;
  }

  // Subject masks are the coordinate lineage shared by masks, subject shape,
  // and eye geometry, so prefer them when independently selected products
  // reference a different crop run.
  constexpr std::array<const char *, 3> lineage_groups = {
      "refined_subject_masks_runs", "refined_keypoints_runs", "keypoints_runs"};
  for (const char *group : lineage_groups) {
    run_name = CropRunFromLatestLineage(archive, group);
    if (!run_name.empty()) {
      return run_name;
    }
  }
  return {};
}

template <typename Source>
bool ReadIntegerArray(const ArchiveContext::Impl &archive,
                      const std::string &path, std::vector<int64_t> *output) {
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
  const Source *values = static_cast<const Source *>(read_result->data());
  output->resize(count);
  for (size_t index = 0; index < count; ++index) {
    (*output)[index] = static_cast<int64_t>(values[index]);
  }
  return true;
}

bool ReadFrameIndices(const ArchiveContext::Impl &archive,
                      const std::string &path, std::vector<int64_t> *output) {
  return ReadIntegerArray<int64_t>(archive, path, output) ||
         ReadIntegerArray<uint64_t>(archive, path, output) ||
         ReadIntegerArray<int32_t>(archive, path, output) ||
         ReadIntegerArray<uint32_t>(archive, path, output) ||
         ReadIntegerArray<int16_t>(archive, path, output) ||
         ReadIntegerArray<uint16_t>(archive, path, output) ||
         ReadIntegerArray<int8_t>(archive, path, output) ||
         ReadIntegerArray<uint8_t>(archive, path, output);
}

template <typename Source, size_t Columns>
bool ReadFixedMatrix(const ArchiveContext::Impl &archive,
                     const std::string &path,
                     std::vector<std::array<double, Columns>> *output) {
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
      read_result->shape()[1] < static_cast<ts::Index>(Columns)) {
    return false;
  }
  const size_t rows = static_cast<size_t>(read_result->shape()[0]);
  const size_t source_columns = static_cast<size_t>(read_result->shape()[1]);
  const Source *values = static_cast<const Source *>(read_result->data());
  output->resize(rows);
  for (size_t row = 0; row < rows; ++row) {
    for (size_t column = 0; column < Columns; ++column) {
      (*output)[row][column] =
          static_cast<double>(values[row * source_columns + column]);
    }
  }
  return true;
}

template <size_t Columns>
bool ReadNumericFixedMatrix(const ArchiveContext::Impl &archive,
                            const std::string &path,
                            std::vector<std::array<double, Columns>> *output) {
  return ReadFixedMatrix<double, Columns>(archive, path, output) ||
         ReadFixedMatrix<float, Columns>(archive, path, output) ||
         ReadFixedMatrix<int64_t, Columns>(archive, path, output) ||
         ReadFixedMatrix<int32_t, Columns>(archive, path, output);
}

bool ReadRoiSize(const ArchiveContext::Impl &archive,
                 const std::string &run_base, int *output_width,
                 int *output_height) {
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

} // namespace

std::unique_ptr<AnalysisCropGeometryRepository>
OpenAnalysisCropGeometryRepository(
    const std::shared_ptr<ArchiveContext> &archive,
    const std::string &requested_run, std::string *error_message) {
  if (!archive || !archive->impl_) {
    internal::SetArchiveError(error_message, "Archive context is not open");
    return nullptr;
  }
  const auto &impl = *archive->impl_;

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
  const auto direct_run =
      internal::ReadArchiveJson(impl, run_base + "/zarr.json");
  if (direct_run) {
    try {
      const auto &attributes = direct_run->at("attributes");
      if (attributes.contains("run_manifest")) {
        const auto root = internal::ReadArchiveJson(impl, "zarr.json");
        if (!root) {
          internal::SetArchiveError(
              error_message,
              "Coordinate-aware crop run requires consolidated root metadata");
          return nullptr;
        }
        return OpenCoordinateAwareCrop(impl, *root, *direct_run, run_name,
                                       run_base, error_message);
      }
    } catch (const json::exception &exception) {
      internal::SetArchiveError(error_message,
                                "Invalid crop run metadata: " +
                                    std::string(exception.what()));
      return nullptr;
    }
  }
  AnalysisCropGeometryColumns columns;
  if (!ReadFrameIndices(impl, run_base + "/frame_indices",
                        &columns.frame_indices) ||
      columns.frame_indices.empty()) {
    internal::SetArchiveError(error_message,
                              "Crop run has no readable frame_indices");
    return nullptr;
  }
  if (!ReadNumericFixedMatrix<2>(impl, run_base + "/roi_coordinates_full",
                                 &columns.offsets_xy) ||
      columns.offsets_xy.size() != columns.frame_indices.size()) {
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

  std::vector<std::array<double, 4>> boxes;
  const bool boxes_available =
      ReadNumericFixedMatrix<4>(impl, run_base + "/bbox_norm_coords", &boxes) &&
      boxes.size() == columns.frame_indices.size();

  for (size_t index = 0; index < columns.frame_indices.size(); ++index) {
    if (columns.frame_indices[index] < 0 ||
        !std::isfinite(columns.offsets_xy[index][0]) ||
        !std::isfinite(columns.offsets_xy[index][1])) {
      internal::SetArchiveError(
          error_message, "Crop run contains invalid frame or ROI coordinates");
      return nullptr;
    }
  }

  if (boxes_available) {
    columns.normalized_detection_cxcywh = std::move(boxes);
    size_t valid_boxes = 0;
    for (const auto &box : columns.normalized_detection_cxcywh) {
      valid_boxes +=
          std::all_of(box.begin(), box.end(),
                      [](double value) { return std::isfinite(value); }) &&
          box[2] > 0.0 && box[3] > 0.0;
    }
    if (valid_boxes == 0) {
      std::vector<std::array<double, 4>>{}.swap(
          columns.normalized_detection_cxcywh);
    } else if (valid_boxes != columns.normalized_detection_cxcywh.size()) {
      columns.normalized_detection_valid.assign(
          columns.normalized_detection_cxcywh.size(), 0);
      for (size_t index = 0; index < columns.normalized_detection_cxcywh.size();
           ++index) {
        const auto &box = columns.normalized_detection_cxcywh[index];
        columns.normalized_detection_valid[index] =
            std::all_of(box.begin(), box.end(),
                        [](double value) { return std::isfinite(value); }) &&
                    box[2] > 0.0 && box[3] > 0.0
                ? 1
                : 0;
      }
    }
  }

  auto repository = MakeAnalysisCropGeometryRepository(std::move(descriptor),
                                                       std::move(columns));
  if (!repository) {
    internal::SetArchiveError(error_message,
                              "Legacy crop columns are inconsistent");
  }
  return repository;
}

} // namespace crimson::zarr
