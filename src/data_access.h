#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace crimson::data {

struct SourceIdentity {
  std::string archive;
  std::string product;
  std::string run;

  bool valid() const;
  bool operator==(const SourceIdentity &other) const;
  bool operator!=(const SourceIdentity &other) const { return !(*this == other); }
};

struct SourceIdentityHash {
  size_t operator()(const SourceIdentity &source) const;
};

struct FrameRange {
  int64_t first = 0;
  int64_t last = -1;

  bool valid() const;
  uint64_t size() const;
  bool contains(int64_t frame) const;
  bool intersects(const FrameRange &other) const;
  bool operator==(const FrameRange &other) const;
};

struct FieldSelection {
  bool all_fields = true;
  std::vector<std::string> names;

  static FieldSelection All();
  static FieldSelection Named(std::vector<std::string> names);

  bool valid() const;
  FieldSelection normalized() const;
  bool operator==(const FieldSelection &other) const;
};

enum class RequestPriority : uint8_t {
  CurrentFrame = 0,
  Inspection = 1,
  History = 2,
  VisibleWindow = 3,
  Speculative = 4,
};

enum class AccessPattern : uint8_t {
  Forward,
  Reverse,
  Paused,
  RandomSeek,
  ReviewNavigation,
};

struct DataRangeRequest {
  SourceIdentity source;
  FrameRange frames;
  FieldSelection fields;
  RequestPriority priority = RequestPriority::VisibleWindow;
  AccessPattern access_pattern = AccessPattern::Paused;
  uint64_t generation = 0;

  bool valid() const;
  DataRangeRequest normalized() const;
};

bool equivalentDataWork(const DataRangeRequest &left,
                        const DataRangeRequest &right);

enum class DataResultStatus : uint8_t {
  Unavailable,
  Pending,
  Missing,
  ValidEmpty,
  Ready,
  Stale,
  Discarded,
  Failed,
};

struct DataRangeResult {
  DataRangeRequest request;
  DataResultStatus status = DataResultStatus::Unavailable;
  uint64_t decoded_bytes = 0;
  uint64_t retained_cpu_bytes = 0;
  uint64_t retained_gpu_bytes = 0;
  std::string error;

  bool terminal() const;
  bool publishable() const;
};

struct DataCacheBudget {
  uint64_t maximum_cpu_bytes = 0;
  uint64_t maximum_gpu_bytes = 0;
  size_t maximum_items = 0;

  bool valid() const;
};

struct DataPayloadCost {
  uint64_t cpu_bytes = 0;
  uint64_t gpu_bytes = 0;
};

struct SmallSeriesPreloadPolicy {
  uint64_t maximum_retained_bytes = 0;

  bool enabled() const { return maximum_retained_bytes > 0; }
  bool admits(uint64_t retained_bytes) const {
    return enabled() && retained_bytes <= maximum_retained_bytes;
  }
};

const char *dataResultStatusName(DataResultStatus status);
const char *requestPriorityName(RequestPriority priority);
const char *accessPatternName(AccessPattern pattern);

} // namespace crimson::data
