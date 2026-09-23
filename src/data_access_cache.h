#pragma once

#include "data_access.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace crimson::data {

enum class DataCacheAdmissionStatus : uint8_t {
  Inserted,
  Replaced,
  RejectedOversize,
  RejectedPressure,
};

template <typename Key> struct DataCacheAdmission {
  DataCacheAdmissionStatus status = DataCacheAdmissionStatus::RejectedOversize;
  std::vector<Key> evicted_keys;

  bool admitted() const {
    return status == DataCacheAdmissionStatus::Inserted ||
           status == DataCacheAdmissionStatus::Replaced;
  }
};

struct DataCacheMetrics {
  uint64_t admissions = 0;
  uint64_t replacements = 0;
  uint64_t rejected_oversize = 0;
  uint64_t rejected_pressure = 0;
  uint64_t evictions = 0;
  uint64_t released_cpu_bytes = 0;
  uint64_t released_gpu_bytes = 0;
  uint64_t current_cpu_bytes = 0;
  uint64_t current_gpu_bytes = 0;
  uint64_t peak_cpu_bytes = 0;
  uint64_t peak_gpu_bytes = 0;
  size_t current_items = 0;
  size_t peak_items = 0;
};

template <typename Key, typename Value, typename Hash = std::hash<Key>,
          typename Equal = std::equal_to<Key>>
class ByteBudgetLruCache {
public:
  explicit ByteBudgetLruCache(DataCacheBudget budget) : budget_(budget) {}

  const DataCacheBudget &budget() const { return budget_; }
  const DataCacheMetrics &metrics() const { return metrics_; }
  size_t size() const { return entries_.size(); }
  bool empty() const { return entries_.empty(); }

  bool contains(const Key &key) const {
    return entries_.find(key) != entries_.end();
  }

  const Value *peek(const Key &key) const {
    const auto found = entries_.find(key);
    return found == entries_.end() ? nullptr : &found->second.value;
  }

  Value *findAndTouch(const Key &key) {
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
      return nullptr;
    }
    found->second.last_access = next_access_++;
    return &found->second.value;
  }

  bool markActive(const Key &key, bool active) {
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
      return false;
    }
    found->second.active = active;
    found->second.last_access = next_access_++;
    return true;
  }

  DataCacheAdmission<Key> put(Key key, Value value, DataPayloadCost cost,
                              RequestPriority priority,
                              bool active = false) {
    DataCacheAdmission<Key> result;
    if (!budget_.valid() || cost.cpu_bytes > budget_.maximum_cpu_bytes ||
        cost.gpu_bytes > budget_.maximum_gpu_bytes) {
      ++metrics_.rejected_oversize;
      result.status = DataCacheAdmissionStatus::RejectedOversize;
      return result;
    }

    const auto existing = entries_.find(key);
    const bool replacing = existing != entries_.end();
    const DataPayloadCost replaced_cost =
        replacing ? existing->second.cost : DataPayloadCost{};
    const size_t base_items = entries_.size() - (replacing ? 1U : 0U);
    uint64_t base_cpu = metrics_.current_cpu_bytes - replaced_cost.cpu_bytes;
    uint64_t base_gpu = metrics_.current_gpu_bytes - replaced_cost.gpu_bytes;

    struct Candidate {
      const Key *key = nullptr;
      RequestPriority priority = RequestPriority::Speculative;
      bool active = false;
      uint64_t last_access = 0;
      bool incoming = false;
      DataPayloadCost cost;
    };

    std::vector<Candidate> candidates;
    candidates.reserve(entries_.size() + 1);
    for (const auto &entry : entries_) {
      if (replacing && equal_(entry.first, key)) {
        continue;
      }
      candidates.push_back({&entry.first, entry.second.priority,
                            entry.second.active, entry.second.last_access,
                            false, entry.second.cost});
    }
    const uint64_t incoming_access = next_access_;
    candidates.push_back(
        {&key, priority, active, incoming_access, true, cost});
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &left, const Candidate &right) {
                if (left.active != right.active) {
                  return !left.active;
                }
                if (left.priority != right.priority) {
                  return static_cast<uint8_t>(left.priority) >
                         static_cast<uint8_t>(right.priority);
                }
                return left.last_access < right.last_access;
              });

    size_t virtual_items = base_items;
    auto exceeds = [&]() {
      const bool cpu_exceeded =
          cost.cpu_bytes > budget_.maximum_cpu_bytes - base_cpu;
      const bool gpu_exceeded =
          cost.gpu_bytes > budget_.maximum_gpu_bytes - base_gpu;
      return cpu_exceeded || gpu_exceeded ||
             virtual_items + 1 > budget_.maximum_items;
    };

    std::vector<Key> planned_evictions;
    for (const auto &candidate : candidates) {
      if (!exceeds()) {
        break;
      }
      if (candidate.incoming) {
        ++metrics_.rejected_pressure;
        result.status = DataCacheAdmissionStatus::RejectedPressure;
        return result;
      }
      base_cpu -= candidate.cost.cpu_bytes;
      base_gpu -= candidate.cost.gpu_bytes;
      --virtual_items;
      planned_evictions.push_back(*candidate.key);
    }
    if (exceeds()) {
      ++metrics_.rejected_pressure;
      result.status = DataCacheAdmissionStatus::RejectedPressure;
      return result;
    }

    if (replacing) {
      release(existing->second.cost, false);
      entries_.erase(existing);
      ++metrics_.replacements;
      result.status = DataCacheAdmissionStatus::Replaced;
    } else {
      ++metrics_.admissions;
      result.status = DataCacheAdmissionStatus::Inserted;
    }
    for (const auto &evicted : planned_evictions) {
      const auto found = entries_.find(evicted);
      if (found == entries_.end()) {
        continue;
      }
      release(found->second.cost, true);
      entries_.erase(found);
      result.evicted_keys.push_back(evicted);
    }

    Entry entry{std::move(value), cost, priority, active, next_access_++};
    metrics_.current_cpu_bytes += cost.cpu_bytes;
    metrics_.current_gpu_bytes += cost.gpu_bytes;
    entries_.emplace(std::move(key), std::move(entry));
    refreshPeaks();
    return result;
  }

  bool erase(const Key &key) {
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
      return false;
    }
    release(found->second.cost, false);
    entries_.erase(found);
    metrics_.current_items = entries_.size();
    return true;
  }

  void clear() {
    metrics_.released_cpu_bytes += metrics_.current_cpu_bytes;
    metrics_.released_gpu_bytes += metrics_.current_gpu_bytes;
    metrics_.current_cpu_bytes = 0;
    metrics_.current_gpu_bytes = 0;
    metrics_.current_items = 0;
    entries_.clear();
  }

private:
  struct Entry {
    Value value;
    DataPayloadCost cost;
    RequestPriority priority;
    bool active = false;
    uint64_t last_access = 0;
  };

  void release(const DataPayloadCost &cost, bool eviction) {
    metrics_.current_cpu_bytes -=
        std::min(metrics_.current_cpu_bytes, cost.cpu_bytes);
    metrics_.current_gpu_bytes -=
        std::min(metrics_.current_gpu_bytes, cost.gpu_bytes);
    metrics_.released_cpu_bytes += cost.cpu_bytes;
    metrics_.released_gpu_bytes += cost.gpu_bytes;
    if (eviction) {
      ++metrics_.evictions;
    }
  }

  void refreshPeaks() {
    metrics_.current_items = entries_.size();
    metrics_.peak_items = std::max(metrics_.peak_items, entries_.size());
    metrics_.peak_cpu_bytes =
        std::max(metrics_.peak_cpu_bytes, metrics_.current_cpu_bytes);
    metrics_.peak_gpu_bytes =
        std::max(metrics_.peak_gpu_bytes, metrics_.current_gpu_bytes);
  }

  DataCacheBudget budget_;
  uint64_t next_access_ = 1;
  std::unordered_map<Key, Entry, Hash, Equal> entries_;
  Equal equal_;
  DataCacheMetrics metrics_;
};

const char *dataCacheAdmissionStatusName(DataCacheAdmissionStatus status);

} // namespace crimson::data
