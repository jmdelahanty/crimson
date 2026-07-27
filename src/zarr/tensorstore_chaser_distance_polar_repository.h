#pragma once

#include "chaser_distance_polar.h"
#include "zarr/archive_context.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace crimson::zarr {

struct TensorStoreChaserDistancePolarOptions {
  std::string requested_run;
  std::string requested_component;
  std::string requested_stimulus_run;
  size_t radial_scan_block_rows = 4096;
};

struct TensorStoreChaserDistancePolarMetrics {
  uint64_t frame_index_rows_read = 0;
  uint64_t chaser_index_rows_read = 0;
  uint64_t radial_scan_rows = 0;
  uint64_t exact_frame_rows_read = 0;
  uint64_t matrix_read_operations = 0;
  size_t maximum_rows_per_matrix_read = 0;
  size_t resident_frame_ids = 0;
  size_t resident_chaser_ids = 0;
};

class TensorStoreChaserDistancePolarRepository
    : public crimson::polar::ChaserDistancePolarRepository {
 public:
  ~TensorStoreChaserDistancePolarRepository() override = default;

  virtual TensorStoreChaserDistancePolarMetrics metrics() const = 0;
};

std::unique_ptr<TensorStoreChaserDistancePolarRepository>
OpenTensorStoreChaserDistancePolarRepository(
    const std::shared_ptr<ArchiveContext>& archive,
    const TensorStoreChaserDistancePolarOptions& options = {},
    std::string* error_message = nullptr);

}  // namespace crimson::zarr
