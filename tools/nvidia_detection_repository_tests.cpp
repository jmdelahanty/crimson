#include "platform/nvidia/nvidia_detection_repository.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__           \
                << ": " #condition << '\n';                                    \
      return false;                                                            \
    }                                                                          \
  } while (false)

bool near(float left, float right) {
  return std::abs(left - right) < 1.0e-4f;
}

crimson::zarr::CanonicalDetectionDescriptor
makeDescriptor(size_t frame_count = 2'592'041, size_t source_width = 1000,
               size_t source_height = 500) {
  crimson::zarr::CanonicalDetectionDescriptor descriptor;
  descriptor.source_group = "detect_runs";
  descriptor.run_name = "august_raw_fixture";
  descriptor.instance_group = "instances";
  descriptor.row_count = frame_count;
  descriptor.camera_frame_count = frame_count;
  descriptor.source_width = source_width;
  descriptor.source_height = source_height;
  descriptor.retained_offset_bytes = (frame_count + 1) * sizeof(int64_t);
  descriptor.offset_read_calls = 1;
  descriptor.consolidated_metadata = true;
  descriptor.identity_authority =
      crimson::zarr::CanonicalDetectionIdentityAuthority::
          PublishedInstanceKeyV1;
  return descriptor;
}

struct FakeRepositoryState {
  mutable std::mutex mutex;
  mutable std::condition_variable condition;
  size_t reads_started = 0;
  size_t reads_finished = 0;
  size_t reads_to_block = 0;
  bool release_reads = false;
  size_t destructor_calls = 0;
  std::vector<std::pair<int64_t, int64_t>> ranges;
  std::set<int64_t> empty_frames;
  std::set<int64_t> invalid_box_frames;

  bool waitForReads(size_t count, std::chrono::milliseconds timeout = 2s) {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(lock, timeout,
                              [&] { return reads_started >= count; });
  }

  bool waitForFinished(size_t count,
                       std::chrono::milliseconds timeout = 2s) {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(lock, timeout,
                              [&] { return reads_finished >= count; });
  }

  void release() {
    std::lock_guard<std::mutex> lock(mutex);
    release_reads = true;
    condition.notify_all();
  }

  size_t readCount() const {
    std::lock_guard<std::mutex> lock(mutex);
    return reads_started;
  }

  std::vector<std::pair<int64_t, int64_t>> observedRanges() const {
    std::lock_guard<std::mutex> lock(mutex);
    return ranges;
  }
};

class FakeCanonicalDetectionRepository final
    : public crimson::zarr::CanonicalDetectionRepository {
public:
  FakeCanonicalDetectionRepository(
      crimson::zarr::CanonicalDetectionDescriptor descriptor,
      std::shared_ptr<FakeRepositoryState> state)
      : descriptor_(std::move(descriptor)), state_(std::move(state)) {}

  ~FakeCanonicalDetectionRepository() override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    ++state_->destructor_calls;
    state_->condition.notify_all();
  }

  const crimson::zarr::CanonicalDetectionDescriptor &
  descriptor() const override {
    return descriptor_;
  }

  crimson::zarr::CanonicalDetectionPage
  resolveCameraFrameRange(int64_t first, int64_t last) const override {
    crimson::zarr::CanonicalDetectionPage page;
    page.first_camera_frame = first;
    page.last_camera_frame = last;
    if (first < 0 || last < first ||
        static_cast<uint64_t>(last) >= descriptor_.camera_frame_count) {
      page.status = crimson::zarr::CanonicalDetectionPageStatus::OutOfRange;
      return page;
    }

    {
      std::unique_lock<std::mutex> lock(state_->mutex);
      state_->ranges.emplace_back(first, last);
      ++state_->reads_started;
      const bool block = state_->reads_to_block > 0;
      if (block) {
        --state_->reads_to_block;
      }
      state_->condition.notify_all();
      if (block) {
        state_->condition.wait(lock, [&] { return state_->release_reads; });
      }
    }

    for (int64_t camera_frame = first; camera_frame <= last; ++camera_frame) {
      crimson::zarr::CanonicalDetectionFrame frame;
      frame.camera_frame = camera_frame;
      bool empty = false;
      bool include_invalid = false;
      {
        std::lock_guard<std::mutex> lock(state_->mutex);
        empty = state_->empty_frames.count(camera_frame) != 0;
        include_invalid =
            state_->invalid_box_frames.count(camera_frame) != 0;
      }
      if (!empty) {
        frame.detections.push_back(makeDetection(camera_frame));
        if (include_invalid) {
          auto zero_width = makeDetection(camera_frame);
          zero_width.row_index = camera_frame * 4 + 1;
          zero_width.normalized_cxcywh[2] = 0.0f;
          frame.detections.push_back(zero_width);
          auto non_finite = makeDetection(camera_frame);
          non_finite.row_index = camera_frame * 4 + 2;
          non_finite.normalized_cxcywh[0] =
              std::numeric_limits<float>::quiet_NaN();
          frame.detections.push_back(non_finite);
          auto negative_height = makeDetection(camera_frame);
          negative_height.row_index = camera_frame * 4 + 3;
          negative_height.normalized_cxcywh[3] = -0.1f;
          frame.detections.push_back(negative_height);
        }
      }
      page.frames.push_back(std::move(frame));
    }
    page.decoded_bytes = page.frames.size() * 24;
    page.status = crimson::zarr::CanonicalDetectionPageStatus::Ready;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      ++state_->reads_finished;
      metrics_.range_reads++;
      metrics_.paged_range_reads++;
      metrics_.resolved_frames += page.frames.size();
      for (const auto &frame : page.frames) {
        metrics_.resolved_rows += frame.detections.size();
      }
      metrics_.ui_field_reads += 3;
      state_->condition.notify_all();
    }
    return page;
  }

  uint64_t decodedUiColumnBytes() const override { return 0; }

  std::vector<crimson::zarr::CanonicalDetectionUiResidencyChunk>
  planUiResidency(uint64_t) const override {
    return {};
  }

  crimson::zarr::CanonicalDetectionUiRows
  readUiRowsForResidency(size_t, size_t) const override {
    return {};
  }

  bool publishResidentUiColumns(
      std::shared_ptr<
          const crimson::zarr::CanonicalDetectionResidentUiColumns>,
      std::string *error) override {
    if (error) {
      *error = "Fixture does not support UI residency";
    }
    return false;
  }

  bool residentUiColumnsReady() const override { return false; }
  uint64_t residentUiColumnBytes() const override { return 0; }

  crimson::zarr::CanonicalDetectionRepositoryMetrics metrics() const override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return metrics_;
  }

private:
  static crimson::zarr::CanonicalDetection makeDetection(int64_t frame) {
    crimson::zarr::CanonicalDetection detection;
    detection.row_index = frame * 4;
    detection.instance_key =
        frame == 54'000
            ? uint64_t{0}
            : frame == 53'999
                  ? std::numeric_limits<uint64_t>::max() - 3
                  : static_cast<uint64_t>(frame) + 10;
    detection.instance_key_valid = true;
    detection.source_kind_code = 2;
    detection.score_valid = true;
    detection.normalized_cxcywh = {0.5f, 0.25f, 0.2f, 0.1f};
    detection.score = 0.875f;
    detection.class_id = 7;
    return detection;
  }

  crimson::zarr::CanonicalDetectionDescriptor descriptor_;
  std::shared_ptr<FakeRepositoryState> state_;
  mutable crimson::zarr::CanonicalDetectionRepositoryMetrics metrics_;
};

struct CallState {
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool done = false;
  bool result = false;
  std::string error;

  void markEntered() {
    std::lock_guard<std::mutex> lock(mutex);
    entered = true;
    condition.notify_all();
  }

  void finish(bool value) {
    std::lock_guard<std::mutex> lock(mutex);
    result = value;
    done = true;
    condition.notify_all();
  }

  bool waitForDone(std::chrono::milliseconds timeout = 2s) {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(lock, timeout, [&] { return done; });
  }

  bool waitForEntered(std::chrono::milliseconds timeout = 2s) {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(lock, timeout, [&] { return entered; });
  }
};

std::unique_ptr<FakeCanonicalDetectionRepository>
makeRepository(const crimson::zarr::CanonicalDetectionDescriptor &descriptor,
               const std::shared_ptr<FakeRepositoryState> &state) {
  return std::make_unique<FakeCanonicalDetectionRepository>(descriptor, state);
}

crimson::platform::nvidia::NvidiaDetectionOpenRequest
makeOpenRequest(size_t page_frames = 10, size_t cache_pages = 2) {
  crimson::platform::nvidia::NvidiaDetectionOpenRequest request;
  request.archive_path = "in_memory_august_fixture";
  request.canonical_raw_run = "august_raw_fixture";
  request.frames_per_second = 50.0;
  request.expected_camera_frame_count = 2'592'041;
  request.expected_source_width = 1000;
  request.expected_source_height = 500;
  request.page_frames = page_frames;
  request.cache_pages = cache_pages;
  return request;
}

bool hasRange(const std::vector<std::pair<int64_t, int64_t>> &ranges,
              int64_t first, int64_t last) {
  return std::find(ranges.begin(), ranges.end(),
                   std::make_pair(first, last)) != ranges.end();
}

bool testCacheOnlyResolveAndNonblockingRequest() {
  auto scheduler =
      std::make_shared<crimson::data::DataAccessScheduler>(8, 2, 1, 1);
  auto state = std::make_shared<FakeRepositoryState>();
  state->reads_to_block = 1;
  state->empty_frames.insert(54'001);
  state->invalid_box_frames.insert(54'002);

  crimson::platform::nvidia::NvidiaDetectionRepository repository(scheduler);
  std::string error;
  CHECK(repository.openForTesting(makeRepository(makeDescriptor(), state),
                                  makeOpenRequest(), &error));
  CHECK(error.empty());
  CHECK(repository.state() ==
        crimson::platform::nvidia::NvidiaDetectionState::Ready);

  const auto descriptor = repository.descriptor();
  CHECK(descriptor.available);
  CHECK(descriptor.active_dataset ==
        crimson::zarr::DetectionDataset::RawDetect);
  CHECK(!descriptor.coordinates_normalized);
  CHECK(descriptor.has_validated_instance_keys);
  CHECK(descriptor.source_width == 1000);
  CHECK(descriptor.source_height == 500);
  CHECK(!descriptor.activeDatasetAllowsBboxEditing());

  const auto cold = repository.resolveFrame(54'000, false);
  CHECK(cold.status == crimson::zarr::DetectionFrameStatus::Unavailable);
  CHECK(state->readCount() == 0);
  CHECK(repository.observationCount(54'000) == 0);
  CHECK(state->readCount() == 0);

  CallState request_call;
  std::thread requester([&] {
    request_call.markEntered();
    const bool result =
        repository.requestPresentedFrame(54'000, false, &request_call.error);
    request_call.finish(result);
  });
  const bool read_started = state->waitForReads(1);
  const bool returned_while_storage_blocked = request_call.waitForDone(500ms);
  state->release();
  requester.join();
  CHECK(read_started);
  CHECK(returned_while_storage_blocked);
  CHECK(request_call.result);
  CHECK(request_call.error.empty());

  scheduler->waitUntilIdle();
  CHECK(state->readCount() == 2);
  const auto ready = repository.resolveFrame(54'000, false);
  CHECK(ready.status == crimson::zarr::DetectionFrameStatus::Ready);
  CHECK(ready.frame_id == 54'000);
  CHECK(ready.observations.size() == 1);
  const auto &observation = ready.observations.front();
  CHECK(observation.ordinal == 0);
  CHECK(observation.canonical_row_index == 216'000);
  CHECK(observation.instance_key == 0);
  CHECK(observation.instance_key_valid);
  CHECK(near(observation.box_xyxy[0], 400.0f));
  CHECK(near(observation.box_xyxy[1], 100.0f));
  CHECK(near(observation.box_xyxy[2], 600.0f));
  CHECK(near(observation.box_xyxy[3], 150.0f));
  CHECK(near(observation.score, 0.875f));
  CHECK(observation.class_id == 7);
  CHECK(observation.score_valid);
  CHECK(observation.class_id_valid);
  CHECK(repository.observationCount(54'000) == 1);

  const auto empty = repository.resolveFrame(54'001, false);
  CHECK(empty.status == crimson::zarr::DetectionFrameStatus::Ready);
  CHECK(empty.frame_id == 54'001);
  CHECK(empty.observations.empty());
  CHECK(repository.observationCount(54'001) == 0);
  // A malformed canonical row fails the whole cached frame closed. Silently
  // omitting it could turn corrupt payload into a misleading Ready+empty frame.
  const auto invalid = repository.resolveFrame(54'002, false);
  CHECK(invalid.status ==
        crimson::zarr::DetectionFrameStatus::Unavailable);
  CHECK(invalid.frame_id == 54'002);
  CHECK(invalid.observations.empty());
  CHECK(!repository.metrics().last_error.empty());
  const auto out_of_range = repository.resolveFrame(2'592'041, false);
  CHECK(out_of_range.status ==
        crimson::zarr::DetectionFrameStatus::OutOfRange);

  const size_t payload_reads = state->readCount();
  (void)repository.resolveFrame(54'000, false);
  (void)repository.observationCount(54'000);
  CHECK(state->readCount() == payload_reads);

  const auto datasets = repository.availableDatasets();
  CHECK(datasets.size() == 1);
  CHECK(datasets.front().dataset ==
        crimson::zarr::DetectionDataset::RawDetect);
  CHECK(repository.isDatasetAvailable(
      crimson::zarr::DetectionDataset::RawDetect));
  CHECK(!repository.isDatasetAvailable(
      crimson::zarr::DetectionDataset::RefinedRoot));
  CHECK(repository.selectDataset(crimson::zarr::DetectionDataset::RawDetect));
  CHECK(!repository.selectDataset(
      crimson::zarr::DetectionDataset::RefinedManual));
  CHECK(!repository.isFrameInterpolated(54'000));

  const auto metrics = repository.metrics();
  CHECK(metrics.frame_requests == 1);
  CHECK(metrics.buffer.demand_pages == 1);
  CHECK(metrics.buffer.lead_pages == 1);
  CHECK(metrics.buffer.peak_cached_pages <= 2);
  CHECK(!metrics.page_cache_hard_byte_budgeted);
  repository.close();
  scheduler->shutdown();
  return true;
}

bool testExactParentBoundariesAndBoundedPages() {
  auto scheduler =
      std::make_shared<crimson::data::DataAccessScheduler>(8, 2, 1, 1);
  auto state = std::make_shared<FakeRepositoryState>();
  crimson::platform::nvidia::NvidiaDetectionRepository repository(scheduler);
  std::string error;
  CHECK(repository.openForTesting(makeRepository(makeDescriptor(), state),
                                  makeOpenRequest(10, 2), &error));

  CHECK(repository.requestPresentedFrame(53'999, false, &error));
  scheduler->waitUntilIdle();
  CHECK(repository.resolveFrame(53'999, false).frame_id == 53'999);
  CHECK(repository.resolveFrame(53'999, false).observations.front().instance_key ==
        std::numeric_limits<uint64_t>::max() - 3);
  CHECK(repository.resolveFrame(53'999, false)
            .observations.front()
            .instance_key_valid);
  CHECK(repository.resolveFrame(54'000, false).frame_id == 54'000);

  // The mounted August clip set has a late unequal boundary at parent frame
  // 2,592,030 and ends at 2,592,041. The last lead page is intentionally short.
  CHECK(repository.requestPresentedFrame(2'592'030, true, &error));
  scheduler->waitUntilIdle();
  const auto boundary = repository.resolveFrame(2'592'030, false);
  const auto last = repository.resolveFrame(2'592'040, false);
  CHECK(boundary.ready() && boundary.frame_id == 2'592'030);
  CHECK(last.ready() && last.frame_id == 2'592'040);

  const auto ranges = state->observedRanges();
  CHECK(hasRange(ranges, 53'990, 53'999));
  CHECK(hasRange(ranges, 54'000, 54'009));
  CHECK(hasRange(ranges, 2'592'030, 2'592'039));
  CHECK(hasRange(ranges, 2'592'040, 2'592'040));

  // Churn two adjacent reverse pages without a discontinuity so the
  // page-count bound is exercised through real eviction accounting.
  CHECK(repository.requestPresentedFrame(2'592'029, false, &error));
  scheduler->waitUntilIdle();
  const auto metrics = repository.metrics();
  CHECK(metrics.buffer.demand_pages == 3);
  CHECK(metrics.buffer.lead_pages == 3);
  CHECK(metrics.buffer.peak_cached_pages <= 2);
  CHECK(metrics.buffer.evicted_pages >= 2);
  CHECK(repository.resolveFrame(53'999, false).status ==
        crimson::zarr::DetectionFrameStatus::Unavailable);
  CHECK(repository.resolveFrame(2'592'040, false).status ==
        crimson::zarr::DetectionFrameStatus::Unavailable);
  repository.close();
  scheduler->shutdown();
  return true;
}

bool testSeekAndReopenRejectStaleCompletion() {
  auto scheduler =
      std::make_shared<crimson::data::DataAccessScheduler>(12, 2, 1, 1);
  auto state = std::make_shared<FakeRepositoryState>();
  state->reads_to_block = 1;
  crimson::platform::nvidia::NvidiaDetectionRepository repository(scheduler);
  std::string error;
  CHECK(repository.openForTesting(makeRepository(makeDescriptor(), state),
                                  makeOpenRequest(), &error));
  CHECK(repository.requestPresentedFrame(100, false, &error));
  CHECK(state->waitForReads(1));

  // A discontinuous request advances the buffer generation. Its current page
  // may complete while the cancelled old page is still blocked.
  CHECK(repository.requestPresentedFrame(54'000, true, &error));
  CHECK(state->waitForReads(2));
  CHECK(state->waitForFinished(1));
  CHECK(repository.resolveFrame(54'000, false).ready());
  state->release();
  scheduler->waitUntilIdle();
  CHECK(repository.resolveFrame(100, false).status ==
        crimson::zarr::DetectionFrameStatus::Unavailable);
  CHECK(repository.metrics().buffer.discarded_pages >= 1);

  auto old_state = std::make_shared<FakeRepositoryState>();
  old_state->reads_to_block = 1;
  CHECK(repository.openForTesting(makeRepository(makeDescriptor(), old_state),
                                  makeOpenRequest(), &error));
  CHECK(repository.requestPresentedFrame(200, false, &error));
  CHECK(old_state->waitForReads(1));

  auto replacement_state = std::make_shared<FakeRepositoryState>();
  CallState reopen_call;
  std::thread reopener([&] {
    reopen_call.markEntered();
    const bool result = repository.openForTesting(
        makeRepository(makeDescriptor(), replacement_state), makeOpenRequest(),
        &reopen_call.error);
    reopen_call.finish(result);
  });
  CHECK(reopen_call.waitForEntered());
  // Release the old in-flight storage call only after reopen has begun. The
  // close path drains it before adopting the replacement repository.
  old_state->release();
  CHECK(reopen_call.waitForDone());
  reopener.join();
  CHECK(reopen_call.result);
  CHECK(repository.state() ==
        crimson::platform::nvidia::NvidiaDetectionState::Ready);
  CHECK(repository.resolveFrame(200, false).status ==
        crimson::zarr::DetectionFrameStatus::Unavailable);
  CHECK(repository.requestPresentedFrame(54'000, false, &error));
  scheduler->waitUntilIdle();
  CHECK(repository.resolveFrame(54'000, false).ready());
  {
    std::lock_guard<std::mutex> lock(old_state->mutex);
    CHECK(old_state->destructor_calls == 1);
  }

  repository.close();
  scheduler->shutdown();
  return true;
}

bool testDestructionWaitsForBlockedWorkSafely() {
  auto scheduler =
      std::make_shared<crimson::data::DataAccessScheduler>(8, 2, 1, 1);
  auto state = std::make_shared<FakeRepositoryState>();
  state->reads_to_block = 1;
  auto repository =
      std::make_unique<crimson::platform::nvidia::NvidiaDetectionRepository>(
          scheduler);
  std::string error;
  CHECK(repository->openForTesting(makeRepository(makeDescriptor(), state),
                                   makeOpenRequest(), &error));
  CHECK(repository->requestPresentedFrame(300, false, &error));
  CHECK(state->waitForReads(1));

  CallState destruction;
  std::thread destroyer([&] {
    destruction.markEntered();
    repository.reset();
    destruction.finish(true);
  });
  CHECK(destruction.waitForEntered());
  state->release();
  CHECK(destruction.waitForDone());
  destroyer.join();
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    CHECK(state->reads_finished >= 1);
    CHECK(state->destructor_calls == 1);
  }
  scheduler->shutdown();
  return true;
}

bool testOpenFailureIsTerminalAndNeverFallsBack() {
  auto scheduler =
      std::make_shared<crimson::data::DataAccessScheduler>(8, 2, 1, 1);

  auto invalid_state = std::make_shared<FakeRepositoryState>();
  crimson::platform::nvidia::NvidiaDetectionRepository invalid(scheduler);
  std::string error;
  CHECK(!invalid.openForTesting(
      makeRepository(makeDescriptor(2'592'041, 0, 500), invalid_state),
      makeOpenRequest(), &error));
  CHECK(!error.empty());
  CHECK(invalid.state() ==
        crimson::platform::nvidia::NvidiaDetectionState::Failed);
  CHECK(!invalid.descriptor().available);
  CHECK(invalid.availableDatasets().empty());

  struct OpenGate {
    std::mutex mutex;
    std::condition_variable condition;
    bool started = false;
    bool release = false;
  } gate;
  crimson::platform::nvidia::NvidiaDetectionStorageOpener failing_opener =
      [&](const crimson::platform::nvidia::NvidiaDetectionOpenRequest &,
          crimson::zarr::DetectionRepositorySelectionMetrics *,
          std::string *open_error)
      -> std::unique_ptr<crimson::zarr::CanonicalDetectionRepository> {
    {
      std::unique_lock<std::mutex> lock(gate.mutex);
      gate.started = true;
      gate.condition.notify_all();
      gate.condition.wait(lock, [&] { return gate.release; });
    }
    if (open_error) {
      *open_error = "Unsupported canonical fixture";
    }
    return nullptr;
  };
  crimson::platform::nvidia::NvidiaDetectionRepository failed(
      scheduler, std::move(failing_opener));
  auto request = makeOpenRequest();
  request.archive_path = "unique_missing_august_fixture.zarr";
  CHECK(failed.beginOpen(request, &error));
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    CHECK(gate.condition.wait_for(lock, 2s, [&] { return gate.started; }));
  }
  CHECK(failed.state() ==
        crimson::platform::nvidia::NvidiaDetectionState::Opening);
  CHECK(!failed.waitUntilOpen(0ms));
  CHECK(failed.resolveFrame(54'000, false).status ==
        crimson::zarr::DetectionFrameStatus::Unavailable);
  {
    std::lock_guard<std::mutex> lock(gate.mutex);
    gate.release = true;
    gate.condition.notify_all();
  }
  CHECK(failed.waitUntilOpen(2s));
  CHECK(failed.state() ==
        crimson::platform::nvidia::NvidiaDetectionState::Failed);
  CHECK(!failed.descriptor().available);
  CHECK(failed.availableDatasets().empty());
  const auto metrics = failed.metrics();
  CHECK(metrics.open_attempts == 1);
  CHECK(metrics.successful_opens == 0);
  CHECK(metrics.failed_opens == 1);
  CHECK(!metrics.last_error.empty());

  // Legacy fallback is intentionally owned by the NVIDIA composition root,
  // not silently installed after a canonical bridge failure.
  CHECK(!failed.isDatasetAvailable(crimson::zarr::DetectionDataset::RawDetect));
  failed.close();
  invalid.close();
  scheduler->shutdown();
  return true;
}

bool testExpectedVideoContractMismatchesFailClosed() {
  auto scheduler =
      std::make_shared<crimson::data::DataAccessScheduler>(8, 2, 1, 1);
  crimson::platform::nvidia::NvidiaDetectionRepository repository(scheduler);
  std::string error;

  auto identity_unavailable = makeDescriptor();
  identity_unavailable.identity_authority =
      crimson::zarr::CanonicalDetectionIdentityAuthority::Unavailable;
  CHECK(!repository.openForTesting(
      makeRepository(identity_unavailable,
                     std::make_shared<FakeRepositoryState>()),
      makeOpenRequest(), &error));
  CHECK(repository.state() ==
        crimson::platform::nvidia::NvidiaDetectionState::Failed);
  CHECK(!error.empty());

  auto frame_request = makeOpenRequest();
  frame_request.expected_camera_frame_count = 2'592'040;
  CHECK(!repository.openForTesting(
      makeRepository(makeDescriptor(),
                     std::make_shared<FakeRepositoryState>()),
      frame_request, &error));
  CHECK(repository.state() ==
        crimson::platform::nvidia::NvidiaDetectionState::Failed);
  CHECK(!repository.descriptor().available);

  auto width_request = makeOpenRequest();
  width_request.expected_source_width = 999;
  error.clear();
  CHECK(!repository.openForTesting(
      makeRepository(makeDescriptor(),
                     std::make_shared<FakeRepositoryState>()),
      width_request, &error));
  CHECK(repository.state() ==
        crimson::platform::nvidia::NvidiaDetectionState::Failed);
  CHECK(!error.empty());

  auto height_request = makeOpenRequest();
  height_request.expected_source_height = 501;
  error.clear();
  CHECK(!repository.openForTesting(
      makeRepository(makeDescriptor(),
                     std::make_shared<FakeRepositoryState>()),
      height_request, &error));
  CHECK(repository.state() ==
        crimson::platform::nvidia::NvidiaDetectionState::Failed);
  CHECK(!repository.descriptor().available);
  repository.close();
  scheduler->shutdown();
  return true;
}

bool testExpectedRecordingIdentityIsExactWhenRequired() {
  auto scheduler =
      std::make_shared<crimson::data::DataAccessScheduler>(8, 2, 1, 1);
  crimson::platform::nvidia::NvidiaDetectionRepository repository(scheduler);
  constexpr const char *expected_identity =
      "2026_08_06_19_13_35_cam2010093";
  auto request = makeOpenRequest();
  request.expected_recording_identity = expected_identity;
  std::string error;

  // Frame count, source dimensions, and run are identical in all three cases;
  // only the validated manifest recording identity changes.
  CHECK(!repository.openForTesting(
      makeRepository(makeDescriptor(),
                     std::make_shared<FakeRepositoryState>()),
      request, &error));
  CHECK(repository.state() ==
        crimson::platform::nvidia::NvidiaDetectionState::Failed);
  CHECK(!error.empty());
  CHECK(!repository.descriptor().available);

  auto mismatched = makeDescriptor();
  mismatched.recording_identity = "2026_08_06_19_13_35_cam2010094";
  error.clear();
  CHECK(!repository.openForTesting(
      makeRepository(mismatched, std::make_shared<FakeRepositoryState>()),
      request, &error));
  CHECK(repository.state() ==
        crimson::platform::nvidia::NvidiaDetectionState::Failed);
  CHECK(!error.empty());
  CHECK(!repository.descriptor().available);

  auto exact = makeDescriptor();
  exact.recording_identity = expected_identity;
  error.clear();
  CHECK(repository.openForTesting(
      makeRepository(exact, std::make_shared<FakeRepositoryState>()), request,
      &error));
  CHECK(error.empty());
  CHECK(repository.state() ==
        crimson::platform::nvidia::NvidiaDetectionState::Ready);
  CHECK(repository.descriptor().available);
  repository.close();
  scheduler->shutdown();
  return true;
}

bool testThrownOpenerBecomesFailedState() {
  auto scheduler =
      std::make_shared<crimson::data::DataAccessScheduler>(8, 2, 1, 1);
  crimson::platform::nvidia::NvidiaDetectionStorageOpener throwing_opener =
      [](const crimson::platform::nvidia::NvidiaDetectionOpenRequest &,
         crimson::zarr::DetectionRepositorySelectionMetrics *,
         std::string *)
      -> std::unique_ptr<crimson::zarr::CanonicalDetectionRepository> {
    throw std::runtime_error("fixture opener exception");
  };
  crimson::platform::nvidia::NvidiaDetectionRepository repository(
      scheduler, std::move(throwing_opener));
  std::string error;
  CHECK(repository.beginOpen(makeOpenRequest(), &error));
  CHECK(repository.waitUntilOpen(2s));
  CHECK(repository.state() ==
        crimson::platform::nvidia::NvidiaDetectionState::Failed);
  CHECK(!repository.descriptor().available);
  const auto metrics = repository.metrics();
  CHECK(metrics.open_attempts == 1);
  CHECK(metrics.failed_opens == 1);
  CHECK(metrics.successful_opens == 0);
  CHECK(!metrics.last_error.empty());
  repository.close();
  scheduler->shutdown();
  return true;
}

bool testArchiveIdentityKeepsSharedSchedulerWorkIndependent() {
  auto scheduler =
      std::make_shared<crimson::data::DataAccessScheduler>(12, 2, 1, 1);
  auto first_state = std::make_shared<FakeRepositoryState>();
  first_state->reads_to_block = 1;
  auto second_state = std::make_shared<FakeRepositoryState>();
  crimson::platform::nvidia::NvidiaDetectionRepository first(scheduler);
  crimson::platform::nvidia::NvidiaDetectionRepository second(scheduler);

  auto first_request = makeOpenRequest();
  first_request.archive_path = "august_fixture_camera_2010093";
  auto second_request = makeOpenRequest();
  second_request.archive_path = "august_fixture_camera_2010094";
  std::string error;
  CHECK(first.openForTesting(makeRepository(makeDescriptor(), first_state),
                             first_request, &error));
  CHECK(second.openForTesting(makeRepository(makeDescriptor(), second_state),
                              second_request, &error));

  CHECK(first.requestPresentedFrame(54'000, false, &error));
  CHECK(first_state->waitForReads(1));
  CHECK(second.requestPresentedFrame(54'000, false, &error));
  const bool independent_second_read = second_state->waitForReads(1, 1s);
  first_state->release();
  scheduler->waitUntilIdle();
  CHECK(independent_second_read);
  CHECK(first.resolveFrame(54'000, false).ready());
  CHECK(second.resolveFrame(54'000, false).ready());
  CHECK(first.metrics().archive_path != second.metrics().archive_path);

  first.close();
  second.close();
  scheduler->shutdown();
  return true;
}

bool testOpeningReplacementHidesOldSourceFrames() {
  auto scheduler =
      std::make_shared<crimson::data::DataAccessScheduler>(8, 2, 1, 1);
  struct OpenGate {
    std::mutex mutex;
    std::condition_variable condition;
    bool started = false;
    bool release = false;
  } gate;
  auto replacement_state = std::make_shared<FakeRepositoryState>();
  crimson::platform::nvidia::NvidiaDetectionStorageOpener opener =
      [&](const crimson::platform::nvidia::NvidiaDetectionOpenRequest &,
          crimson::zarr::DetectionRepositorySelectionMetrics *, std::string *) {
        std::unique_lock<std::mutex> lock(gate.mutex);
        gate.started = true;
        gate.condition.notify_all();
        gate.condition.wait(lock, [&] { return gate.release; });
        return makeRepository(makeDescriptor(), replacement_state);
      };
  crimson::platform::nvidia::NvidiaDetectionRepository repository(
      scheduler, std::move(opener));
  auto old_state = std::make_shared<FakeRepositoryState>();
  std::string error;
  CHECK(repository.openForTesting(makeRepository(makeDescriptor(), old_state),
                                  makeOpenRequest(), &error));
  CHECK(repository.requestPresentedFrame(54'000, false, &error));
  scheduler->waitUntilIdle();
  CHECK(repository.resolveFrame(54'000, false).ready());

  auto replacement_request = makeOpenRequest();
  replacement_request.archive_path = "replacement_august_fixture";
  CHECK(repository.beginOpen(replacement_request, &error));
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    CHECK(gate.condition.wait_for(lock, 2s, [&] { return gate.started; }));
  }
  CHECK(repository.state() ==
        crimson::platform::nvidia::NvidiaDetectionState::Opening);
  CHECK(repository.resolveFrame(54'000, false).status ==
        crimson::zarr::DetectionFrameStatus::Unavailable);
  CHECK(!repository.descriptor().available);
  {
    std::lock_guard<std::mutex> lock(gate.mutex);
    gate.release = true;
    gate.condition.notify_all();
  }
  CHECK(repository.waitUntilOpen(2s));
  CHECK(repository.state() ==
        crimson::platform::nvidia::NvidiaDetectionState::Ready);
  CHECK(repository.resolveFrame(54'000, false).status ==
        crimson::zarr::DetectionFrameStatus::Unavailable);
  repository.close();
  scheduler->shutdown();
  return true;
}

} // namespace

int main() {
  if (!testCacheOnlyResolveAndNonblockingRequest() ||
      !testExactParentBoundariesAndBoundedPages() ||
      !testSeekAndReopenRejectStaleCompletion() ||
      !testDestructionWaitsForBlockedWorkSafely() ||
      !testOpenFailureIsTerminalAndNeverFallsBack() ||
      !testExpectedVideoContractMismatchesFailClosed() ||
      !testExpectedRecordingIdentityIsExactWhenRequired() ||
      !testThrownOpenerBecomesFailedState() ||
      !testArchiveIdentityKeepsSharedSchedulerWorkIndependent() ||
      !testOpeningReplacementHidesOldSourceFrames()) {
    return 1;
  }
  std::cout << "nvidia_detection_repository_tests: PASS\n";
  return 0;
}
