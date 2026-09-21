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
int main() {
  if (!testSnapshotsAndFailures() || !testSupersededOpen() ||
      !testNonblockingCloseAndSourceIsolation() ||
      !testMaskReadAheadAtRenderRate(30) ||
      !testMaskReadAheadAtRenderRate(60)) return 1;
  std::cout << "canonical_overlay_session_tests passed\n";
}
