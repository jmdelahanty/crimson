#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace crimson::zarr {

class ArchiveContext;

enum class AffiliatedVideoSource : uint8_t {
  AcquisitionFullStream,
  SourceVideoLocator,
  RawVideoAttributes,
  RootAttributes,
};

enum class AffiliatedVideoResolution : uint8_t {
  StoredAbsolute,
  RecordingRelative,
  RelocatedAbsolute,
  LegacyArchiveRelative,
  LegacyArchiveParentRelative,
  LegacyCameraBasename,
};

struct AffiliatedVideoDescriptor {
  std::filesystem::path stored_path;
  std::filesystem::path recording_root;
  std::filesystem::path resolved_path;
  AffiliatedVideoSource source = AffiliatedVideoSource::RootAttributes;
  AffiliatedVideoResolution resolution =
      AffiliatedVideoResolution::RecordingRelative;
  std::string metadata_path;
  std::string metadata_key;
};

struct AffiliatedRecordingClipIndexDescriptor {
  std::filesystem::path index_path;
  std::filesystem::path recording_root;
  std::string recording_id;
  std::string camera_serial;
  int64_t frame_count = 0;
  double frames_per_second = 0.0;
};

const char *AffiliatedVideoSourceName(AffiliatedVideoSource source);
const char *AffiliatedVideoResolutionName(AffiliatedVideoResolution resolution);

// Returns an empty optional with an empty error when the archive has no
// affiliated-video metadata. Invalid or stale metadata returns a diagnostic.
std::optional<AffiliatedVideoDescriptor>
DiscoverAffiliatedVideo(const std::shared_ptr<ArchiveContext> &archive,
                        std::string *error_message = nullptr);

// Discovers Palette's recording_clip_index.json through recording identity.
// The returned path has already passed the shared strict compatibility
// adapter, including clip continuity and media-file validation.
std::optional<AffiliatedRecordingClipIndexDescriptor>
DiscoverAffiliatedRecordingClipIndex(
    const std::shared_ptr<ArchiveContext> &archive,
    std::string *error_message = nullptr);

} // namespace crimson::zarr
