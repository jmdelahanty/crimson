#include <algorithm>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string_view>
#include <utility>
#include <vector>

#include "recording_clip_index.h"
#include "recording_path_resolution.h"
#include "zarr/affiliated_video_repository.h"
#include "zarr/archive_context.h"
#include "zarr/archive_context_internal.h"

namespace crimson::zarr {
using json = nlohmann::json;

namespace {

constexpr std::string_view kInventoryPath =
    "analysis/acquisition_video_streams";
constexpr std::string_view kFullStreamPath =
    "analysis/acquisition_video_streams/streams/full";

bool ContainsParentTraversal(const std::filesystem::path &path) {
  return std::any_of(path.begin(), path.end(),
                     [](const auto &component) { return component == ".."; });
}

std::optional<std::string> StringValue(const json &object, const char *key) {
  if (!object.is_object()) {
    return std::nullopt;
  }
  const auto found = object.find(key);
  if (found == object.end() || !found->is_string() ||
      found->get_ref<const std::string &>().empty()) {
    return std::nullopt;
  }
  return found->get<std::string>();
}

bool StringEquals(const json &object, const char *key, const char *expected) {
  const auto value = StringValue(object, key);
  return value && *value == expected;
}

bool IntegerEquals(const json &object, const char *key, int64_t expected) {
  if (!object.is_object()) {
    return false;
  }
  const auto found = object.find(key);
  return found != object.end() && found->is_number_integer() &&
         found->get<int64_t>() == expected;
}

std::optional<AffiliatedVideoDescriptor> Fail(std::string *error_message,
                                              std::string message) {
  internal::SetArchiveError(error_message, std::move(message));
  return std::nullopt;
}

struct ResolutionCandidate {
  std::filesystem::path path;
  AffiliatedVideoResolution resolution;
};

void AddCandidate(const std::filesystem::path &candidate,
                  AffiliatedVideoResolution resolution,
                  std::vector<ResolutionCandidate> *candidates) {
  if (candidate.empty()) {
    return;
  }
  const auto normalized = candidate.lexically_normal();
  const auto existing =
      std::find_if(candidates->begin(), candidates->end(),
                   [&](const auto &entry) { return entry.path == normalized; });
  if (existing == candidates->end()) {
    candidates->push_back({normalized, resolution});
  }
}

AffiliatedVideoResolution ClassifyRecordingResolution(
    crimson::media::RecordingPathResolutionKind resolution) {
  switch (resolution) {
  case crimson::media::RecordingPathResolutionKind::RecordingRelative:
    return AffiliatedVideoResolution::RecordingRelative;
  case crimson::media::RecordingPathResolutionKind::RelocatedAbsolute:
    return AffiliatedVideoResolution::RelocatedAbsolute;
  case crimson::media::RecordingPathResolutionKind::StoredAbsolute:
  case crimson::media::RecordingPathResolutionKind::UnresolvedAbsolute:
  case crimson::media::RecordingPathResolutionKind::Empty:
    return AffiliatedVideoResolution::StoredAbsolute;
  }
  return AffiliatedVideoResolution::StoredAbsolute;
}

bool ResolveExistingFile(const ArchiveContext &archive,
                         AffiliatedVideoDescriptor *descriptor,
                         bool allow_legacy_candidates,
                         std::string *error_message) {
  if (descriptor->stored_path.empty()) {
    internal::SetArchiveError(error_message,
                              "Affiliated video metadata has an empty path");
    return false;
  }
  if (ContainsParentTraversal(descriptor->stored_path)) {
    internal::SetArchiveError(
        error_message, "Affiliated video path escapes the recording root: " +
                           descriptor->stored_path.string());
    return false;
  }

  descriptor->recording_root = archive.recordingRootPath();
  std::vector<ResolutionCandidate> candidates;
  const auto recording_resolution = crimson::media::ResolveStoredRecordingPath(
      archive.recordingRootPath(), descriptor->stored_path);
  AddCandidate(recording_resolution.resolved_path,
               ClassifyRecordingResolution(recording_resolution.kind),
               &candidates);
  if (descriptor->stored_path.is_absolute()) {
    AddCandidate(descriptor->stored_path,
                 AffiliatedVideoResolution::StoredAbsolute, &candidates);
  } else if (allow_legacy_candidates) {
    AddCandidate(archive.rootPath() / descriptor->stored_path,
                 AffiliatedVideoResolution::LegacyArchiveRelative, &candidates);
    AddCandidate(archive.rootPath().parent_path() / descriptor->stored_path,
                 AffiliatedVideoResolution::LegacyArchiveParentRelative,
                 &candidates);
    AddCandidate(archive.recordingRootPath() / "cams" /
                     descriptor->stored_path.filename(),
                 AffiliatedVideoResolution::LegacyCameraBasename, &candidates);
  }

  std::vector<ResolutionCandidate> matches;
  for (const auto &candidate : candidates) {
    std::error_code status_error;
    if (std::filesystem::is_regular_file(candidate.path, status_error)) {
      matches.push_back(candidate);
    }
  }
  if (matches.empty()) {
    internal::SetArchiveError(
        error_message,
        "Affiliated video file does not exist for stored path: " +
            descriptor->stored_path.string());
    return false;
  }
  if (matches.size() > 1) {
    internal::SetArchiveError(
        error_message, "Affiliated video path is ambiguous on this host: " +
                           descriptor->stored_path.string());
    return false;
  }
  descriptor->resolved_path = std::move(matches.front().path);
  descriptor->resolution = matches.front().resolution;
  return true;
}

std::optional<std::string> FirstAlias(const json &attributes,
                                      std::string *metadata_key) {
  constexpr const char *kKeys[] = {"source_path", "source_video_path",
                                   "source_video", "path"};
  for (const char *key : kKeys) {
    if (auto value = StringValue(attributes, key)) {
      if (metadata_key) {
        *metadata_key = key;
      }
      return value;
    }
  }
  return std::nullopt;
}

std::optional<AffiliatedVideoDescriptor>
ResolveLegacyMetadata(const ArchiveContext &archive, const json *root,
                      const json *raw_video, std::string *error_message) {
  AffiliatedVideoDescriptor descriptor;

  if (raw_video) {
    std::string key;
    std::optional<std::string> hint;
    if (auto value = StringValue(*raw_video, "source_path")) {
      key = "source_path";
      hint = std::move(value);
    } else if (auto value = StringValue(*raw_video, "source_video")) {
      key = "source_video";
      hint = std::move(value);
    }
    if (hint) {
      descriptor.stored_path = *hint;
      descriptor.source = AffiliatedVideoSource::RawVideoAttributes;
      descriptor.metadata_path = "raw_video";
      descriptor.metadata_key = std::move(key);
      if (!ResolveExistingFile(archive, &descriptor, true, error_message)) {
        return std::nullopt;
      }
      return descriptor;
    }
  }

  if (!root) {
    return std::nullopt;
  }
  const json *source_metadata = nullptr;
  const auto metadata_found = root->find("source_video_metadata");
  if (metadata_found != root->end()) {
    if (!metadata_found->is_object()) {
      return Fail(error_message, "source_video_metadata is not an object");
    }
    source_metadata = &*metadata_found;
  }
  if (source_metadata) {
    const auto locator_found = source_metadata->find("locator");
    if (locator_found != source_metadata->end()) {
      if (!locator_found->is_object()) {
        return Fail(error_message, "source video locator is not an object");
      }
      const auto locator_kind = StringValue(*locator_found, "kind");
      if (!locator_kind) {
        return Fail(error_message, "source video locator has no kind");
      }
      if (*locator_kind != "recording_relative") {
        return Fail(error_message,
                    "Unsupported source video locator kind: " + *locator_kind);
      }
      if (auto relative_path = StringValue(*locator_found, "relative_path")) {
        if (std::filesystem::path(*relative_path).is_absolute()) {
          return Fail(error_message,
                      "recording_relative source video locator is absolute");
        }
        descriptor.stored_path = *relative_path;
        descriptor.source = AffiliatedVideoSource::SourceVideoLocator;
        descriptor.metadata_path = "source_video_metadata.locator";
        descriptor.metadata_key = "relative_path";
        if (!ResolveExistingFile(archive, &descriptor, false, error_message)) {
          return std::nullopt;
        }
        return descriptor;
      }
      return Fail(error_message,
                  "recording_relative source video locator has no path");
    }
  }

  if (auto hint = FirstAlias(*root, &descriptor.metadata_key)) {
    descriptor.stored_path = *hint;
    descriptor.source = AffiliatedVideoSource::RootAttributes;
    descriptor.metadata_path.clear();
    if (!ResolveExistingFile(archive, &descriptor, true, error_message)) {
      return std::nullopt;
    }
    return descriptor;
  }
  if (source_metadata) {
    if (auto hint = FirstAlias(*source_metadata, &descriptor.metadata_key)) {
      descriptor.stored_path = *hint;
      descriptor.source = AffiliatedVideoSource::RootAttributes;
      descriptor.metadata_path = "source_video_metadata";
      if (!ResolveExistingFile(archive, &descriptor, true, error_message)) {
        return std::nullopt;
      }
      return descriptor;
    }
  }
  return std::nullopt;
}

} // namespace

class TensorStoreAffiliatedVideoRepository {
public:
  static std::optional<AffiliatedVideoDescriptor>
  Discover(const std::shared_ptr<ArchiveContext> &archive,
           std::string *error_message) {
    if (error_message) {
      error_message->clear();
    }
    if (!archive || !archive->impl_) {
      return Fail(error_message, "Archive context is not open");
    }

    const auto inventory = internal::ReadArchiveAttributes(
        *archive->impl_, std::string(kInventoryPath));
    const auto full = internal::ReadArchiveAttributes(
        *archive->impl_, std::string(kFullStreamPath));
    bool full_declared = false;
    const json *duplicated_full = nullptr;
    if (inventory) {
      const auto streams = inventory->find("streams");
      if (streams != inventory->end() && streams->is_object()) {
        const auto declared = streams->find("full");
        if (declared != streams->end()) {
          full_declared = true;
          if (declared->is_object()) {
            duplicated_full = &*declared;
          }
        }
      }
    }

    if (full || full_declared) {
      if (!inventory || !full || !duplicated_full) {
        return Fail(error_message,
                    "Acquisition full-stream inventory is incomplete");
      }
      if (!StringEquals(*inventory, "schema_id",
                        "palette.acquisition_video_streams.v1") ||
          !IntegerEquals(*inventory, "schema_version", 1) ||
          !StringEquals(*inventory, "source_schema_id",
                        "orange_runtime_video_streams_v1") ||
          !StringEquals(*inventory, "source_frame_clock",
                        "recording_frame_id") ||
          !StringEquals(*inventory, "inventory_status", "ok")) {
        return Fail(error_message,
                    "Unsupported acquisition video stream inventory");
      }
      if (*duplicated_full != *full) {
        return Fail(error_message,
                    "Acquisition root and child full descriptors disagree");
      }

      const auto availability = StringValue(*full, "availability_status");
      const auto required_missing = full->find("required_missing");
      const bool cleanly_unavailable =
          availability && *availability == "missing" &&
          required_missing != full->end() && required_missing->is_array() &&
          !required_missing->empty();
      if (!cleanly_unavailable) {
        if (!availability || *availability != "ok" ||
            required_missing == full->end() || !required_missing->is_array() ||
            !required_missing->empty()) {
          return Fail(error_message,
                      "Acquisition full stream is not available");
        }
        const auto contract_found = full->find("contract");
        const auto files_found = full->find("files");
        if (contract_found == full->end() || !contract_found->is_object() ||
            files_found == full->end() || !files_found->is_object()) {
          return Fail(error_message,
                      "Acquisition full stream has no contract or files");
        }
        const json &contract = *contract_found;
        if (!StringEquals(*full, "stream_key", "full") ||
            !StringEquals(contract, "role",
                          "ingest_authoritative_full_frame") ||
            !StringEquals(contract, "output_kind", "full") ||
            !StringEquals(contract, "frame_clock", "recording_frame_id") ||
            !StringEquals(contract, "coordinate_space", "full_frame_pixels")) {
          return Fail(error_message,
                      "Unsupported acquisition full-stream contract");
        }
        const auto video = StringValue(contract, "video");
        const auto video_file = files_found->find("video");
        const auto file_path =
            video_file != files_found->end() && video_file->is_object()
                ? StringValue(*video_file, "path")
                : std::nullopt;
        if (!video || !file_path || *video != *file_path) {
          return Fail(error_message,
                      "Acquisition full contract and file path disagree");
        }

        AffiliatedVideoDescriptor descriptor;
        descriptor.stored_path = *video;
        descriptor.source = AffiliatedVideoSource::AcquisitionFullStream;
        descriptor.metadata_path = std::string(kFullStreamPath);
        descriptor.metadata_key = "contract.video";
        if (!ResolveExistingFile(*archive, &descriptor, false, error_message)) {
          return std::nullopt;
        }
        return descriptor;
      }
    }

    const auto root = internal::ReadArchiveAttributes(*archive->impl_, "");
    const auto raw_video =
        internal::ReadArchiveAttributes(*archive->impl_, "raw_video");
    return ResolveLegacyMetadata(*archive, root ? &*root : nullptr,
                                 raw_video ? &*raw_video : nullptr,
                                 error_message);
  }

  static std::optional<AffiliatedRecordingClipIndexDescriptor>
  DiscoverClipIndex(const std::shared_ptr<ArchiveContext> &archive,
                    std::string *error_message) {
    if (error_message) {
      error_message->clear();
    }
    if (!archive || !archive->impl_) {
      return FailClipIndex(error_message, "Archive context is not open");
    }

    const auto root = internal::ReadArchiveAttributes(*archive->impl_, "");
    const auto recording_id =
        root ? StringValue(*root, "recording_id") : std::nullopt;
    const std::string inferred_recording_id =
        archive->recordingRootPath().filename().string();
    const std::optional<std::string> expected_recording_id =
        recording_id ? recording_id
                     : inferred_recording_id.empty()
                           ? std::nullopt
                           : std::optional<std::string>{inferred_recording_id};
    std::vector<std::filesystem::path> candidates;
    const auto add_candidate = [&](std::filesystem::path candidate) {
      candidate = candidate.lexically_normal();
      if (std::find(candidates.begin(), candidates.end(), candidate) ==
          candidates.end()) {
        candidates.push_back(std::move(candidate));
      }
    };
    add_candidate(archive->recordingRootPath() / "recording_clip_index.json");
    if (recording_id) {
      for (auto ancestor = archive->rootPath().parent_path(); !ancestor.empty();
           ancestor = ancestor.parent_path()) {
        if (ancestor.filename() == "recordings") {
          add_candidate(ancestor / *recording_id / "recording_clip_index.json");
        }
        if (ancestor.filename() == ".palette_benchmarks") {
          add_candidate(ancestor.parent_path() / *recording_id /
                        "recording_clip_index.json");
        }
        const auto parent = ancestor.parent_path();
        if (parent == ancestor) {
          break;
        }
      }
    }

    std::vector<AffiliatedRecordingClipIndexDescriptor> matches;
    for (const auto &candidate : candidates) {
      std::error_code status_error;
      if (!std::filesystem::is_regular_file(candidate, status_error)) {
        continue;
      }
      std::string validation_error;
      const auto index = crimson::media::RecordingClipIndex::Open(
          candidate, &validation_error);
      if (!index) {
        return FailClipIndex(
            error_message,
            validation_error.empty()
                ? "Affiliated recording clip index is invalid: " +
                      candidate.string()
                : validation_error);
      }
      if (expected_recording_id &&
          index->recordingId() != *expected_recording_id) {
        return FailClipIndex(
            error_message,
            "Affiliated recording clip index identity disagrees with archive");
      }
      matches.push_back({index->indexPath(), index->recordingRoot(),
                         index->recordingId(), index->cameraSerial(),
                         index->totalFrameCount(), index->framesPerSecond()});
    }
    if (matches.empty()) {
      return std::nullopt;
    }
    if (matches.size() != 1) {
      return FailClipIndex(error_message,
                           "Affiliated recording clip index is ambiguous");
    }
    return std::move(matches.front());
  }

private:
  static std::optional<AffiliatedRecordingClipIndexDescriptor>
  FailClipIndex(std::string *error_message, std::string message) {
    internal::SetArchiveError(error_message, std::move(message));
    return std::nullopt;
  }
};

const char *AffiliatedVideoSourceName(AffiliatedVideoSource source) {
  switch (source) {
  case AffiliatedVideoSource::AcquisitionFullStream:
    return "acquisition_full_stream";
  case AffiliatedVideoSource::SourceVideoLocator:
    return "source_video_locator";
  case AffiliatedVideoSource::RawVideoAttributes:
    return "raw_video_attributes";
  case AffiliatedVideoSource::RootAttributes:
    return "root_attributes";
  }
  return "unknown";
}

const char *
AffiliatedVideoResolutionName(AffiliatedVideoResolution resolution) {
  switch (resolution) {
  case AffiliatedVideoResolution::StoredAbsolute:
    return "stored_absolute";
  case AffiliatedVideoResolution::RecordingRelative:
    return "recording_relative";
  case AffiliatedVideoResolution::RelocatedAbsolute:
    return "relocated_absolute";
  case AffiliatedVideoResolution::LegacyArchiveRelative:
    return "legacy_archive_relative";
  case AffiliatedVideoResolution::LegacyArchiveParentRelative:
    return "legacy_archive_parent_relative";
  case AffiliatedVideoResolution::LegacyCameraBasename:
    return "legacy_camera_basename";
  }
  return "unknown";
}

std::optional<AffiliatedVideoDescriptor>
DiscoverAffiliatedVideo(const std::shared_ptr<ArchiveContext> &archive,
                        std::string *error_message) {
  return TensorStoreAffiliatedVideoRepository::Discover(archive, error_message);
}

std::optional<AffiliatedRecordingClipIndexDescriptor>
DiscoverAffiliatedRecordingClipIndex(
    const std::shared_ptr<ArchiveContext> &archive,
    std::string *error_message) {
  return TensorStoreAffiliatedVideoRepository::DiscoverClipIndex(archive,
                                                                 error_message);
}

} // namespace crimson::zarr
