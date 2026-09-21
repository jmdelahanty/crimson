#include "gui/canonical_overlay_session.h"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

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
bool waitFrame(crimson::gui::CanonicalOverlaySession& session, int64_t frame) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (session.snapshot(frame).keypoints.frame) return true;
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
int main() {
  if (!testSnapshotsAndFailures() || !testSupersededOpen() ||
      !testNonblockingCloseAndSourceIsolation()) return 1;
  std::cout << "canonical_overlay_session_tests passed\n";
}
