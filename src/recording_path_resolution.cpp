#include "recording_path_resolution.h"

#include <system_error>

namespace crimson::media {

const char *RecordingPathResolutionKindName(RecordingPathResolutionKind kind) {
  switch (kind) {
  case RecordingPathResolutionKind::Empty:
    return "empty";
  case RecordingPathResolutionKind::RecordingRelative:
    return "recording_relative";
  case RecordingPathResolutionKind::StoredAbsolute:
    return "stored_absolute";
  case RecordingPathResolutionKind::RelocatedAbsolute:
    return "relocated_absolute";
  case RecordingPathResolutionKind::UnresolvedAbsolute:
    return "unresolved_absolute";
  }
  return "unknown";
}

std::filesystem::path
InferRecordingRootFromArchive(const std::filesystem::path &archive_root) {
  if (archive_root.empty()) {
    return {};
  }
  std::filesystem::path recording_root =
      archive_root.lexically_normal().parent_path();
  if (recording_root.filename() == "zarr") {
    recording_root = recording_root.parent_path();
  }
  return recording_root.lexically_normal();
}

RecordingPathResolution
ResolveStoredRecordingPath(const std::filesystem::path &recording_root,
                           const std::filesystem::path &stored_path) {
  RecordingPathResolution result;
  result.stored_path = stored_path;
  result.recording_root = recording_root.lexically_normal();
  if (stored_path.empty()) {
    return result;
  }

  if (stored_path.is_relative()) {
    result.resolved_path =
        result.recording_root.empty()
            ? stored_path.lexically_normal()
            : (result.recording_root / stored_path).lexically_normal();
    result.kind = RecordingPathResolutionKind::RecordingRelative;
    return result;
  }

  std::error_code exists_error;
  if (std::filesystem::exists(stored_path, exists_error)) {
    result.resolved_path = stored_path.lexically_normal();
    result.kind = RecordingPathResolutionKind::StoredAbsolute;
    return result;
  }

  const auto recording_identity = result.recording_root.filename();
  if (!result.recording_root.empty() && !recording_identity.empty()) {
    bool found_recording = false;
    std::filesystem::path suffix;
    for (const auto &component : stored_path) {
      if (!found_recording) {
        found_recording = component == recording_identity;
        continue;
      }
      suffix /= component;
    }
    if (found_recording && !suffix.empty()) {
      result.resolved_path =
          (result.recording_root / suffix).lexically_normal();
      result.kind = RecordingPathResolutionKind::RelocatedAbsolute;
      return result;
    }
  }

  result.resolved_path = stored_path.lexically_normal();
  result.kind = RecordingPathResolutionKind::UnresolvedAbsolute;
  return result;
}

} // namespace crimson::media
