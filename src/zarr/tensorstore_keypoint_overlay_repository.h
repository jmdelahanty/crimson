#pragma once

#include "zarr/keypoint_overlay_repository.h"

#include <memory>
#include <string>
#include <vector>

namespace crimson::zarr {

class ArchiveContext;

struct KeypointRepositoryOpenEvent {
  std::string phase;
  std::string operation;
  std::string path;
  std::string candidate;
  double elapsed_ms = 0.0;
  bool success = false;
};

struct KeypointRepositoryOpenMetrics {
  double total_ms = 0.0;
  double selection_ms = 0.0;
  double run_attributes_ms = 0.0;
  double required_handles_ms = 0.0;
  double frame_counts_read_ms = 0.0;
  double prefix_sum_ms = 0.0;
  double crop_lineage_ms = 0.0;
  double optional_handles_ms = 0.0;
  double fallback_materialization_ms = 0.0;
  size_t attribute_reads = 0;
  size_t array_open_attempts = 0;
  size_t array_open_successes = 0;
  size_t array_open_failures = 0;
  size_t array_reads = 0;
  bool lazy_attempted = false;
  bool lazy_path = false;
  bool fallback_path = false;
  std::vector<KeypointRepositoryOpenEvent> events;
};

std::unique_ptr<KeypointOverlayRepository> OpenKeypointOverlayRepository(
    const std::shared_ptr<ArchiveContext>& archive,
    const std::string& requested_run = {},
    std::string* error_message = nullptr,
    KeypointRepositoryOpenMetrics* open_metrics = nullptr);

}  // namespace crimson::zarr
