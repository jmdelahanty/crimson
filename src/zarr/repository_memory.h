#pragma once

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <type_traits>

namespace crimson::zarr {

struct RepositoryMemoryMetrics {
  uint64_t retained_metadata_bytes = 0;
  uint64_t retained_index_bytes = 0;
  uint64_t retained_payload_bytes = 0;
  uint64_t decoded_cache_bytes = 0;
  bool complete = false;

  uint64_t reportedRetainedBytes() const {
    uint64_t total = 0;
    for (const uint64_t value : {retained_metadata_bytes, retained_index_bytes,
                                 retained_payload_bytes, decoded_cache_bytes}) {
      if (value > std::numeric_limits<uint64_t>::max() - total) {
        return std::numeric_limits<uint64_t>::max();
      }
      total += value;
    }
    return total;
  }
};

namespace memory {

template <typename Container>
uint64_t vectorAllocationBytes(const Container &container) {
  using Value = typename Container::value_type;
  if (container.capacity() >
      std::numeric_limits<uint64_t>::max() / sizeof(Value)) {
    return std::numeric_limits<uint64_t>::max();
  }
  return static_cast<uint64_t>(container.capacity()) * sizeof(Value);
}

template <typename Map> uint64_t vectorMapAllocationLowerBound(const Map &map) {
  uint64_t total = 0;
  auto add = [&](uint64_t value) {
    total = value > std::numeric_limits<uint64_t>::max() - total
                ? std::numeric_limits<uint64_t>::max()
                : total + value;
  };
  if (map.bucket_count() <=
      std::numeric_limits<uint64_t>::max() / sizeof(void *)) {
    add(static_cast<uint64_t>(map.bucket_count()) * sizeof(void *));
  } else {
    total = std::numeric_limits<uint64_t>::max();
  }
  if (map.size() <=
      std::numeric_limits<uint64_t>::max() / sizeof(typename Map::value_type)) {
    add(static_cast<uint64_t>(map.size()) * sizeof(typename Map::value_type));
  } else {
    total = std::numeric_limits<uint64_t>::max();
  }
  for (const auto &entry : map) {
    add(vectorAllocationBytes(entry.second));
  }
  return total;
}

} // namespace memory
} // namespace crimson::zarr
