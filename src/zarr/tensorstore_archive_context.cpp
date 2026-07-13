#include "zarr/archive_context_internal.h"

#include <absl/strings/cord.h>
#include <tensorstore/kvstore/operations.h>
#include <tensorstore/kvstore/spec.h>

#include <utility>

namespace crimson::zarr {
namespace ts = tensorstore;
using json = nlohmann::json;

namespace {

std::string NormalizeFileRoot(std::filesystem::path path) {
  std::string normalized = path.lexically_normal().string();
  if (!normalized.empty() && normalized.back() != '/') {
    normalized.push_back('/');
  }
  return normalized;
}

std::filesystem::path InferRecordingRoot(
    const std::filesystem::path& archive_root) {
  std::filesystem::path recording_root = archive_root.parent_path();
  if (recording_root.filename() == "zarr") {
    recording_root = recording_root.parent_path();
  }
  return recording_root;
}

}  // namespace

namespace internal {

void SetArchiveError(std::string* error_message, std::string message) {
  if (error_message) {
    *error_message = std::move(message);
  }
}

std::optional<json> ReadArchiveJson(const ArchiveContext::Impl& archive,
                                    const std::string& key) {
  auto read_result = ts::kvstore::Read(archive.store, key).result();
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

std::optional<json> ReadArchiveAttributes(
    const ArchiveContext::Impl& archive,
    const std::string& group_path) {
  std::string prefix = group_path;
  if (!prefix.empty() && prefix.back() != '/') {
    prefix.push_back('/');
  }
  if (auto metadata = ReadArchiveJson(archive, prefix + "zarr.json")) {
    if (metadata->contains("attributes") &&
        (*metadata)["attributes"].is_object()) {
      return (*metadata)["attributes"];
    }
  }
  if (auto attributes = ReadArchiveJson(archive, prefix + ".zattrs")) {
    if (attributes->is_object()) {
      return attributes;
    }
  }
  return std::nullopt;
}

}  // namespace internal

ArchiveContext::ArchiveContext(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ArchiveContext::~ArchiveContext() = default;

std::shared_ptr<ArchiveContext> ArchiveContext::Open(
    const std::filesystem::path& root_path,
    std::string* error_message) {
  if (!std::filesystem::is_directory(root_path)) {
    internal::SetArchiveError(
        error_message,
        "Zarr archive directory does not exist: " + root_path.string());
    return nullptr;
  }

  std::error_code absolute_error;
  const auto absolute_root =
      std::filesystem::absolute(root_path, absolute_error).lexically_normal();
  if (absolute_error) {
    internal::SetArchiveError(
        error_message,
        "Could not resolve Zarr archive path: " + absolute_error.message());
    return nullptr;
  }
  auto spec = ts::kvstore::Spec::FromJson(
      {{"driver", "file"}, {"path", NormalizeFileRoot(absolute_root)}});
  if (!spec.ok()) {
    internal::SetArchiveError(
        error_message,
        "Failed to create archive kvstore spec: " +
            spec.status().ToString());
    return nullptr;
  }

  auto impl = std::make_shared<Impl>();
  impl->root_path = absolute_root;
  impl->recording_root_path = InferRecordingRoot(impl->root_path);
  auto store = ts::kvstore::Open(*spec, impl->context).result();
  if (!store.ok()) {
    internal::SetArchiveError(
        error_message,
        "Failed to open archive kvstore: " + store.status().ToString());
    return nullptr;
  }
  impl->store = *store;
  return std::shared_ptr<ArchiveContext>(new ArchiveContext(std::move(impl)));
}

const std::filesystem::path& ArchiveContext::rootPath() const {
  return impl_->root_path;
}

const std::filesystem::path& ArchiveContext::recordingRootPath() const {
  return impl_->recording_root_path;
}

std::filesystem::path ArchiveContext::resolveStoredPath(
    const std::filesystem::path& stored_path) const {
  if (stored_path.empty()) {
    return {};
  }
  if (stored_path.is_relative()) {
    return (impl_->recording_root_path / stored_path).lexically_normal();
  }
  if (std::filesystem::exists(stored_path)) {
    return stored_path;
  }
  if (impl_->recording_root_path.empty()) {
    return stored_path;
  }

  bool found_recording = false;
  std::filesystem::path suffix;
  for (const auto& component : stored_path) {
    if (!found_recording) {
      found_recording =
          component == impl_->recording_root_path.filename();
      continue;
    }
    suffix /= component;
  }
  if (!found_recording || suffix.empty()) {
    return stored_path;
  }
  return (impl_->recording_root_path / suffix).lexically_normal();
}

}  // namespace crimson::zarr
