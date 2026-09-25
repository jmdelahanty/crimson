#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace crimson::playback {

// Three seconds of presentation lead time, capped to keep preparation local
// to the next boundary even at high source or playback rates.
inline int leadFrames(double fps, double rate) {
  const double frames = std::ceil(std::max(1.0, fps) *
                                  std::max(0.1, rate) * 3.0);
  return static_cast<int>(std::clamp(frames, 1.0, 90.0));
}

// Only frames that are presently readable in the normal ring belong in
// resident_parent_frames. A gap means retiring the old producer would stall.
inline bool hasContiguousTail(
    int64_t first, int64_t last,
    const std::vector<int64_t> &resident_parent_frames) {
  if (first < 0 || last < first) return false;
  const uint64_t required = static_cast<uint64_t>(last - first) + 1;
  if (required > resident_parent_frames.size()) return false;
  const std::unordered_set<int64_t> resident(
      resident_parent_frames.begin(), resident_parent_frames.end());
  for (int64_t frame = first;; ++frame) {
    if (resident.find(frame) == resident.end()) return false;
    if (frame == last) return true;
  }
}

inline bool canAdopt(int64_t expected_parent, int64_t expected_local,
                     int64_t actual_parent, int64_t actual_local,
                     bool old_joined, bool failed) {
  return old_joined && !failed && expected_parent >= 0 &&
         expected_local >= 0 && expected_parent == actual_parent &&
         expected_local == actual_local;
}

// Reserve headroom for the live ring, GL/CUDA interop, and decoder surfaces.
inline bool allowedStageBytes(size_t bytes, size_t budget,
                              size_t free_bytes, size_t headroom) {
  return bytes > 0 && bytes <= budget && free_bytes > headroom &&
         bytes <= free_bytes - headroom;
}

} // namespace crimson::playback
