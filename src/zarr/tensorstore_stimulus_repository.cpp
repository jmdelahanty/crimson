#include "zarr/tensorstore_stimulus_repository.h"

#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
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

template <typename Source>
bool ReadTypedArray(const ArchiveContext::Impl& archive,
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
  const size_t size = static_cast<size_t>(read_result->shape()[0]);
  const Source* values = static_cast<const Source*>(read_result->data());
  output->resize(size);
  for (size_t index = 0; index < size; ++index) {
    if constexpr (std::is_unsigned_v<Source>) {
      const auto max_value = static_cast<std::make_unsigned_t<int64_t>>(
          std::numeric_limits<int64_t>::max());
      (*output)[index] = values[index] > max_value
                             ? std::numeric_limits<int64_t>::max()
                             : static_cast<int64_t>(values[index]);
    } else {
      (*output)[index] = static_cast<int64_t>(values[index]);
    }
  }
  return true;
}

bool ReadIntegerArray(const ArchiveContext::Impl& archive,
                      const std::string& path, std::vector<int64_t>* output) {
  return ReadTypedArray<int64_t>(archive, path, output) ||
         ReadTypedArray<uint64_t>(archive, path, output) ||
         ReadTypedArray<int32_t>(archive, path, output) ||
         ReadTypedArray<uint32_t>(archive, path, output) ||
         ReadTypedArray<int16_t>(archive, path, output) ||
         ReadTypedArray<uint16_t>(archive, path, output) ||
         ReadTypedArray<int8_t>(archive, path, output) ||
         ReadTypedArray<uint8_t>(archive, path, output) ||
         ReadTypedArray<bool>(archive, path, output);
}

std::vector<int32_t> ToInt32(const std::vector<int64_t>& values) {
  std::vector<int32_t> converted;
  converted.reserve(values.size());
  for (int64_t value : values) {
    converted.push_back(static_cast<int32_t>(
        std::clamp<int64_t>(value, std::numeric_limits<int32_t>::min(),
                            std::numeric_limits<int32_t>::max())));
  }
  return converted;
}

bool ReadInt32Array(const ArchiveContext::Impl& archive,
                    const std::string& path, std::vector<int32_t>* output) {
  std::vector<int64_t> values;
  if (!ReadIntegerArray(archive, path, &values)) {
    return false;
  }
  *output = ToInt32(values);
  return true;
}

bool ReadFlagArray(const ArchiveContext::Impl& archive, const std::string& path,
                   std::vector<uint8_t>* output) {
  std::vector<int64_t> values;
  if (!ReadIntegerArray(archive, path, &values)) {
    return false;
  }
  output->resize(values.size());
  std::transform(values.begin(), values.end(), output->begin(),
                 [](int64_t value) { return value != 0 ? 1 : 0; });
  return true;
}

}  // namespace

std::unique_ptr<StimulusRepository> OpenStimulusRepository(
    const std::shared_ptr<ArchiveContext>& archive,
    const std::string& requested_run, std::string* error_message,
    const std::string& source_video_override) {
  if (!archive || !archive->impl_) {
    internal::SetArchiveError(error_message, "Archive context is not open");
    return nullptr;
  }
  const auto& impl = *archive->impl_;

  std::string run_name = requested_run;
  if (run_name.empty()) {
    auto attributes =
        internal::ReadArchiveAttributes(impl, "analysis/stimulus_runs");
    if (attributes && attributes->contains("latest") &&
        (*attributes)["latest"].is_string()) {
      run_name = (*attributes)["latest"].get<std::string>();
    }
  }
  if (run_name.empty()) {
    internal::SetArchiveError(error_message,
                              "No stimulus run was requested and "
                              "analysis/stimulus_runs has no latest run");
    return nullptr;
  }

  StimulusAlignmentData alignment;
  alignment.run_name = run_name;
  const std::string run_base = "analysis/stimulus_runs/" + run_name + "/";

  if (auto attributes = internal::ReadArchiveAttributes(impl, run_base)) {
    if (attributes->contains("source_stimulus_video_path") &&
        (*attributes)["source_stimulus_video_path"].is_string()) {
      alignment.source_video_path =
          (*attributes)["source_stimulus_video_path"].get<std::string>();
      alignment.resolved_source_video_path =
          archive->resolveStoredPath(alignment.source_video_path).string();
    }
  }
  if (!source_video_override.empty()) {
    alignment.resolved_source_video_path = source_video_override;
  }

  if (auto attributes =
          internal::ReadArchiveAttributes(impl, run_base + "frame_alignment")) {
    if (attributes->contains("camera_frame_offset") &&
        (*attributes)["camera_frame_offset"].is_number_integer()) {
      alignment.camera_frame_offset =
          (*attributes)["camera_frame_offset"].get<int64_t>();
    }
  }

  const bool legacy_mapping = ReadInt32Array(
      impl, run_base + "frame_alignment/camera_to_metadata_index",
      &alignment.camera_to_metadata_index);
  const bool corrected_mapping = ReadInt32Array(
      impl, run_base + "frame_alignment/camera_to_metadata_index_corrected",
      &alignment.camera_to_metadata_index_corrected);
  const bool corrected_direct = ReadInt32Array(
      impl, run_base + "frame_alignment/camera_to_stimulus_frame_corrected",
      &alignment.camera_to_stimulus_frame_corrected);
  const bool legacy_frames = ReadInt32Array(
      impl, run_base + "video_metadata/frame_metadata/stimulus_frame_num",
      &alignment.frame_metadata_stimulus_frames);
  const bool corrected_frames = ReadInt32Array(
      impl,
      run_base + "video_metadata/frame_metadata/stimulus_frame_num_corrected",
      &alignment.frame_metadata_stimulus_frames_corrected);

  ReadFlagArray(impl, run_base + "frame_alignment/camera_interpolation_mask",
                &alignment.camera_frame_original);
  ReadFlagArray(impl,
                run_base + "frame_alignment/camera_stimulus_frame_interpolated",
                &alignment.camera_stimulus_frame_interpolated);

  alignment.direct_corrected_available =
      corrected_direct && !alignment.camera_to_stimulus_frame_corrected.empty();
  alignment.corrected_metadata_available =
      corrected_mapping && corrected_frames &&
      !alignment.camera_to_metadata_index_corrected.empty() &&
      !alignment.frame_metadata_stimulus_frames_corrected.empty();
  alignment.legacy_metadata_available =
      legacy_mapping && legacy_frames &&
      !alignment.camera_to_metadata_index.empty() &&
      !alignment.frame_metadata_stimulus_frames.empty();
  alignment.alignment_available = alignment.direct_corrected_available ||
                                  alignment.corrected_metadata_available ||
                                  alignment.legacy_metadata_available;

  if (!alignment.alignment_available) {
    internal::SetArchiveError(
        error_message,
        "Stimulus run '" + run_name +
            "' has no usable corrected or legacy frame mapping"
            " (legacy_mapping=" +
            (legacy_mapping ? "true" : "false") +
            ", legacy_frames=" + (legacy_frames ? "true" : "false") +
            ", corrected_mapping=" + (corrected_mapping ? "true" : "false") +
            ", corrected_frames=" + (corrected_frames ? "true" : "false") +
            ", corrected_direct=" + (corrected_direct ? "true" : "false") +
            ")");
    return nullptr;
  }
  return MakeStimulusRepository(std::move(alignment));
}

}  // namespace crimson::zarr
