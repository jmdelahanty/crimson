#include "zarr/tensorstore_stimulus_repository.h"

#include <absl/strings/cord.h>
#include <nlohmann/json.hpp>
#include <tensorstore/kvstore/operations.h>
#include <tensorstore/kvstore/spec.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace crimson::zarr {
namespace ts = tensorstore;
using json = nlohmann::json;

struct ArchiveContext::Impl {
  std::filesystem::path root_path;
  ts::Context context = ts::Context::Default();
  ts::kvstore::KvStore store;
};

namespace {

std::string NormalizeFileRoot(std::filesystem::path path) {
  std::string normalized = path.lexically_normal().string();
  if (!normalized.empty() && normalized.back() != '/') {
    normalized.push_back('/');
  }
  return normalized;
}

void SetError(std::string* error_message, std::string message) {
  if (error_message) {
    *error_message = std::move(message);
  }
}

std::optional<json> ReadJson(const ts::kvstore::KvStore& store,
                             const std::string& key) {
  auto read_result = ts::kvstore::Read(store, key).result();
  if (!read_result.ok() || !read_result->has_value()) {
    return std::nullopt;
  }
  std::string payload;
  absl::CopyCordToString(read_result->value, &payload);
  try {
    return json::parse(payload);
  } catch (const json::exception&) {
    return std::nullopt;
  }
}

std::optional<json> ReadAttributes(const ts::kvstore::KvStore& store,
                                   const std::string& group_path) {
  std::string prefix = group_path;
  if (!prefix.empty() && prefix.back() != '/') {
    prefix.push_back('/');
  }
  if (auto metadata = ReadJson(store, prefix + "zarr.json")) {
    if (metadata->contains("attributes") &&
        (*metadata)["attributes"].is_object()) {
      return (*metadata)["attributes"];
    }
  }
  if (auto attributes = ReadJson(store, prefix + ".zattrs")) {
    if (attributes->is_object()) {
      return attributes;
    }
  }
  return std::nullopt;
}

template <typename Source>
bool ReadTypedArray(const ArchiveContext::Impl& archive,
                    const std::string& path,
                    std::vector<int64_t>* output) {
  auto store_spec = archive.store.spec();
  if (!store_spec.ok()) {
    return false;
  }
  auto kvstore_json = store_spec->ToJson();
  if (!kvstore_json.ok()) {
    return false;
  }
  json spec = {{"driver", "zarr3"},
               {"kvstore", *kvstore_json},
               {"path", path}};
  auto open_result =
      ts::Open<Source, 1>(spec, ts::OpenMode::open,
                          ts::ReadWriteMode::read, archive.context)
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
      const auto max_value =
          static_cast<std::make_unsigned_t<int64_t>>(
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
                      const std::string& path,
                      std::vector<int64_t>* output) {
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
    converted.push_back(static_cast<int32_t>(std::clamp<int64_t>(
        value, std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::max())));
  }
  return converted;
}

bool ReadInt32Array(const ArchiveContext::Impl& archive,
                    const std::string& path,
                    std::vector<int32_t>* output) {
  std::vector<int64_t> values;
  if (!ReadIntegerArray(archive, path, &values)) {
    return false;
  }
  *output = ToInt32(values);
  return true;
}

bool ReadFlagArray(const ArchiveContext::Impl& archive,
                   const std::string& path,
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

ArchiveContext::ArchiveContext(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ArchiveContext::~ArchiveContext() = default;

std::shared_ptr<ArchiveContext> ArchiveContext::Open(
    const std::filesystem::path& root_path,
    std::string* error_message) {
  if (!std::filesystem::is_directory(root_path)) {
    SetError(error_message,
             "Zarr archive directory does not exist: " + root_path.string());
    return nullptr;
  }

  auto spec = ts::kvstore::Spec::FromJson(
      {{"driver", "file"}, {"path", NormalizeFileRoot(root_path)}});
  if (!spec.ok()) {
    SetError(error_message,
             "Failed to create archive kvstore spec: " +
                 spec.status().ToString());
    return nullptr;
  }

  auto impl = std::make_shared<Impl>();
  impl->root_path = root_path;
  auto store = ts::kvstore::Open(*spec, impl->context).result();
  if (!store.ok()) {
    SetError(error_message,
             "Failed to open archive kvstore: " + store.status().ToString());
    return nullptr;
  }
  impl->store = *store;
  return std::shared_ptr<ArchiveContext>(new ArchiveContext(std::move(impl)));
}

const std::filesystem::path& ArchiveContext::rootPath() const {
  return impl_->root_path;
}

std::unique_ptr<StimulusRepository> OpenStimulusRepository(
    const std::shared_ptr<ArchiveContext>& archive,
    const std::string& requested_run,
    std::string* error_message) {
  if (!archive || !archive->impl_) {
    SetError(error_message, "Archive context is not open");
    return nullptr;
  }
  const auto& impl = *archive->impl_;

  std::string run_name = requested_run;
  if (run_name.empty()) {
    auto attributes = ReadAttributes(impl.store, "analysis/stimulus_runs");
    if (attributes && attributes->contains("latest") &&
        (*attributes)["latest"].is_string()) {
      run_name = (*attributes)["latest"].get<std::string>();
    }
  }
  if (run_name.empty()) {
    SetError(error_message,
             "No stimulus run was requested and analysis/stimulus_runs has no latest run");
    return nullptr;
  }

  StimulusAlignmentData alignment;
  alignment.run_name = run_name;
  const std::string run_base =
      "analysis/stimulus_runs/" + run_name + "/";

  if (auto attributes =
          ReadAttributes(impl.store, run_base + "frame_alignment")) {
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
      impl,
      run_base + "frame_alignment/camera_to_stimulus_frame_corrected",
      &alignment.camera_to_stimulus_frame_corrected);
  const bool legacy_frames = ReadInt32Array(
      impl, run_base + "video_metadata/frame_metadata/stimulus_frame_num",
      &alignment.frame_metadata_stimulus_frames);
  const bool corrected_frames = ReadInt32Array(
      impl,
      run_base +
          "video_metadata/frame_metadata/stimulus_frame_num_corrected",
      &alignment.frame_metadata_stimulus_frames_corrected);

  ReadFlagArray(impl, run_base + "frame_alignment/camera_interpolation_mask",
                &alignment.camera_frame_original);
  ReadFlagArray(
      impl,
      run_base +
          "frame_alignment/camera_stimulus_frame_interpolated",
      &alignment.camera_stimulus_frame_interpolated);

  alignment.direct_corrected_available =
      corrected_direct &&
      !alignment.camera_to_stimulus_frame_corrected.empty();
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
    SetError(error_message,
             "Stimulus run '" + run_name +
                 "' has no usable corrected or legacy frame mapping");
    return nullptr;
  }
  return MakeStimulusRepository(std::move(alignment));
}

}  // namespace crimson::zarr
