#include "gui/canonical_overlay_session.h"

#include <chrono>
#include <condition_variable>
#include <algorithm>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using crimson::gui::CanonicalOverlayState;
namespace crimson::gui {
// This target tests lifecycle/scheduling with deterministic injected readers.
// The real archive factory is covered by the separate August probe.
CanonicalOverlayRepositories openCanonicalOverlayRepositories(const CanonicalOverlayOpenRequest&) {
  CanonicalOverlayRepositories result;
  result.error = "Test requires an injected opener";
  return result;
}
}

#define CHECK(condition) do { if (!(condition)) { std::cerr << "CHECK failed: " #condition \
  << " line " << __LINE__ << "\n"; return false; } } while (false)

struct Gate {
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool released = false;
  void block() {
    std::unique_lock<std::mutex> lock(mutex);
    entered = true; condition.notify_all();
    condition.wait(lock, [&] { return released; });
  }
  bool wait() {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(lock, 3s, [&] { return entered; });
  }
  void release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true; condition.notify_all();
  }
};
class Reader : public crimson::zarr::KeypointOverlayRepository {
 public:
  Reader(std::string name, std::shared_ptr<Gate> gate = {})
      : gate_(std::move(gate)) {
    descriptor_.run_name = std::move(name);
    descriptor_.source_group = "bound_keypoints";
    descriptor_.camera_frame_count = 20;
  }
  const crimson::zarr::KeypointOverlayDescriptor& descriptor() const override { return descriptor_; }
  crimson::zarr::KeypointOverlayResolution resolveCameraFrame(int64_t frame, int, int) const override {
    if (frame == 4 && gate_) gate_->block();
    if (frame == 7) throw std::runtime_error("deliberate storage failure");
    crimson::zarr::KeypointOverlayResolution result;
    result.camera_frame = frame;
    if (frame == 8) {
      result.status = crimson::zarr::KeypointOverlayStatus::OutOfRange;
      return result;
    }
    result.status = frame >= 10 ? crimson::zarr::KeypointOverlayStatus::Missing
                               : crimson::zarr::KeypointOverlayStatus::Mapped;
    if (frame < 10) {
      crimson::zarr::KeypointOverlayDetection detection;
      detection.instance_key = 0;
      detection.instance_key_valid = true;
      detection.keypoints = {{2, 3}};
      detection.keypoint_valid = {1};
      result.detections.push_back(detection);
    }
    return result;
  }
 private:
  crimson::zarr::KeypointOverlayDescriptor descriptor_;
  std::shared_ptr<Gate> gate_;
};

struct EyeReaderState {
  std::mutex mutex;
  std::vector<int64_t> reads;
  std::shared_ptr<Gate> gate;
};
class EyeReader : public crimson::zarr::EyeGeometryOverlayRepository {
 public:
  explicit EyeReader(std::shared_ptr<EyeReaderState> state)
      : state_(std::move(state)) {
    descriptor_.source_group = "eye_angle_runs";
    descriptor_.run_name = "bound-eyes";
    descriptor_.camera_frame_count = 20;
    descriptor_.coordinate_width = descriptor_.coordinate_height = 100;
  }
  const crimson::zarr::EyeGeometryOverlayDescriptor& descriptor() const override {
    return descriptor_;
  }
  AccessMetrics accessMetrics() const override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    AccessMetrics metrics;
    metrics.payload_read_calls = state_->reads.size();
    metrics.logical_payload_bytes_read = state_->reads.size() * 64;
    return metrics;
  }
  crimson::zarr::EyeGeometryOverlayResolution resolveCameraFrame(
      int64_t frame, int, int) const override {
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->reads.push_back(frame);
    }
    if (frame == 4 && state_->gate) state_->gate->block();
    if (frame == 6) throw std::runtime_error("fixture eye read failure");
    crimson::zarr::EyeGeometryOverlayResolution result;
    result.camera_frame = frame == 8 ? 9 : frame;
    result.status = crimson::zarr::EyeGeometryOverlayStatus::Mapped;
    crimson::zarr::EyeGeometryOverlayDetection eye;
    eye.instance_key = 7;
    eye.instance_key_valid = true;
    result.detections.push_back(eye);
    return result;
  }
 private:
  crimson::zarr::EyeGeometryOverlayDescriptor descriptor_;
  std::shared_ptr<EyeReaderState> state_;
};

struct MaskReaderState {
  std::mutex mutex;
  std::vector<int64_t> reads;
};

class MaskReader : public crimson::zarr::SubjectMaskOverlayRepository {
 public:
  explicit MaskReader(std::shared_ptr<MaskReaderState> state)
      : state_(std::move(state)) {
    descriptor_.source_group = "refined_subject_masks_runs";
    descriptor_.run_name = "latency-mask-fixture";
    descriptor_.camera_frame_count = 80;
    descriptor_.row_count = 80;
    descriptor_.mask_width = 2;
    descriptor_.mask_height = 2;
    descriptor_.storage_chunk_rows = 8;
    descriptor_.component_labels = {"subject_body"};
  }
  const crimson::zarr::SubjectMaskOverlayDescriptor& descriptor() const override {
    return descriptor_;
  }
  crimson::zarr::SubjectMaskOverlayResolution resolveCameraFrame(
      int64_t frame, int, int) const override {
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->reads.push_back(frame);
    }
    std::this_thread::sleep_for(
        frame == 3 || frame == 13 || frame == 23 ? 40ms : 2ms);
    if (frame == 31) throw std::runtime_error("fixture mask read failure");
    crimson::zarr::SubjectMaskOverlayResolution result;
    result.camera_frame = frame == 32 ? 33 : frame;
    if (frame == 30) {
      result.status = crimson::zarr::SubjectMaskOverlayStatus::Missing;
      return result;
    }
    result.status = crimson::zarr::SubjectMaskOverlayStatus::Mapped;
    crimson::zarr::SubjectMaskOverlayDetection detection;
    crimson::zarr::SubjectMaskOverlayComponent component;
    component.label = "subject_body";
    component.present = true;
    component.mask_width = component.mask_height = 2;
    component.mask = std::make_shared<const std::vector<uint8_t>>(
        std::initializer_list<uint8_t>{255, 0, 0, 255});
    detection.components.push_back(std::move(component));
    result.detections.push_back(std::move(detection));
    return result;
  }
 private:
  crimson::zarr::SubjectMaskOverlayDescriptor descriptor_;
  std::shared_ptr<MaskReaderState> state_;
};

struct ContourReaderState {
  std::mutex mutex;
  std::vector<int64_t> reads;
  std::shared_ptr<Gate> gate;
};
class ContourReader : public crimson::zarr::SubjectMaskOverlayRepository {
 public:
  explicit ContourReader(std::shared_ptr<ContourReaderState> state)
      : state_(std::move(state)) {
    descriptor_.source_group = "subject_mask_cache_runs";
    descriptor_.run_name = "contour-fixture";
    descriptor_.camera_frame_count = 80;
    descriptor_.row_count = 80;
    descriptor_.mask_width = descriptor_.mask_height = 2;
    descriptor_.component_labels = {"subject_body"};
    descriptor_.contour_only = true;
  }
  const crimson::zarr::SubjectMaskOverlayDescriptor& descriptor() const override {
    return descriptor_;
  }
  crimson::zarr::SubjectMaskOverlayResolution resolveCameraFrame(
      int64_t frame, int, int) const override {
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->reads.push_back(frame);
    }
    if (frame == 4 && state_->gate) state_->gate->block();
    if (frame == 6) throw std::runtime_error("fixture contour read failure");
    crimson::zarr::SubjectMaskOverlayResolution result;
    result.camera_frame = frame;
    result.status = crimson::zarr::SubjectMaskOverlayStatus::Mapped;
    crimson::zarr::SubjectMaskOverlayDetection detection;
    detection.instance_key = 0;
    detection.source_crop_row_id = -1;
    crimson::zarr::SubjectMaskOverlayComponent component;
    component.label = "subject_body";
    component.channel_index = 0;
    component.contour = {{10.0, 20.0}, {20.0, 20.0}, {20.0, 30.0}};
    detection.components.push_back(std::move(component));
    result.detections.push_back(std::move(detection));
    return result;
  }
 private:
  crimson::zarr::SubjectMaskOverlayDescriptor descriptor_;
  std::shared_ptr<ContourReaderState> state_;
};
crimson::gui::CanonicalOverlayOpenRequest request(std::string name) {
  crimson::gui::CanonicalOverlayOpenRequest result;
  result.archive_path = std::move(name);
  result.recording_id = result.archive_path;
  result.frame_count = 20;
  result.source_width = result.source_height = 100;
  return result;
}
crimson::gui::CanonicalOverlayRepositories opened(const std::string& name,
                                                std::shared_ptr<Gate> gate = {}) {
  crimson::gui::CanonicalOverlayRepositories result;
  result.selection.archive_identity = name;
  result.selection.frame_count = 20;
  result.keypoints = std::make_unique<Reader>(name, std::move(gate));
  result.shape_error = "fixture shape unavailable";
  return result;
}
crimson::gui::CanonicalOverlayRepositories openedMasks(
    const std::string& name, const std::shared_ptr<MaskReaderState>& state) {
  crimson::gui::CanonicalOverlayRepositories result;
  result.selection.archive_identity = name;
  result.selection.frame_count = 80;
  result.masks = std::make_unique<MaskReader>(state);
  return result;
}
crimson::gui::CanonicalOverlayRepositories openedMasksAndContours(
    const std::string& name, const std::shared_ptr<MaskReaderState>& masks,
    const std::shared_ptr<ContourReaderState>& contours) {
  auto result = openedMasks(name, masks);
  result.mask_contours = std::make_unique<ContourReader>(contours);
  return result;
}
bool waitFrame(crimson::gui::CanonicalOverlaySession& session, int64_t frame) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (session.snapshot(frame).keypoints.frame) return true;
    std::this_thread::sleep_for(1ms);
  }
  return false;
}
bool waitMaskState(crimson::gui::CanonicalOverlaySession& session,
                   int64_t frame, CanonicalOverlayState state) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (session.snapshot(frame).masks.state == state) return true;
    std::this_thread::sleep_for(1ms);
  }
  return false;
}
bool waitContourState(crimson::gui::CanonicalOverlaySession& session,
                      int64_t frame, CanonicalOverlayState state) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (session.snapshot(frame).mask_contours.state == state) return true;
    std::this_thread::sleep_for(1ms);
  }
  return false;
}
bool testSnapshotsAndFailures() {
  auto scheduler = std::make_shared<crimson::data::DataAccessScheduler>(32, 3, 1, 1);
  crimson::gui::CanonicalOverlaySession session(scheduler, [](const auto& r) { return opened(r.archive_path); });
  auto missing_identity = request("missing-identity");
  missing_identity.recording_id.clear();
  CHECK(!session.beginOpen(missing_identity));
  CHECK(session.beginOpen(request("one")));
  CHECK(session.waitUntilOpen(3s));
  auto pending = session.snapshot(2);
  CHECK(pending.keypoints.state == CanonicalOverlayState::Pending);
  CHECK(pending.masks.state == CanonicalOverlayState::Unavailable);
  CHECK(pending.shapes.state == CanonicalOverlayState::Failed);
  CHECK(session.requestFrame(2, true, false, false));
  CHECK(waitFrame(session, 2));
  auto frame = session.snapshot(2);
  CHECK(frame.keypoints.state == CanonicalOverlayState::Ready);
  CHECK(frame.keypoints.frame->detections[0].instance_key_valid);
  CHECK(frame.keypoints.frame->detections[0].instance_key == 0);
  CHECK(session.requestFrame(10, true, false, false, true));
  CHECK(waitFrame(session, 10));
  CHECK(session.snapshot(10).keypoints.state == CanonicalOverlayState::Empty);
  CHECK(session.requestFrame(7, true, false, false, true));
  CHECK(waitFrame(session, 7));
  CHECK(session.snapshot(7).keypoints.state == CanonicalOverlayState::Failed);
  CHECK(!session.snapshot(7).keypoints.error.empty());
  CHECK(session.requestFrame(8, true, false, false, true));
  CHECK(waitFrame(session, 8));
  CHECK(session.snapshot(8).keypoints.state == CanonicalOverlayState::Failed);
  CHECK(!session.snapshot(8).keypoints.error.empty());
  session.shutdown();
  CHECK(!session.beginOpen(request("after-shutdown")));
  return true;
}
bool testSupersededOpen() {
  auto scheduler = std::make_shared<crimson::data::DataAccessScheduler>(32, 3, 1, 1);
  auto gate = std::make_shared<Gate>();
  crimson::gui::CanonicalOverlaySession session(scheduler, [gate](const auto& r) {
    if (r.archive_path == "old") gate->block();
    return opened(r.archive_path);
  });
  CHECK(session.beginOpen(request("old")));
  const bool started = gate->wait();
  const auto start = std::chrono::steady_clock::now();
  const bool accepted = session.beginOpen(request("new"));
  const auto elapsed = std::chrono::steady_clock::now() - start;
  auto snapshot = session.snapshot(0);
  gate->release();
  CHECK(started && accepted && elapsed < 500ms);
  CHECK(snapshot.state == CanonicalOverlayState::Opening);
  CHECK(!snapshot.selection);
  CHECK(session.waitUntilOpen(3s));
  CHECK(session.snapshot(0).selection->archive_identity == "new");
  return true;
}
bool testNonblockingCloseAndSourceIsolation() {
  auto scheduler = std::make_shared<crimson::data::DataAccessScheduler>(32, 3, 1, 1);
  auto gate = std::make_shared<Gate>();
  crimson::gui::CanonicalOverlaySession first(scheduler, [gate](const auto& r) { return opened(r.archive_path, gate); });
  crimson::gui::CanonicalOverlaySession second(scheduler, [](const auto& r) { return opened(r.archive_path); });
  CHECK(first.beginOpen(request("same")));
  CHECK(second.beginOpen(request("same")));
  CHECK(first.waitUntilOpen(3s) && second.waitUntilOpen(3s));
  CHECK(first.requestFrame(4, true, false, false));
  const bool started = gate->wait();
  const auto start = std::chrono::steady_clock::now();
  first.close();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const bool other_accepted = second.requestFrame(4, true, false, false);
  const bool other_ready = waitFrame(second, 4);
  auto closed = first.snapshot(4);
  gate->release();
  CHECK(started && elapsed < 500ms);
  CHECK(closed.state == CanonicalOverlayState::Closed && !closed.keypoints.frame);
  CHECK(other_accepted && other_ready);
  first.shutdown();
  second.shutdown();
  return true;
}

bool testMaskReadAheadAtRenderRate(int render_hz) {
  auto scheduler =
      std::make_shared<crimson::data::DataAccessScheduler>(64, 4, 1, 1);
  auto reader_state = std::make_shared<MaskReaderState>();
  crimson::gui::CanonicalOverlaySession session(
      scheduler, [reader_state](const auto& r) {
        return openedMasks(r.archive_path, reader_state);
      });
  auto open_request = request("mask-latency-" + std::to_string(render_hz));
  open_request.frame_count = 80;
  CHECK(session.beginOpen(open_request));
  CHECK(session.waitUntilOpen(3s));
  crimson::gui::CanonicalOverlayPlaybackDemand paused;
  paused.read_ahead = true;
  paused.direction = crimson::gui::CanonicalOverlayPlaybackDirection::Paused;
  paused.source_frames_per_second = 30.0;
  paused.playback_rate = 1.0;
  CHECK(session.requestFrame(0, false, true, false, true, paused));
  const auto prewarm_deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < prewarm_deadline &&
         session.snapshot(0).mask_buffer_metrics.contiguous_ready_ahead < 8) {
    std::this_thread::sleep_for(1ms);
  }
  auto warmed = session.snapshot(0);
  CHECK(warmed.masks.state == CanonicalOverlayState::Ready);
  CHECK(warmed.mask_buffer_metrics.effective_lookahead_frames >= 8);
  CHECK(warmed.mask_buffer_metrics.contiguous_ready_ahead >= 8);

  auto playing = paused;
  playing.direction =
      crimson::gui::CanonicalOverlayPlaybackDirection::Forward;
  const auto started = std::chrono::steady_clock::now();
  int64_t last_frame = -1;
  for (int tick = 0; last_frame < 29; ++tick) {
    const int64_t frame = std::min<int64_t>(29, (tick * 30) / render_hz);
    CHECK(session.requestFrame(frame, false, true, false, false, playing));
    if (frame != last_frame) {
      const auto first_presentation = session.snapshot(frame);
      CHECK(first_presentation.masks.state == CanonicalOverlayState::Ready);
      CHECK(first_presentation.masks.frame != nullptr);
      CHECK(first_presentation.masks.frame->camera_frame == frame);
      last_frame = frame;
    }
    std::this_thread::sleep_until(
        started + std::chrono::microseconds(
                      static_cast<int64_t>((tick + 1) * 1000000LL /
                                           render_hz)));
  }
  const auto final = session.snapshot(29);
  CHECK(final.mask_buffer_metrics.cached_payload_bytes <=
        final.mask_buffer_metrics.maximum_cached_payload_bytes);
  CHECK(final.mask_buffer_metrics.current_frame_ready);
  {
    std::lock_guard<std::mutex> lock(reader_state->mutex);
    for (const int64_t boundary : {7, 8, 15, 16, 23, 24}) {
      CHECK(std::find(reader_state->reads.begin(), reader_state->reads.end(),
                      boundary) != reader_state->reads.end());
    }
  }

  CHECK(session.requestFrame(30, false, true, false, false, playing));
  CHECK(waitMaskState(session, 30, CanonicalOverlayState::Empty));
  CHECK(session.requestFrame(31, false, true, false, false, playing));
  CHECK(waitMaskState(session, 31, CanonicalOverlayState::Failed));
  CHECK(session.requestFrame(32, false, true, false, false, playing));
  CHECK(waitMaskState(session, 32, CanonicalOverlayState::Failed));
  CHECK(session.snapshot(32).masks.error.find("different") !=
        std::string::npos);
  session.shutdown();
  scheduler->shutdown();
  return true;
}
bool testIndependentContourDemandAndFailure() {
  auto scheduler = std::make_shared<crimson::data::DataAccessScheduler>(32, 3, 1, 1);
  auto mask_state = std::make_shared<MaskReaderState>();
  auto contour_state = std::make_shared<ContourReaderState>();
  contour_state->gate = std::make_shared<Gate>();
  crimson::gui::CanonicalOverlaySession session(
      scheduler, [mask_state, contour_state](const auto& r) {
        return openedMasksAndContours(r.archive_path, mask_state, contour_state);
      });
  auto open_request = request("independent-contours");
  open_request.frame_count = 80;
  CHECK(session.beginOpen(open_request));
  CHECK(session.waitUntilOpen(3s));

  // A fill-only request must not schedule even one contour payload read.
  CHECK(session.requestFrame(2, false, true, false, true));
  CHECK(waitMaskState(session, 2, CanonicalOverlayState::Ready));
  {
    std::lock_guard<std::mutex> lock(contour_state->mutex);
    CHECK(contour_state->reads.empty());
  }
  CHECK(!session.snapshot(2).mask_contours.frame);

  // A blocked contour read must not hold a ready fill frame hostage.
  CHECK(session.requestFrame(4, false, true, false, true, {}, true));
  const bool contour_started = contour_state->gate->wait();
  const bool fill_ready_while_contour_blocked =
      waitMaskState(session, 4, CanonicalOverlayState::Ready);
  const auto blocked = session.snapshot(4);
  contour_state->gate->release();
  CHECK(contour_started && fill_ready_while_contour_blocked);
  CHECK(blocked.masks.frame && blocked.masks.frame->camera_frame == 4);
  CHECK(!blocked.mask_contours.frame);
  CHECK(waitContourState(session, 4, CanonicalOverlayState::Ready));

  CHECK(session.requestFrame(5, false, true, false, true, {}, true));
  const auto after_seek = session.snapshot(5);
  CHECK(!after_seek.mask_contours.frame ||
        after_seek.mask_contours.frame->camera_frame == 5);
  CHECK(waitMaskState(session, 5, CanonicalOverlayState::Ready));
  CHECK(waitContourState(session, 5, CanonicalOverlayState::Ready));
  CHECK(session.snapshot(5).mask_contours.frame->camera_frame == 5);

  CHECK(session.requestFrame(6, false, true, false, true, {}, true));
  CHECK(waitMaskState(session, 6, CanonicalOverlayState::Ready));
  CHECK(waitContourState(session, 6, CanonicalOverlayState::Failed));
  const auto failed = session.snapshot(6);
  CHECK(failed.masks.frame && failed.masks.frame->camera_frame == 6);
  if (failed.mask_contours.frame) {
    CHECK(failed.mask_contours.frame->camera_frame == 6);
    CHECK(failed.mask_contours.frame->status ==
          crimson::zarr::SubjectMaskOverlayStatus::ReadFailed);
    CHECK(failed.mask_contours.frame->detections.empty());
  }
  CHECK(!failed.mask_contours.error.empty());

  size_t contour_reads_before_fill_only = 0;
  {
    std::lock_guard<std::mutex> lock(contour_state->mutex);
    contour_reads_before_fill_only = contour_state->reads.size();
  }
  CHECK(session.requestFrame(7, false, true, false, true));
  CHECK(waitMaskState(session, 7, CanonicalOverlayState::Ready));
  {
    std::lock_guard<std::mutex> lock(contour_state->mutex);
    CHECK(contour_state->reads.size() == contour_reads_before_fill_only);
  }
  session.shutdown();
  scheduler->shutdown();
  return true;
}
bool testIndependentEyeDemandAndSeek() {
  auto scheduler = std::make_shared<crimson::data::DataAccessScheduler>(32, 3, 1, 1);
  auto eye_state = std::make_shared<EyeReaderState>();
  eye_state->gate = std::make_shared<Gate>();
  crimson::gui::CanonicalOverlaySession session(
      scheduler, [eye_state](const auto& r) {
        auto result = opened(r.archive_path);
        result.eyes = std::make_unique<EyeReader>(eye_state);
        return result;
      });
  CHECK(session.beginOpen(request("eyes-only-demand")));
  CHECK(session.waitUntilOpen(3s));
  CHECK(session.requestFrame(2, true, false, false));
  CHECK(waitFrame(session, 2));
  CHECK(session.snapshot(2).eye_metrics.payload_read_calls == 0);
  { std::lock_guard<std::mutex> lock(eye_state->mutex);
    CHECK(eye_state->reads.empty()); }
  CHECK(session.requestFrame(4, false, false, false, true, {}, false, true));
  const bool eye_read_started = eye_state->gate->wait();
  const bool requested_after_seek = session.requestFrame(
      5, true, false, false, true, {}, false, true);
  const bool keypoint_ready_while_eye_blocked =
      requested_after_seek && waitFrame(session, 5);
  eye_state->gate->release();
  CHECK(eye_read_started);
  CHECK(requested_after_seek);
  CHECK(keypoint_ready_while_eye_blocked);
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline &&
         !session.snapshot(5).eyes.frame) std::this_thread::sleep_for(1ms);
  CHECK(session.snapshot(5).eyes.state == CanonicalOverlayState::Ready);
  CHECK(session.snapshot(5).eye_metrics.payload_read_calls > 0);
  CHECK(!session.snapshot(4).eyes.frame);
  CHECK(session.snapshot(5).keypoints.state == CanonicalOverlayState::Ready);
  CHECK(session.requestFrame(6, false, false, false, true, {}, false, true));
  const auto failed_deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < failed_deadline &&
         session.snapshot(6).eyes.state == CanonicalOverlayState::Pending)
    std::this_thread::sleep_for(1ms);
  CHECK(session.snapshot(6).eyes.state == CanonicalOverlayState::Failed);
  CHECK(!session.snapshot(6).eyes.error.empty());
  CHECK(session.requestFrame(7, false, false, false, true, {}, false, true));
  const auto ready_deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < ready_deadline &&
         !session.snapshot(7).eyes.frame) std::this_thread::sleep_for(1ms);
  CHECK(session.snapshot(7).eyes.state == CanonicalOverlayState::Ready);
  CHECK(session.requestFrame(8, false, false, false, true, {}, false, true));
  const auto mismatch_deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < mismatch_deadline &&
         session.snapshot(8).eyes.state == CanonicalOverlayState::Pending)
    std::this_thread::sleep_for(1ms);
  CHECK(session.snapshot(8).eyes.state == CanonicalOverlayState::Failed);
  CHECK(session.requestFrame(9, false, false, false, true));
  CHECK(!session.snapshot(7).eyes.frame);
  session.shutdown();
  scheduler->shutdown();
  return true;
}
int main() {
  if (!testSnapshotsAndFailures() || !testSupersededOpen() ||
      !testNonblockingCloseAndSourceIsolation() ||
      !testMaskReadAheadAtRenderRate(30) ||
      !testMaskReadAheadAtRenderRate(60) ||
      !testIndependentContourDemandAndFailure() ||
      !testIndependentEyeDemandAndSeek()) return 1;
  std::cout << "canonical_overlay_session_tests passed\n";
}
