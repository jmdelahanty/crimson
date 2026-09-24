#pragma once

#include "zarr/canonical_overlay_selection.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace crimson::zarr {

class ArchiveContext;

// An epoch-local, immutable view of the selected strict mask's frame offsets.
// The archive reference keeps the exact opener epoch alive. Consumers retain
// their own admission rules.
class SharedMaskFrameIndex {
 public:
  const std::vector<int64_t>& offsets() const { return offsets_; }
  size_t frameCount() const { return frame_count_; }
  size_t rowCount() const { return row_count_; }
  size_t maximumObservationsPerFrame() const { return maximum_per_frame_; }
  uint64_t retainedBytes() const {
    return static_cast<uint64_t>(offsets_.capacity()) * sizeof(int64_t);
  }
  const std::string& maskRun() const { return mask_run_; }
  const std::string& manifestPayloadDigest() const { return manifest_payload_digest_; }
  const std::string& offsetValuesDigest() const { return offset_values_digest_; }

  bool matches(const std::shared_ptr<ArchiveContext>& archive,
               const CanonicalOverlaySelection& selection) const;
  bool matchesMask(const std::shared_ptr<ArchiveContext>& archive,
                   const std::string& run, const std::string& manifest_digest,
                   const std::string& offset_values_digest,
                   size_t frame_count, size_t row_count) const;
  bool admits(size_t max_observations_per_frame) const {
    return max_observations_per_frame != 0 &&
           maximum_per_frame_ <= max_observations_per_frame;
  }

 private:
  friend struct SharedMaskFrameIndexTestAccess;
  friend std::shared_ptr<const SharedMaskFrameIndex> OpenSharedMaskFrameIndex(
      const std::shared_ptr<ArchiveContext>&,
      const CanonicalOverlaySelection&, std::string*);
  SharedMaskFrameIndex(std::shared_ptr<ArchiveContext> archive,
                       const CanonicalOverlaySelection& selection,
                       std::vector<int64_t> offsets,
                       size_t maximum_per_frame);

  std::shared_ptr<ArchiveContext> archive_;
  std::string archive_identity_;
  std::string mask_run_;
  std::string manifest_payload_digest_;
  std::string offset_values_digest_;
  size_t frame_count_ = 0;
  size_t row_count_ = 0;
  size_t maximum_per_frame_ = 0;
  std::vector<int64_t> offsets_;
};

std::shared_ptr<const SharedMaskFrameIndex> OpenSharedMaskFrameIndex(
    const std::shared_ptr<ArchiveContext>& archive,
    const CanonicalOverlaySelection& selection,
    std::string* error_message = nullptr);

} // namespace crimson::zarr
