#include "data_access.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace crimson::data {
namespace {

void hashCombine(size_t *seed, size_t value) {
  *seed ^= value + static_cast<size_t>(0x9e3779b9U) + (*seed << 6U) +
           (*seed >> 2U);
}

} // namespace

bool SourceIdentity::valid() const {
  return !archive.empty() && !product.empty() && !run.empty();
}

bool SourceIdentity::operator==(const SourceIdentity &other) const {
  return archive == other.archive && product == other.product &&
         run == other.run;
}

size_t SourceIdentityHash::operator()(const SourceIdentity &source) const {
  size_t result = std::hash<std::string>{}(source.archive);
  hashCombine(&result, std::hash<std::string>{}(source.product));
  hashCombine(&result, std::hash<std::string>{}(source.run));
  return result;
}

bool FrameRange::valid() const { return first >= 0 && last >= first; }

uint64_t FrameRange::size() const {
  if (!valid()) {
    return 0;
  }
  const uint64_t first_value = static_cast<uint64_t>(first);
  const uint64_t last_value = static_cast<uint64_t>(last);
  if (last_value - first_value == std::numeric_limits<uint64_t>::max()) {
    return std::numeric_limits<uint64_t>::max();
  }
  return last_value - first_value + 1;
}

bool FrameRange::contains(int64_t frame) const {
  return valid() && frame >= first && frame <= last;
}

bool FrameRange::intersects(const FrameRange &other) const {
  return valid() && other.valid() && first <= other.last &&
         other.first <= last;
}

bool FrameRange::operator==(const FrameRange &other) const {
  return first == other.first && last == other.last;
}

FieldSelection FieldSelection::All() { return {}; }

FieldSelection FieldSelection::Named(std::vector<std::string> values) {
  FieldSelection result;
  result.all_fields = false;
  result.names = std::move(values);
  return result.normalized();
}

bool FieldSelection::valid() const {
  if (all_fields) {
    return names.empty();
  }
  return !names.empty() &&
         std::all_of(names.begin(), names.end(),
                     [](const std::string &name) { return !name.empty(); });
}

FieldSelection FieldSelection::normalized() const {
  if (all_fields) {
    return All();
  }
  FieldSelection result = *this;
  result.names.erase(
      std::remove(result.names.begin(), result.names.end(), std::string{}),
      result.names.end());
  std::sort(result.names.begin(), result.names.end());
  result.names.erase(std::unique(result.names.begin(), result.names.end()),
                     result.names.end());
  return result;
}

bool FieldSelection::operator==(const FieldSelection &other) const {
  const auto left = normalized();
  const auto right = other.normalized();
  return left.all_fields == right.all_fields && left.names == right.names;
}

bool DataRangeRequest::valid() const {
  return source.valid() && frames.valid() && fields.valid() && generation > 0;
}

DataRangeRequest DataRangeRequest::normalized() const {
  DataRangeRequest result = *this;
  result.fields = fields.normalized();
  return result;
}

bool equivalentDataWork(const DataRangeRequest &left,
                        const DataRangeRequest &right) {
  return left.source == right.source && left.frames == right.frames &&
         left.fields == right.fields && left.generation == right.generation;
}

bool DataRangeResult::terminal() const {
  return status != DataResultStatus::Pending;
}

bool DataRangeResult::publishable() const {
  return status == DataResultStatus::Unavailable ||
         status == DataResultStatus::Missing ||
         status == DataResultStatus::ValidEmpty ||
         status == DataResultStatus::Ready || status == DataResultStatus::Failed;
}

bool DataCacheBudget::valid() const { return maximum_items > 0; }

const char *dataResultStatusName(DataResultStatus status) {
  switch (status) {
  case DataResultStatus::Unavailable:
    return "unavailable";
  case DataResultStatus::Pending:
    return "pending";
  case DataResultStatus::Missing:
    return "missing";
  case DataResultStatus::ValidEmpty:
    return "valid_empty";
  case DataResultStatus::Ready:
    return "ready";
  case DataResultStatus::Stale:
    return "stale";
  case DataResultStatus::Discarded:
    return "discarded";
  case DataResultStatus::Failed:
    return "failed";
  }
  return "unknown";
}

const char *requestPriorityName(RequestPriority priority) {
  switch (priority) {
  case RequestPriority::CurrentFrame:
    return "current_frame";
  case RequestPriority::Inspection:
    return "inspection";
  case RequestPriority::History:
    return "history";
  case RequestPriority::VisibleWindow:
    return "visible_window";
  case RequestPriority::Speculative:
    return "speculative";
  }
  return "unknown";
}

const char *accessPatternName(AccessPattern pattern) {
  switch (pattern) {
  case AccessPattern::Forward:
    return "forward";
  case AccessPattern::Reverse:
    return "reverse";
  case AccessPattern::Paused:
    return "paused";
  case AccessPattern::RandomSeek:
    return "random_seek";
  case AccessPattern::ReviewNavigation:
    return "review_navigation";
  }
  return "unknown";
}

} // namespace crimson::data
