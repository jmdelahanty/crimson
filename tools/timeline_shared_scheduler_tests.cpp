#include "eye_angle_timeline_buffer.h"
#include "swim_bout_timeline_buffer.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>

namespace {
using namespace crimson::data;
using namespace crimson::timeline;
using namespace std::chrono_literals;
#define CHECK(value) do { if (!(value)) { std::cerr << __LINE__ << ": " #value "\n"; return false; } } while (false)

struct Gate {
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool released = false;
  void block() {
    std::unique_lock<std::mutex> lock(mutex);
    entered = true;
    condition.notify_all();
    condition.wait(lock, [&] { return released; });
  }
  bool wait() {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(lock, 2s, [&] { return entered; });
  }
  void release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    condition.notify_all();
  }
};
// Declared after buffers: even a failed assertion releases blocked I/O before
// buffer destructors drain their scheduler sources.
struct ReleaseOnExit {
  std::shared_ptr<Gate> gate;
  ~ReleaseOnExit() { gate->release(); }
};

class Eyes final : public EyeAngleTimelineRepository {
 public:
  explicit Eyes(std::shared_ptr<Gate> gate = {}, bool throws = false)
      : gate_(std::move(gate)), throws_(throws) {
    descriptor_.frame_count = 240;
    descriptor_.source_group = "analysis/eye_angle_runs";
    descriptor_.run_name = "same-run";
    descriptor_.default_representation = "eye_frame";
    EyeAngleTimelineRepresentation representation;
    representation.key = "eye_frame";
    representation.fields.push_back({"left", "left", "Left", "deg"});
    descriptor_.representations.push_back(std::move(representation));
  }
  const EyeAngleTimelineDescriptor& descriptor() const override { return descriptor_; }
  EyeAngleTimelineWindow resolveWindow(const EyeAngleTimelineRequest& request) const override {
    if (gate_ && calls_.fetch_add(1) == 0) gate_->block();
    if (throws_) throw std::runtime_error("fixture eye read failed");
    EyeAngleTimelineWindow result;
    result.request = request;
    result.status = EyeAngleTimelineStatus::Mapped;
    result.source_row_count = request.last_frame - request.first_frame + 1;
    EyeAngleTimelineTrace trace;
    trace.field = descriptor_.representations.front().fields.front();
    trace.frames = {request.first_frame};
    trace.times_seconds = {request.first_frame / 30.0};
    trace.values = {12.5};
    result.traces.push_back(std::move(trace));
    result.published_point_count = 1;
    return result;
  }
 private:
  EyeAngleTimelineDescriptor descriptor_;
  std::shared_ptr<Gate> gate_;
  bool throws_;
  mutable std::atomic<unsigned> calls_{0};
};

class Bouts final : public SwimBoutTimelineRepository {
 public:
  explicit Bouts(bool throws = false) : throws_(throws) {
    descriptor_.frame_count = 240;
    descriptor_.default_candidate = "candidate";
    SwimBoutCandidateDescriptor candidate;
    candidate.key = "candidate";
    candidate.source_group = "analysis/swim_bout_runs";
    candidate.run_name = "same-run";
    descriptor_.candidates.push_back(std::move(candidate));
  }
  const SwimBoutTimelineDescriptor& descriptor() const override { return descriptor_; }
  SwimBoutTimelineWindow resolveWindow(const SwimBoutTimelineRequest& request) const override {
    if (throws_) throw std::runtime_error("fixture bout read failed");
    SwimBoutTimelineWindow result;
    result.request = request;
    result.status = SwimBoutTimelineStatus::Mapped;
    return result; // A valid interval-free window is cacheable, not pending.
  }
 private:
  SwimBoutTimelineDescriptor descriptor_;
  bool throws_;
};

bool prioritySeekAndEmpty() {
  auto scheduler = std::make_shared<DataAccessScheduler>(16, 2, 1, 1);
  auto gate = std::make_shared<Gate>();
  EyeAngleTimelineBuffer eyes(scheduler, "archive-a");
  SwimBoutTimelineBuffer bouts(scheduler, "archive-a");
  ReleaseOnExit cleanup{gate};
  CHECK(eyes.open(std::make_unique<Eyes>(gate), 40, 20, 9, 2));
  CHECK(bouts.open(std::make_unique<Bouts>(), 40, 20, 9, 2));
  CHECK(eyes.requestFrame(5, {}, 30));
  CHECK(gate->wait());
  CHECK(bouts.requestFrame(5, {}, 30, false));
  auto current = std::make_shared<std::promise<bool>>();
  auto finished = current->get_future();
  CHECK(scheduler->submit({{"archive-a", "video", "camera"}, {5, 5},
          FieldSelection::All(), RequestPriority::CurrentFrame,
          AccessPattern::Paused, 1},
      [current](const ScheduledDataRequest&) {
        current->set_value(true);
        return DataResultStatus::Ready;
      }).accepted());
  CHECK(finished.wait_for(2s) == std::future_status::ready);
  CHECK(finished.get());
  CHECK(!bouts.window(5, {}, false)); // Reserved worker cannot run background work.
  CHECK(eyes.requestFrame(165, {}, 30, true));
  gate->release();
  CHECK(eyes.waitForFrame(165, {}, 30, 2s));
  CHECK(bouts.waitForFrame(5, {}, 30, false, 2s));
  scheduler->waitUntilIdle();
  CHECK(!eyes.window(5, {}));
  CHECK(eyes.window(165, {})->ready());
  CHECK(eyes.metrics().discarded_results == 1);
  CHECK(bouts.window(5, {}, false)->status == SwimBoutTimelineStatus::Mapped);
  CHECK(bouts.window(5, {}, false)->intervals.empty());
  CHECK(bouts.metrics().resolved_windows == 1);
  const auto before = scheduler->metrics().queue.submissions;
  for (int i = 0; i < 100; ++i) {
    CHECK(eyes.window(165, {}));
    CHECK(bouts.window(5, {}, false));
  }
  CHECK(scheduler->metrics().queue.submissions == before);
  return true;
}

bool sourceIsolationAndReopen() {
  auto scheduler = std::make_shared<DataAccessScheduler>(16, 3, 1, 1);
  auto gate = std::make_shared<Gate>();
  EyeAngleTimelineBuffer first(scheduler, "archive-a");
  EyeAngleTimelineBuffer second(scheduler, "archive-b");
  ReleaseOnExit cleanup{gate};
  CHECK(first.open(std::make_unique<Eyes>(), 40, 20, 9, 2));
  CHECK(second.open(std::make_unique<Eyes>(gate), 40, 20, 9, 2));
  CHECK(second.requestFrame(5, {}, 30));
  CHECK(gate->wait());
  CHECK(first.requestFrame(5, {}, 30));
  CHECK(first.waitForFrame(5, {}, 30, 2s));
  first.close(); // Must not cancel another archive's identical run and page.
  gate->release();
  CHECK(second.waitForFrame(5, {}, 30, 2s));
  CHECK(second.window(5, {})->ready());
  CHECK(first.open(std::make_unique<Eyes>(), 40, 20, 9, 2));
  CHECK(!first.window(5, {}));
  CHECK(first.requestFrame(5, {}, 30));
  CHECK(first.waitForFrame(5, {}, 30, 2s));
  CHECK(first.window(5, {})->ready());
  return true;
}

bool exceptionsArePublished() {
  auto scheduler = std::make_shared<DataAccessScheduler>(16, 2);
  EyeAngleTimelineBuffer eyes(scheduler, "archive-a");
  SwimBoutTimelineBuffer bouts(scheduler, "archive-a");
  CHECK(eyes.open(std::make_unique<Eyes>(nullptr, true), 40, 20, 9, 2));
  CHECK(bouts.open(std::make_unique<Bouts>(true), 40, 20, 9, 2));
  CHECK(eyes.requestFrame(5, {}, 30));
  CHECK(bouts.requestFrame(5, {}, 30, true));
  CHECK(eyes.waitForFrame(5, {}, 30, 2s));
  CHECK(bouts.waitForFrame(5, {}, 30, true, 2s));
  CHECK(eyes.window(5, {})->status == EyeAngleTimelineStatus::ReadFailed);
  CHECK(bouts.window(5, {}, true)->status == SwimBoutTimelineStatus::ReadFailed);
  CHECK(eyes.metrics().failed_windows == 1);
  CHECK(bouts.metrics().failed_windows == 1);
  return true;
}
} // namespace

int main() {
  if (!prioritySeekAndEmpty() || !sourceIsolationAndReopen() ||
      !exceptionsArePublished()) return 1;
  std::cout << "timeline_shared_scheduler_tests PASS\n";
}
