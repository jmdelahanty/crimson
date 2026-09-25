#pragma once

#include <cstdint>
#include <filesystem>

namespace crimson::media {

enum class RecordingPathResolutionKind : uint8_t {
  Empty,
  RecordingRelative,
  StoredAbsolute,
  RelocatedAbsolute,
  UnresolvedAbsolute,
};

struct RecordingPathResolution {
  std::filesystem::path stored_path;
  std::filesystem::path recording_root;
  std::filesystem::path resolved_path;
  RecordingPathResolutionKind kind = RecordingPathResolutionKind::Empty;
};

const char *RecordingPathResolutionKindName(RecordingPathResolutionKind kind);

std::filesystem::path
InferRecordingRootFromArchive(const std::filesystem::path &archive_root);

RecordingPathResolution
ResolveStoredRecordingPath(const std::filesystem::path &recording_root,
                           const std::filesystem::path &stored_path);

} // namespace crimson::media
