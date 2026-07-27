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

struct AffiliatedVideoDescriptor {
  std::filesystem::path stored_path;
  std::filesystem::path resolved_path;
  AffiliatedVideoSource source = AffiliatedVideoSource::RootAttributes;
  std::string metadata_path;
  std::string metadata_key;
};

const char* AffiliatedVideoSourceName(AffiliatedVideoSource source);

// Returns an empty optional with an empty error when the archive has no
// affiliated-video metadata. Invalid or stale metadata returns a diagnostic.
std::optional<AffiliatedVideoDescriptor> DiscoverAffiliatedVideo(
    const std::shared_ptr<ArchiveContext>& archive,
    std::string* error_message = nullptr);

}  // namespace crimson::zarr
