#include "canonical_detection_buffer.h"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <limits>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>

namespace {

void assignError(std::string *destination, const std::string &value) {
  if (destination) {
    *destination = value;
  }
}

uint64_t frameBytes(const crimson::zarr::CanonicalDetectionFrame &frame) {
  return sizeof(frame) +
         frame.detections.size() * sizeof(crimson::zarr::CanonicalDetection);
}

bool canonicalDetectionTraceEnabled() {
  static const bool enabled = [] {
    const char *value = std::getenv("CRIMSON_CANONICAL_DETECTION_TRACE");
    return value && value[0] != '\0' && std::string(value) != "0";
  }();
  return enabled;
}

crimson::data::FieldSelection detectionUiFields(bool stable_identity) {
  if (!stable_identity) {
    return crimson::data::FieldSelection::Named(
        {"bbox_norm_coords", "scores", "class_ids"});
  }
  return crimson::data::FieldSelection::Named(
      {"bbox_norm_coords", "scores", "class_ids", "instance_key",
       "refined_row_ids", "source_detect_row_index", "source_kind_codes",
       "score_valid", "manual_edit_flags"});
}

} // namespace

struct CanonicalDetectionBuffer::Impl {
  using Clock = std::chrono::steady_clock;

  struct ResidencyBuild {
    std::vector<crimson::zarr::CanonicalDetectionUiResidencyChunk> chunks;
    size_t next_chunk = 0;
    std::shared_ptr<crimson::zarr::CanonicalDetectionResidentUiColumns> columns;
  };

  mutable std::mutex mutex;
  mutable std::condition_variable condition;
  std::unique_ptr<crimson::zarr::CanonicalDetectionRepository> repository;
  crimson::zarr::CanonicalDetectionDescriptor descriptor;
  std::shared_ptr<crimson::data::DataAccessScheduler> scheduler;
  std::string archive_identity;
  crimson::data::SourceIdentity scheduler_source;
  crimson::data::SourceIdentity residency_source;
  bool stopping = false;
  size_t page_frames = 70;
  size_t cache_pages = 32;
  uint64_t generation = 1;
  int64_t last_request = -1;
  std::unordered_map<
      int64_t, std::shared_ptr<const crimson::zarr::CanonicalDetectionFrame>>
      frames;
  std::unordered_map<int64_t, uint64_t> page_bytes;
  std::deque<int64_t> page_order;
  CanonicalDetectionBufferMetrics metrics;
  uint64_t residency_generation = 1;
  std::shared_ptr<ResidencyBuild> residency_build;
  Clock::time_point residency_started;
  CanonicalDetectionResidencyMetrics residency_metrics;

  int64_t pageStart(int64_t frame) const {
    return frame / static_cast<int64_t>(page_frames) *
           static_cast<int64_t>(page_frames);
  }

  int64_t pageEnd(int64_t page_start) const {
    const int64_t maximum =
        descriptor.camera_frame_count >
                static_cast<size_t>(std::numeric_limits<int64_t>::max())
            ? std::numeric_limits<int64_t>::max()
            : static_cast<int64_t>(descriptor.camera_frame_count) - 1;
    return std::min(maximum,
                    page_start + static_cast<int64_t>(page_frames) - 1);
  }

  bool hasPageLocked(int64_t page_start) const {
    return page_bytes.find(page_start) != page_bytes.end();
  }

  void clearCacheLocked() {
    frames.clear();
    page_bytes.clear();
    page_order.clear();
    metrics.cached_bytes = 0;
  }

  void publishPageLocked(int64_t page_start,
                         const crimson::zarr::CanonicalDetectionPage &page) {
    if (hasPageLocked(page_start)) {
      return;
    }
    uint64_t retained = 0;
    for (const auto &frame : page.frames) {
      retained += frameBytes(frame);
      frames[frame.camera_frame] =
          std::make_shared<const crimson::zarr::CanonicalDetectionFrame>(frame);
    }
    page_bytes[page_start] = retained;
    page_order.push_back(page_start);
    metrics.cached_bytes += retained;
    while (page_order.size() > cache_pages) {
      const int64_t evicted_start = page_order.front();
      page_order.pop_front();
      const int64_t evicted_end = pageEnd(evicted_start);
      for (int64_t frame = evicted_start; frame <= evicted_end; ++frame) {
        frames.erase(frame);
      }
      const auto found = page_bytes.find(evicted_start);
      if (found != page_bytes.end()) {
        metrics.cached_bytes -= std::min(metrics.cached_bytes, found->second);
        page_bytes.erase(found);
      }
      ++metrics.evicted_pages;
    }
    metrics.peak_cached_pages =
        std::max(metrics.peak_cached_pages, page_order.size());
    metrics.peak_cached_bytes =
        std::max(metrics.peak_cached_bytes, metrics.cached_bytes);
  }

  bool submitNextResidencyChunkLocked(std::string *error = nullptr) {
    if (!repository || stopping || !residency_build ||
        residency_metrics.state != CanonicalDetectionResidencyState::Loading ||
        residency_build->next_chunk >= residency_build->chunks.size()) {
      assignError(error,
                  "Canonical detection residency builder is unavailable");
      return false;
    }
    const auto build = residency_build;
    const size_t chunk_index = build->next_chunk;
    const auto chunk = build->chunks[chunk_index];
    const uint64_t generation = residency_generation;
    crimson::data::DataRangeRequest request{
        residency_source,
        {chunk.first_camera_frame, chunk.last_camera_frame},
        detectionUiFields(descriptor.stable_identity),
        crimson::data::RequestPriority::Speculative,
        crimson::data::AccessPattern::Forward,
        generation};
    const auto outcome = scheduler->submit(
        std::move(request),
        [this, build, chunk, chunk_index,
         generation](const crimson::data::ScheduledDataRequest &scheduled) {
          const auto started = Clock::now();
          if (scheduled.cancellation.cancelled()) {
            return crimson::data::DataResultStatus::Stale;
          }

          try {
            if (!build->columns) {
              auto columns = std::make_shared<
                  crimson::zarr::CanonicalDetectionResidentUiColumns>();
              columns->bbox_norm_coords.resize(descriptor.row_count * 4);
              columns->scores.resize(descriptor.row_count);
              columns->class_ids.resize(descriptor.row_count);
              if (descriptor.stable_identity) {
                columns->instance_keys.resize(descriptor.row_count);
                columns->refined_row_ids.resize(descriptor.row_count);
                columns->source_detect_row_indices.resize(descriptor.row_count);
                columns->source_kind_codes.resize(descriptor.row_count);
                columns->score_valid.resize(descriptor.row_count);
                columns->manual_edit_flags.resize(descriptor.row_count);
              }
              build->columns = std::move(columns);
            }
          } catch (const std::exception &exception) {
            std::lock_guard<std::mutex> lock(mutex);
            if (residency_build == build &&
                residency_metrics.state ==
                    CanonicalDetectionResidencyState::Loading) {
              residency_metrics.state =
                  CanonicalDetectionResidencyState::Failed;
              ++residency_metrics.failed_chunks;
              residency_metrics.last_error =
                  "Canonical detection resident allocation failed: " +
                  std::string(exception.what());
              residency_metrics.elapsed_ms =
                  std::chrono::duration<double, std::milli>(Clock::now() -
                                                            residency_started)
                      .count();
              residency_build.reset();
              condition.notify_all();
            }
            return crimson::data::DataResultStatus::Failed;
          }

          auto rows = repository->readUiRowsForResidency(
              chunk.first_row, chunk.last_row_exclusive);
          const double elapsed_ms =
              std::chrono::duration<double, std::milli>(Clock::now() - started)
                  .count();
          if (scheduled.cancellation.cancelled()) {
            std::lock_guard<std::mutex> lock(mutex);
            ++residency_metrics.stale_chunks;
            condition.notify_all();
            return crimson::data::DataResultStatus::Stale;
          }

          const size_t row_count = chunk.last_row_exclusive - chunk.first_row;
          if (rows.ready() &&
              (rows.bbox_norm_coords.size() != row_count * 4 ||
               rows.scores.size() != row_count ||
               rows.class_ids.size() != row_count ||
               (descriptor.stable_identity &&
                (rows.instance_keys.size() != row_count ||
                 rows.refined_row_ids.size() != row_count ||
                 rows.source_detect_row_indices.size() != row_count ||
                 rows.source_kind_codes.size() != row_count ||
                 rows.score_valid.size() != row_count ||
                 rows.manual_edit_flags.size() != row_count)))) {
            rows.status =
                crimson::zarr::CanonicalDetectionPageStatus::ReadFailed;
            rows.error = "Canonical detection resident chunk shape mismatch";
          }
          if (rows.ready()) {
            std::copy(
                rows.bbox_norm_coords.begin(), rows.bbox_norm_coords.end(),
                build->columns->bbox_norm_coords.begin() + chunk.first_row * 4);
            std::copy(rows.scores.begin(), rows.scores.end(),
                      build->columns->scores.begin() + chunk.first_row);
            std::copy(rows.class_ids.begin(), rows.class_ids.end(),
                      build->columns->class_ids.begin() + chunk.first_row);
            if (descriptor.stable_identity) {
              std::copy(rows.instance_keys.begin(), rows.instance_keys.end(),
                        build->columns->instance_keys.begin() +
                            chunk.first_row);
              std::copy(
                  rows.refined_row_ids.begin(), rows.refined_row_ids.end(),
                  build->columns->refined_row_ids.begin() + chunk.first_row);
              std::copy(rows.source_detect_row_indices.begin(),
                        rows.source_detect_row_indices.end(),
                        build->columns->source_detect_row_indices.begin() +
                            chunk.first_row);
              std::copy(
                  rows.source_kind_codes.begin(), rows.source_kind_codes.end(),
                  build->columns->source_kind_codes.begin() + chunk.first_row);
              std::copy(rows.score_valid.begin(), rows.score_valid.end(),
                        build->columns->score_valid.begin() + chunk.first_row);
              std::copy(
                  rows.manual_edit_flags.begin(), rows.manual_edit_flags.end(),
                  build->columns->manual_edit_flags.begin() + chunk.first_row);
            }
          }

          std::lock_guard<std::mutex> lock(mutex);
          residency_metrics.maximum_chunk_ms =
              std::max(residency_metrics.maximum_chunk_ms, elapsed_ms);
          if (scheduled.cancellation.cancelled() || stopping ||
              residency_build != build || generation != residency_generation ||
              residency_metrics.state !=
                  CanonicalDetectionResidencyState::Loading) {
            ++residency_metrics.stale_chunks;
            condition.notify_all();
            return crimson::data::DataResultStatus::Stale;
          }
          if (!rows.ready()) {
            residency_metrics.state = CanonicalDetectionResidencyState::Failed;
            ++residency_metrics.failed_chunks;
            residency_metrics.last_error = rows.error;
            residency_metrics.elapsed_ms =
                std::chrono::duration<double, std::milli>(Clock::now() -
                                                          residency_started)
                    .count();
            residency_build.reset();
            condition.notify_all();
            return crimson::data::DataResultStatus::Failed;
          }

          ++residency_metrics.completed_chunks;
          residency_metrics.decoded_source_bytes += rows.decoded_bytes;
          if (chunk_index + 1 == build->chunks.size()) {
            std::string publication_error;
            if (!repository->publishResidentUiColumns(build->columns,
                                                      &publication_error)) {
              residency_metrics.state =
                  CanonicalDetectionResidencyState::Failed;
              ++residency_metrics.failed_chunks;
              residency_metrics.last_error = publication_error;
              residency_metrics.elapsed_ms =
                  std::chrono::duration<double, std::milli>(Clock::now() -
                                                            residency_started)
                      .count();
              residency_build.reset();
              condition.notify_all();
              return crimson::data::DataResultStatus::Failed;
            }
            residency_metrics.state = CanonicalDetectionResidencyState::Ready;
            residency_metrics.retained_bytes =
                repository->residentUiColumnBytes();
            ++residency_metrics.publications;
            residency_metrics.elapsed_ms =
                std::chrono::duration<double, std::milli>(Clock::now() -
                                                          residency_started)
                    .count();
            residency_build.reset();
            condition.notify_all();
            return crimson::data::DataResultStatus::Ready;
          }

          if (!submitNextResidencyChunkLocked(&residency_metrics.last_error)) {
            residency_metrics.state = CanonicalDetectionResidencyState::Failed;
            ++residency_metrics.failed_chunks;
            residency_metrics.elapsed_ms =
                std::chrono::duration<double, std::milli>(Clock::now() -
                                                          residency_started)
                    .count();
            residency_build.reset();
            condition.notify_all();
            return crimson::data::DataResultStatus::Failed;
          }
          condition.notify_all();
          return crimson::data::DataResultStatus::Ready;
        });
    if (!outcome.accepted()) {
      assignError(error,
                  "Canonical detection scheduler rejected residency chunk");
      return false;
    }
    ++build->next_chunk;
    return true;
  }
};

CanonicalDetectionBuffer::CanonicalDetectionBuffer(
    std::shared_ptr<crimson::data::DataAccessScheduler> scheduler,
    std::string archive_identity)
    : impl_(std::make_unique<Impl>()) {
  impl_->scheduler =
      scheduler ? std::move(scheduler)
                : std::make_shared<crimson::data::DataAccessScheduler>(32, 1);
  impl_->archive_identity = std::move(archive_identity);
}

CanonicalDetectionBuffer::~CanonicalDetectionBuffer() { close(); }

bool CanonicalDetectionBuffer::open(
    std::unique_ptr<crimson::zarr::CanonicalDetectionRepository> repository,
    size_t page_frames, size_t cache_pages, std::string *error) {
  close();
  if (!repository || !repository->descriptor().ready()) {
    assignError(error, "Canonical detection repository is unavailable");
    return false;
  }
  if (page_frames == 0 || cache_pages < 2) {
    assignError(error, "Canonical detection page/cache policy is invalid");
    return false;
  }
  if (!impl_->scheduler || !impl_->scheduler->running()) {
    assignError(error, "Canonical detection scheduler is unavailable");
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->descriptor = repository->descriptor();
  impl_->scheduler_source = {
      impl_->archive_identity.empty() ? "in_memory" : impl_->archive_identity,
      "canonical_detection",
      impl_->descriptor.source_group + "/" + impl_->descriptor.run_name};
  impl_->residency_source = {
      impl_->archive_identity.empty() ? "in_memory" : impl_->archive_identity,
      "canonical_detection_residency",
      impl_->descriptor.source_group + "/" + impl_->descriptor.run_name};
  impl_->repository = std::move(repository);
  impl_->page_frames = page_frames;
  impl_->cache_pages = cache_pages;
  impl_->generation = 1;
  impl_->last_request = -1;
  impl_->stopping = false;
  impl_->metrics = {};
  impl_->residency_generation = 1;
  impl_->residency_build.reset();
  impl_->residency_metrics = {};
  return true;
}

void CanonicalDetectionBuffer::close() {
  crimson::data::SourceIdentity source;
  crimson::data::SourceIdentity residency_source;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->stopping = true;
    if (impl_->residency_metrics.state ==
        CanonicalDetectionResidencyState::Loading) {
      impl_->residency_metrics.state =
          CanonicalDetectionResidencyState::Cancelled;
      impl_->residency_metrics.elapsed_ms =
          std::chrono::duration<double, std::milli>(Impl::Clock::now() -
                                                    impl_->residency_started)
              .count();
    }
    impl_->condition.notify_all();
    source = impl_->scheduler_source;
    residency_source = impl_->residency_source;
  }
  if (source.valid()) {
    impl_->scheduler->cancelSource(source);
    impl_->scheduler->waitForSourceIdle(source);
  }
  if (residency_source.valid()) {
    impl_->scheduler->cancelSource(residency_source);
    impl_->scheduler->waitForSourceIdle(residency_source);
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->repository.reset();
  impl_->descriptor = {};
  impl_->scheduler_source = {};
  impl_->residency_source = {};
  impl_->residency_build.reset();
  impl_->clearCacheLocked();
  impl_->last_request = -1;
  impl_->stopping = false;
}

bool CanonicalDetectionBuffer::isOpen() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->repository && !impl_->stopping && impl_->scheduler &&
         impl_->scheduler->running();
}

bool CanonicalDetectionBuffer::requestFrame(int64_t camera_frame,
                                            bool discontinuity,
                                            std::string *error) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->repository || impl_->stopping) {
    assignError(error, "Canonical detection buffer is not open");
    return false;
  }
  if (camera_frame < 0 || static_cast<uint64_t>(camera_frame) >=
                              impl_->descriptor.camera_frame_count) {
    assignError(error, "Canonical detection frame is out of range");
    return false;
  }
  ++impl_->metrics.requests;
  const bool reverse =
      impl_->last_request >= 0 && camera_frame < impl_->last_request;
  const bool jumped = impl_->last_request >= 0 &&
                      std::llabs(camera_frame - impl_->last_request) >
                          static_cast<int64_t>(impl_->page_frames);
  if (discontinuity || jumped) {
    ++impl_->generation;
    impl_->scheduler->cancelSource(impl_->scheduler_source);
    impl_->clearCacheLocked();
  }
  impl_->last_request = camera_frame;
  if (impl_->frames.find(camera_frame) != impl_->frames.end()) {
    ++impl_->metrics.cache_hits;
  }

  const int64_t current_start = impl_->pageStart(camera_frame);
  const int64_t current_end = impl_->pageEnd(current_start);
  const int64_t lead_start =
      reverse ? current_start - static_cast<int64_t>(impl_->page_frames)
              : current_start + static_cast<int64_t>(impl_->page_frames);
  const bool lead_valid =
      lead_start >= 0 &&
      static_cast<uint64_t>(lead_start) < impl_->descriptor.camera_frame_count;
  const int64_t retained_first =
      lead_valid ? std::min(current_start, lead_start) : current_start;
  const int64_t retained_last =
      lead_valid ? std::max(current_end, impl_->pageEnd(lead_start))
                 : current_end;
  impl_->scheduler->retainSourceRange(impl_->scheduler_source,
                                      impl_->generation,
                                      {retained_first, retained_last});

  auto submitPage = [&](int64_t page_start, bool demand) {
    if (impl_->hasPageLocked(page_start)) {
      return true;
    }
    const int64_t page_end = impl_->pageEnd(page_start);
    const uint64_t generation = impl_->generation;
    crimson::data::DataRangeRequest request{
        impl_->scheduler_source,
        {page_start, page_end},
        detectionUiFields(impl_->descriptor.stable_identity),
        demand ? crimson::data::RequestPriority::CurrentFrame
               : crimson::data::RequestPriority::VisibleWindow,
        discontinuity ? crimson::data::AccessPattern::RandomSeek
        : reverse     ? crimson::data::AccessPattern::Reverse
                      : crimson::data::AccessPattern::Forward,
        generation};
    const auto outcome = impl_->scheduler->submit(
        std::move(request),
        [this, page_start, page_end,
         generation](const crimson::data::ScheduledDataRequest &scheduled) {
          if (scheduled.cancellation.cancelled()) {
            return crimson::data::DataResultStatus::Stale;
          }
          if (canonicalDetectionTraceEnabled()) {
            std::cerr << "[CanonicalDetectionBuffer] read=begin frames="
                      << page_start << ':' << page_end
                      << " generation=" << generation << '\n';
          }
          const auto started = std::chrono::steady_clock::now();
          auto resolved =
              impl_->repository->resolveCameraFrameRange(page_start, page_end);
          const double elapsed_ms =
              std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - started)
                  .count();
          if (canonicalDetectionTraceEnabled()) {
            std::cerr << "[CanonicalDetectionBuffer] read=end frames="
                      << page_start << ':' << page_end
                      << " generation=" << generation
                      << " elapsed_ms=" << elapsed_ms << '\n';
          }
          std::lock_guard<std::mutex> callback_lock(impl_->mutex);
          impl_->metrics.maximum_resolve_ms =
              std::max(impl_->metrics.maximum_resolve_ms, elapsed_ms);
          if (scheduled.cancellation.cancelled() || impl_->stopping ||
              generation != impl_->generation) {
            ++impl_->metrics.discarded_pages;
            impl_->condition.notify_all();
            return crimson::data::DataResultStatus::Stale;
          }
          if (resolved.status !=
              crimson::zarr::CanonicalDetectionPageStatus::Ready) {
            ++impl_->metrics.failed_pages;
            impl_->metrics.last_error = resolved.error;
            impl_->condition.notify_all();
            return resolved.status ==
                           crimson::zarr::CanonicalDetectionPageStatus::
                               OutOfRange
                       ? crimson::data::DataResultStatus::Missing
                       : crimson::data::DataResultStatus::Failed;
          }
          impl_->publishPageLocked(page_start, resolved);
          ++impl_->metrics.resolved_pages;
          impl_->condition.notify_all();
          return crimson::data::DataResultStatus::Ready;
        });
    if (outcome.accepted()) {
      if (demand) {
        ++impl_->metrics.demand_pages;
      } else {
        ++impl_->metrics.lead_pages;
      }
    }
    return outcome.accepted();
  };

  if (!submitPage(current_start, true)) {
    assignError(error, "Canonical detection scheduler rejected demand page");
    return false;
  }
  if (lead_valid) {
    submitPage(lead_start, false);
  }
  impl_->metrics.peak_pending_pages =
      std::max(impl_->metrics.peak_pending_pages,
               impl_->scheduler->metrics().queue.pending_requests);
  return true;
}

bool CanonicalDetectionBuffer::waitForFrame(
    int64_t camera_frame, std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  return impl_->condition.wait_for(lock, timeout, [&] {
    return impl_->stopping ||
           impl_->frames.find(camera_frame) != impl_->frames.end();
  }) && !impl_->stopping;
}

std::shared_ptr<const crimson::zarr::CanonicalDetectionFrame>
CanonicalDetectionBuffer::frame(int64_t camera_frame) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->frames.find(camera_frame);
  return found == impl_->frames.end() ? nullptr : found->second;
}

bool CanonicalDetectionBuffer::startUiResidency(
    const CanonicalDetectionResidencyPolicy &policy, std::string *error) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->repository || impl_->stopping) {
    assignError(error, "Canonical detection buffer is not open");
    return false;
  }
  if (!policy.enabled()) {
    assignError(error, "Canonical detection residency policy is disabled");
    return false;
  }
  if (impl_->residency_metrics.state ==
          CanonicalDetectionResidencyState::Loading ||
      impl_->residency_metrics.state ==
          CanonicalDetectionResidencyState::Ready) {
    assignError(error, "Canonical detection residency has already started");
    return false;
  }

  impl_->residency_metrics = {};
  impl_->residency_metrics.attempts = 1;
  impl_->residency_metrics.decoded_hot_bytes =
      impl_->repository->decodedUiColumnBytes();
  impl_->residency_metrics.maximum_resident_bytes =
      policy.maximum_resident_bytes;
  impl_->residency_metrics.maximum_chunk_decoded_bytes =
      policy.maximum_chunk_decoded_bytes;
  if (!policy.admits(impl_->residency_metrics.decoded_hot_bytes)) {
    impl_->residency_metrics.state =
        CanonicalDetectionResidencyState::Ineligible;
    impl_->condition.notify_all();
    return true;
  }
  if (impl_->scheduler->metrics().worker_count < 2) {
    impl_->residency_metrics.state = CanonicalDetectionResidencyState::Failed;
    impl_->residency_metrics.last_error =
        "Canonical detection residency requires at least two scheduler workers";
    assignError(error, impl_->residency_metrics.last_error);
    impl_->condition.notify_all();
    return false;
  }

  auto build = std::make_shared<Impl::ResidencyBuild>();
  build->chunks =
      impl_->repository->planUiResidency(policy.maximum_chunk_decoded_bytes);
  if (build->chunks.empty()) {
    impl_->residency_metrics.state = CanonicalDetectionResidencyState::Failed;
    impl_->residency_metrics.last_error =
        "Canonical detection residency produced no chunks";
    assignError(error, impl_->residency_metrics.last_error);
    impl_->condition.notify_all();
    return false;
  }
  impl_->residency_build = std::move(build);
  impl_->residency_metrics.state = CanonicalDetectionResidencyState::Loading;
  impl_->residency_metrics.planned_chunks =
      impl_->residency_build->chunks.size();
  ++impl_->residency_generation;
  impl_->residency_started = Impl::Clock::now();
  if (!impl_->submitNextResidencyChunkLocked(error)) {
    impl_->residency_metrics.state = CanonicalDetectionResidencyState::Failed;
    ++impl_->residency_metrics.failed_chunks;
    impl_->residency_metrics.last_error =
        error ? *error : "Canonical detection residency submission failed";
    impl_->residency_build.reset();
    impl_->condition.notify_all();
    return false;
  }
  return true;
}

bool CanonicalDetectionBuffer::waitForUiResidency(
    std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  const auto terminal = [&] {
    return impl_->residency_metrics.state !=
           CanonicalDetectionResidencyState::Loading;
  };
  return impl_->condition.wait_for(lock, timeout, terminal) && terminal();
}

void CanonicalDetectionBuffer::cancelUiResidency() {
  crimson::data::SourceIdentity source;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->residency_metrics.state !=
        CanonicalDetectionResidencyState::Loading) {
      return;
    }
    impl_->residency_metrics.state =
        CanonicalDetectionResidencyState::Cancelled;
    impl_->residency_metrics.elapsed_ms =
        std::chrono::duration<double, std::milli>(Impl::Clock::now() -
                                                  impl_->residency_started)
            .count();
    source = impl_->residency_source;
    impl_->condition.notify_all();
  }
  if (source.valid()) {
    impl_->scheduler->cancelSource(source);
    impl_->scheduler->waitForSourceIdle(source);
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->residency_build.reset();
  impl_->condition.notify_all();
}

CanonicalDetectionResidencyMetrics
CanonicalDetectionBuffer::residencyMetrics() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->residency_metrics;
}

crimson::zarr::CanonicalDetectionDescriptor
CanonicalDetectionBuffer::descriptor() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->descriptor;
}

CanonicalDetectionBufferMetrics CanonicalDetectionBuffer::metrics() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->metrics;
}

crimson::zarr::CanonicalDetectionRepositoryMetrics
CanonicalDetectionBuffer::repositoryMetrics() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->repository
             ? impl_->repository->metrics()
             : crimson::zarr::CanonicalDetectionRepositoryMetrics{};
}

CanonicalDetectionResidencyPolicy
canonicalDetectionProductionResidencyPolicy() {
  CanonicalDetectionResidencyPolicy policy;
  policy.maximum_resident_bytes = 64ULL * 1024ULL * 1024ULL;
  policy.maximum_chunk_decoded_bytes = 512ULL * 1024ULL;
  return policy;
}

const char *
canonicalDetectionResidencyStateName(CanonicalDetectionResidencyState state) {
  switch (state) {
  case CanonicalDetectionResidencyState::Disabled:
    return "disabled";
  case CanonicalDetectionResidencyState::Ineligible:
    return "ineligible";
  case CanonicalDetectionResidencyState::Loading:
    return "loading";
  case CanonicalDetectionResidencyState::Ready:
    return "ready";
  case CanonicalDetectionResidencyState::Cancelled:
    return "cancelled";
  case CanonicalDetectionResidencyState::Failed:
    return "failed";
  }
  return "unknown";
}
