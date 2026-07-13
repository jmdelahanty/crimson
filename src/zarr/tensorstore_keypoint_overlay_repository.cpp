#include "zarr/tensorstore_keypoint_overlay_repository.h"

#include "zarr/archive_context_internal.h"

#include <nlohmann/json.hpp>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace crimson::zarr {
namespace ts = tensorstore;
using json = nlohmann::json;

namespace {

std::optional<json> MakeArraySpec(const ArchiveContext::Impl& archive,
                                  const std::string& path) {
  auto store_spec = archive.store.spec();
  if (!store_spec.ok()) {
    return std::nullopt;
  }
  auto kvstore_json = store_spec->ToJson();
  if (!kvstore_json.ok()) {
    return std::nullopt;
  }
  return json{{"driver", "zarr3"},
              {"kvstore", *kvstore_json},
              {"path", path}};
}

template <typename Source>
bool ReadIntegerVector(const ArchiveContext::Impl& archive,
                       const std::string& path,
                       std::vector<int64_t>* output) {
  const auto spec = MakeArraySpec(archive, path);
  if (!spec) {
    return false;
  }
  auto store = ts::Open<Source, 1>(*spec, ts::OpenMode::open,
                                   ts::ReadWriteMode::read, archive.context)
                   .result();
  if (!store.ok()) {
    return false;
  }
  auto read = ts::Read(*store).result();
  if (!read.ok() || read->rank() != 1) {
    return false;
  }
  const size_t count = static_cast<size_t>(read->shape()[0]);
  const Source* values = static_cast<const Source*>(read->data());
  output->resize(count);
  for (size_t index = 0; index < count; ++index) {
    (*output)[index] = static_cast<int64_t>(values[index]);
  }
  return true;
}

bool ReadIntegers(const ArchiveContext::Impl& archive,
                  const std::string& path,
                  std::vector<int64_t>* output) {
  return ReadIntegerVector<int64_t>(archive, path, output) ||
         ReadIntegerVector<uint64_t>(archive, path, output) ||
         ReadIntegerVector<int32_t>(archive, path, output) ||
         ReadIntegerVector<uint32_t>(archive, path, output) ||
         ReadIntegerVector<int16_t>(archive, path, output) ||
         ReadIntegerVector<uint16_t>(archive, path, output) ||
         ReadIntegerVector<int8_t>(archive, path, output) ||
         ReadIntegerVector<uint8_t>(archive, path, output);
}

template <typename Source>
bool ReadRealVector(const ArchiveContext::Impl& archive,
                    const std::string& path,
                    std::vector<double>* output) {
  const auto spec = MakeArraySpec(archive, path);
  if (!spec) {
    return false;
  }
  auto store = ts::Open<Source, 1>(*spec, ts::OpenMode::open,
                                   ts::ReadWriteMode::read, archive.context)
                   .result();
  if (!store.ok()) {
    return false;
  }
  auto read = ts::Read(*store).result();
  if (!read.ok() || read->rank() != 1) {
    return false;
  }
  const size_t count = static_cast<size_t>(read->shape()[0]);
  const Source* values = static_cast<const Source*>(read->data());
  output->resize(count);
  for (size_t index = 0; index < count; ++index) {
    (*output)[index] = static_cast<double>(values[index]);
  }
  return true;
}

bool ReadReals(const ArchiveContext::Impl& archive,
               const std::string& path,
               std::vector<double>* output) {
  return ReadRealVector<double>(archive, path, output) ||
         ReadRealVector<float>(archive, path, output) ||
         ReadRealVector<int64_t>(archive, path, output) ||
         ReadRealVector<int32_t>(archive, path, output);
}

template <typename Source>
bool ReadBoolVector(const ArchiveContext::Impl& archive,
                    const std::string& path,
                    std::vector<uint8_t>* output) {
  const auto spec = MakeArraySpec(archive, path);
  if (!spec) {
    return false;
  }
  auto store = ts::Open<Source, 1>(*spec, ts::OpenMode::open,
                                   ts::ReadWriteMode::read, archive.context)
                   .result();
  if (!store.ok()) {
    return false;
  }
  auto read = ts::Read(*store).result();
  if (!read.ok() || read->rank() != 1) {
    return false;
  }
  const size_t count = static_cast<size_t>(read->shape()[0]);
  const Source* values = static_cast<const Source*>(read->data());
  output->resize(count);
  for (size_t index = 0; index < count; ++index) {
    (*output)[index] = values[index] ? 1 : 0;
  }
  return true;
}

bool ReadBools(const ArchiveContext::Impl& archive,
               const std::string& path,
               std::vector<uint8_t>* output) {
  return ReadBoolVector<bool>(archive, path, output) ||
         ReadBoolVector<uint8_t>(archive, path, output) ||
         ReadBoolVector<int8_t>(archive, path, output) ||
         ReadBoolVector<int32_t>(archive, path, output);
}

template <typename Source>
bool ReadMatrix(const ArchiveContext::Impl& archive,
                const std::string& path,
                size_t minimum_columns,
                std::vector<std::vector<double>>* output) {
  const auto spec = MakeArraySpec(archive, path);
  if (!spec) {
    return false;
  }
  auto store = ts::Open<Source, 2>(*spec, ts::OpenMode::open,
                                   ts::ReadWriteMode::read, archive.context)
                   .result();
  if (!store.ok()) {
    return false;
  }
  auto read = ts::Read(*store).result();
  if (!read.ok() || read->rank() != 2 ||
      read->shape()[1] < static_cast<ts::Index>(minimum_columns)) {
    return false;
  }
  const size_t rows = static_cast<size_t>(read->shape()[0]);
  const size_t columns = static_cast<size_t>(read->shape()[1]);
  const Source* values = static_cast<const Source*>(read->data());
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
                       const std::string& path,
                       size_t minimum_columns,
                       std::vector<std::vector<double>>* output) {
  return ReadMatrix<double>(archive, path, minimum_columns, output) ||
         ReadMatrix<float>(archive, path, minimum_columns, output) ||
         ReadMatrix<int64_t>(archive, path, minimum_columns, output) ||
         ReadMatrix<int32_t>(archive, path, minimum_columns, output);
}

template <typename Source>
bool ReadKeypoints(const ArchiveContext::Impl& archive,
                   const std::string& path,
                   size_t expected_rows,
                   std::vector<std::vector<KeypointOverlayPoint>>* output) {
  const auto spec = MakeArraySpec(archive, path);
  if (!spec) {
    return false;
  }
  auto store = ts::Open<Source, 3>(*spec, ts::OpenMode::open,
                                   ts::ReadWriteMode::read, archive.context)
                   .result();
  if (!store.ok()) {
    return false;
  }
  auto read = ts::Read(*store).result();
  if (!read.ok() || read->rank() != 3 ||
      read->shape()[0] != static_cast<ts::Index>(expected_rows) ||
      read->shape()[1] <= 0 || read->shape()[2] < 2) {
    return false;
  }
  const size_t keypoint_count = static_cast<size_t>(read->shape()[1]);
  const size_t coordinate_count = static_cast<size_t>(read->shape()[2]);
  const Source* values = static_cast<const Source*>(read->data());
  output->assign(expected_rows,
                 std::vector<KeypointOverlayPoint>(keypoint_count));
  for (size_t row = 0; row < expected_rows; ++row) {
    for (size_t keypoint = 0; keypoint < keypoint_count; ++keypoint) {
      const size_t offset =
          (row * keypoint_count + keypoint) * coordinate_count;
      (*output)[row][keypoint] = {
          static_cast<double>(values[offset]),
          static_cast<double>(values[offset + 1])};
    }
  }
  return true;
}

bool ReadNumericKeypoints(
    const ArchiveContext::Impl& archive,
    const std::string& path,
    size_t expected_rows,
    std::vector<std::vector<KeypointOverlayPoint>>* output) {
  return ReadKeypoints<double>(archive, path, expected_rows, output) ||
         ReadKeypoints<float>(archive, path, expected_rows, output);
}

std::string LatestRun(const ArchiveContext::Impl& archive,
                      const std::string& group) {
  const auto attributes = internal::ReadArchiveAttributes(archive, group);
  if (!attributes) {
    return {};
  }
  constexpr std::array<const char*, 4> keys = {
      "latest", "latest_completed", "latest_complete", "latest_success"};
  for (const char* key : keys) {
    const auto found = attributes->find(key);
    if (found != attributes->end() && found->is_string() &&
        !found->get_ref<const std::string&>().empty()) {
      return found->get<std::string>();
    }
  }
  return {};
}

bool ValidRunName(const std::string& run_name) {
  return !run_name.empty() && run_name != "." && run_name != ".." &&
         run_name.find('/') == std::string::npos;
}

struct SelectedRun {
  std::string group;
  std::string name;
  bool refined = false;
};

std::optional<SelectedRun> SelectRun(const ArchiveContext::Impl& archive,
                                     std::string requested_run) {
  SelectedRun selected;
  if (requested_run.rfind("refined_keypoints_runs/", 0) == 0) {
    selected.group = "refined_keypoints_runs";
    selected.refined = true;
    requested_run.erase(0, selected.group.size() + 1);
  } else if (requested_run.rfind("keypoints_runs/", 0) == 0) {
    selected.group = "keypoints_runs";
    requested_run.erase(0, selected.group.size() + 1);
  } else if (!requested_run.empty()) {
    selected.name = requested_run;
    selected.group = "refined_keypoints_runs";
    selected.refined = true;
    if (!internal::ReadArchiveAttributes(
            archive, selected.group + "/" + selected.name)) {
      selected.group = "keypoints_runs";
      selected.refined = false;
    }
  } else {
    selected.group = "refined_keypoints_runs";
    selected.refined = true;
    selected.name = LatestRun(archive, selected.group);
    if (selected.name.empty()) {
      selected.group = "keypoints_runs";
      selected.refined = false;
      selected.name = LatestRun(archive, selected.group);
    }
  }
  if (selected.name.empty()) {
    selected.name = requested_run;
  }
  if (!ValidRunName(selected.name) ||
      !internal::ReadArchiveAttributes(
          archive, selected.group + "/" + selected.name)) {
    return std::nullopt;
  }
  return selected;
}

std::string ResolveCropRun(const ArchiveContext::Impl& archive,
                           const json& run_attributes) {
  const auto source = run_attributes.find("source_crop_run");
  if (source != run_attributes.end() && source->is_string()) {
    std::string run_name = source->get<std::string>();
    if (run_name.rfind("crop_runs/", 0) == 0) {
      run_name.erase(0, std::string("crop_runs/").size());
    }
    if (ValidRunName(run_name)) {
      return run_name;
    }
  }
  return LatestRun(archive, "crop_runs");
}

void LoadLabelsAndEdges(const json& attributes,
                        size_t keypoint_count,
                        KeypointOverlayDescriptor* descriptor) {
  const auto labels = attributes.find("keypoint_labels");
  if (labels != attributes.end() && labels->is_array()) {
    for (const auto& label : *labels) {
      if (label.is_string()) {
        descriptor->keypoint_labels.push_back(label.get<std::string>());
      }
    }
  }
  descriptor->keypoint_labels.resize(keypoint_count);
  static constexpr std::array<const char*, 3> defaults = {
      "swim_bladder", "left_eye", "right_eye"};
  for (size_t index = 0; index < keypoint_count; ++index) {
    if (!descriptor->keypoint_labels[index].empty()) {
      continue;
    }
    descriptor->keypoint_labels[index] =
        index < defaults.size() ? defaults[index]
                                : "kp" + std::to_string(index);
  }

  const auto pose_schema = attributes.find("pose_schema");
  if (pose_schema == attributes.end() || !pose_schema->is_object()) {
    return;
  }
  const auto edges = pose_schema->find("edges");
  if (edges == pose_schema->end() || !edges->is_array()) {
    return;
  }
  for (const auto& edge : *edges) {
    if (!edge.is_array() || edge.size() != 2 ||
        !edge[0].is_number_integer() || !edge[1].is_number_integer()) {
      continue;
    }
    const int64_t first = edge[0].get<int64_t>();
    const int64_t second = edge[1].get<int64_t>();
    if (first >= 0 && second >= 0 &&
        static_cast<size_t>(first) < keypoint_count &&
        static_cast<size_t>(second) < keypoint_count) {
      descriptor->skeleton_edges.push_back(
          {static_cast<size_t>(first), static_cast<size_t>(second)});
    }
  }
}

bool ReadRoiSize(const json& crop_attributes,
                 double* width,
                 double* height) {
  const auto size = crop_attributes.find("roi_size");
  if (size == crop_attributes.end() || !size->is_array() ||
      size->size() < 2 || !(*size)[0].is_number() ||
      !(*size)[1].is_number()) {
    return false;
  }
  *height = (*size)[0].get<double>();
  *width = (*size)[1].get<double>();
  return std::isfinite(*width) && std::isfinite(*height) && *width > 0.0 &&
         *height > 0.0;
}

}  // namespace

std::unique_ptr<KeypointOverlayRepository> OpenKeypointOverlayRepository(
    const std::shared_ptr<ArchiveContext>& archive,
    const std::string& requested_run,
    std::string* error_message) {
  if (!archive || !archive->impl_) {
    internal::SetArchiveError(error_message, "Archive context is not open");
    return nullptr;
  }
  const auto& impl = *archive->impl_;
  const auto selected = SelectRun(impl, requested_run);
  if (!selected) {
    internal::SetArchiveError(
        error_message,
        "No valid refined or raw keypoint run is available");
    return nullptr;
  }

  const std::string run_base = selected->group + "/" + selected->name;
  const auto run_attributes = internal::ReadArchiveAttributes(impl, run_base);
  if (!run_attributes) {
    internal::SetArchiveError(error_message,
                              "Keypoint run attributes are unreadable");
    return nullptr;
  }

  std::vector<int64_t> frame_indices;
  if (!ReadIntegers(impl, run_base + "/frame_indices", &frame_indices) ||
      frame_indices.empty()) {
    internal::SetArchiveError(error_message,
                              "Keypoint run has no readable frame_indices");
    return nullptr;
  }
  const size_t row_count = frame_indices.size();

  KeypointOverlayDescriptor descriptor;
  descriptor.source_group = selected->group;
  descriptor.run_name = selected->name;
  descriptor.refined = selected->refined;
  std::vector<std::vector<KeypointOverlayPoint>> keypoints;
  if (ReadNumericKeypoints(impl, run_base + "/keypoints_img", row_count,
                           &keypoints)) {
    descriptor.coordinate_space = KeypointCoordinateSpace::Image;
  } else if (ReadNumericKeypoints(impl, run_base + "/keypoints_roi", row_count,
                                  &keypoints)) {
    descriptor.coordinate_space = KeypointCoordinateSpace::Roi;
  } else if (ReadNumericKeypoints(impl, run_base + "/keypoints_norm",
                                  row_count, &keypoints)) {
    descriptor.coordinate_space = KeypointCoordinateSpace::NormalizedRoi;
  } else {
    internal::SetArchiveError(
        error_message,
        "Keypoint run has no readable image, ROI, or normalized keypoints");
    return nullptr;
  }
  LoadLabelsAndEdges(*run_attributes, keypoints.front().size(), &descriptor);

  std::vector<int64_t> detection_indices;
  if (!ReadIntegers(impl, run_base + "/detection_indices",
                    &detection_indices) ||
      detection_indices.size() != row_count) {
    detection_indices.assign(row_count, -1);
  }
  std::vector<int64_t> source_crop_rows;
  if (!ReadIntegers(impl, run_base + "/source_crop_row_ids",
                    &source_crop_rows) ||
      source_crop_rows.size() != row_count) {
    source_crop_rows.clear();
  }

  std::vector<double> headings;
  if (!ReadReals(impl, run_base + "/heading", &headings) ||
      headings.size() != row_count) {
    headings.assign(row_count, std::numeric_limits<double>::quiet_NaN());
  }
  std::vector<uint8_t> heading_valid;
  if (!ReadBools(impl, run_base + "/detection_success", &heading_valid) ||
      heading_valid.size() != row_count) {
    heading_valid.assign(row_count, 1);
  }
  std::vector<uint8_t> keypoint_source;
  if (!ReadBools(impl, run_base + "/detection_source", &keypoint_source) ||
      keypoint_source.size() != row_count) {
    keypoint_source.assign(row_count, 0);
  }
  std::vector<uint8_t> usable;
  if (!ReadBools(impl, run_base + "/usable_keypoints", &usable) ||
      usable.size() != row_count) {
    usable.assign(row_count, selected->refined ? 0 : 1);
  }
  std::vector<uint8_t> flip_corrected;
  if (!ReadBools(impl, run_base + "/flip_corrected", &flip_corrected) ||
      flip_corrected.size() != row_count) {
    flip_corrected.assign(row_count, 0);
  }

  descriptor.source_crop_run = ResolveCropRun(impl, *run_attributes);
  if (!ValidRunName(descriptor.source_crop_run)) {
    internal::SetArchiveError(
        error_message,
        "Keypoint run has no valid source crop run for row placement");
    return nullptr;
  }
  const std::string crop_base =
      "crop_runs/" + descriptor.source_crop_run;
  const auto crop_attributes =
      internal::ReadArchiveAttributes(impl, crop_base);
  if (!crop_attributes) {
    internal::SetArchiveError(error_message,
                              "Source crop run attributes are unreadable");
    return nullptr;
  }

  std::vector<int64_t> crop_frames;
  std::vector<int64_t> crop_detection_indices;
  std::vector<std::vector<double>> crop_offsets;
  if (!ReadIntegers(impl, crop_base + "/frame_indices", &crop_frames) ||
      crop_frames.empty() ||
      !ReadNumericMatrix(impl, crop_base + "/roi_coordinates_full", 2,
                         &crop_offsets) ||
      crop_offsets.size() != crop_frames.size()) {
    internal::SetArchiveError(
        error_message,
        "Source crop frame and ROI coordinates are not row-aligned");
    return nullptr;
  }
  if (!ReadIntegers(impl, crop_base + "/detection_indices",
                    &crop_detection_indices) ||
      crop_detection_indices.size() != crop_frames.size()) {
    crop_detection_indices.assign(crop_frames.size(), -1);
  }
  std::vector<std::vector<double>> crop_boxes;
  if (!ReadNumericMatrix(impl, crop_base + "/bbox_norm_coords", 4,
                         &crop_boxes) ||
      crop_boxes.size() != crop_frames.size()) {
    crop_boxes.clear();
  }
  std::vector<uint8_t> crop_detection_source;
  if (!ReadBools(impl, crop_base + "/detection_source",
                 &crop_detection_source) ||
      crop_detection_source.size() != crop_frames.size()) {
    crop_detection_source.assign(crop_frames.size(), 0);
  }
  double roi_width = 0.0;
  double roi_height = 0.0;
  ReadRoiSize(*crop_attributes, &roi_width, &roi_height);

  std::unordered_map<int64_t, std::vector<size_t>> crop_rows_by_frame;
  for (size_t index = 0; index < crop_frames.size(); ++index) {
    crop_rows_by_frame[crop_frames[index]].push_back(index);
  }
  std::unordered_map<int64_t, size_t> frame_cursors;
  std::vector<KeypointOverlayRow> rows;
  rows.reserve(row_count);
  for (size_t index = 0; index < row_count; ++index) {
    size_t crop_row = std::numeric_limits<size_t>::max();
    if (!source_crop_rows.empty()) {
      if (source_crop_rows[index] < 0 ||
          static_cast<uint64_t>(source_crop_rows[index]) >=
              crop_frames.size()) {
        internal::SetArchiveError(
            error_message,
            "Keypoint source_crop_row_ids contains an out-of-range row");
        return nullptr;
      }
      crop_row = static_cast<size_t>(source_crop_rows[index]);
      if (crop_frames[crop_row] != frame_indices[index]) {
        internal::SetArchiveError(
            error_message,
            "Keypoint source crop row does not match its camera frame");
        return nullptr;
      }
      if (detection_indices[index] >= 0 &&
          crop_detection_indices[crop_row] >= 0 &&
          detection_indices[index] != crop_detection_indices[crop_row]) {
        internal::SetArchiveError(
            error_message,
            "Keypoint source crop row does not match its detection index");
        return nullptr;
      }
    } else if (crop_frames.size() == row_count &&
               crop_frames[index] == frame_indices[index]) {
      crop_row = index;
    } else {
      const auto candidates = crop_rows_by_frame.find(frame_indices[index]);
      if (candidates == crop_rows_by_frame.end()) {
        internal::SetArchiveError(
            error_message,
            "Keypoint camera frame is absent from the source crop run");
        return nullptr;
      }
      auto& cursor = frame_cursors[frame_indices[index]];
      if (detection_indices[index] >= 0) {
        const auto exact = std::find_if(
            candidates->second.begin(), candidates->second.end(),
            [&](size_t candidate) {
              return crop_detection_indices[candidate] ==
                     detection_indices[index];
            });
        if (exact != candidates->second.end()) {
          crop_row = *exact;
        }
      }
      if (crop_row == std::numeric_limits<size_t>::max()) {
        if (cursor >= candidates->second.size()) {
          internal::SetArchiveError(
              error_message,
              "Legacy keypoint rows exceed source crop rows for a frame");
          return nullptr;
        }
        crop_row = candidates->second[cursor++];
      }
    }

    KeypointOverlayRow row;
    row.camera_frame = frame_indices[index];
    row.detection_index = detection_indices[index] >= 0
                              ? detection_indices[index]
                              : crop_detection_indices[crop_row];
    row.source_crop_row_id = static_cast<int64_t>(crop_row);
    row.keypoints = std::move(keypoints[index]);
    if (std::isfinite(headings[index])) {
      row.heading_degrees = headings[index];
    }
    row.heading_valid = heading_valid[index] != 0;
    row.detection_interpolated = crop_detection_source[crop_row] != 0;
    row.refined_keypoints = selected->refined;
    row.keypoint_usable = usable[index] != 0;
    row.keypoint_detection_interpolated = keypoint_source[index] != 0;
    row.keypoint_flip_corrected = flip_corrected[index] != 0;
    row.roi_offset = KeypointOverlayPoint{
        crop_offsets[crop_row][0], crop_offsets[crop_row][1]};
    row.roi_width = roi_width;
    row.roi_height = roi_height;
    if (!crop_boxes.empty()) {
      const std::array<double, 4> box = {
          crop_boxes[crop_row][0], crop_boxes[crop_row][1],
          crop_boxes[crop_row][2], crop_boxes[crop_row][3]};
      if (std::all_of(box.begin(), box.end(),
                      [](double value) { return std::isfinite(value); }) &&
          box[2] > 0.0 && box[3] > 0.0) {
        row.normalized_detection_cxcywh = box;
      }
    }
    rows.push_back(std::move(row));
  }

  return MakeKeypointOverlayRepository(std::move(descriptor),
                                       std::move(rows));
}

}  // namespace crimson::zarr
