#include "platform/nvidia/nvidia_refined_keypoint_write_session.h"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__           \
                << ": " #condition << '\n';                                    \
      return false;                                                            \
    }                                                                          \
  } while (false)

template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout =
                                        std::chrono::milliseconds(2000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return predicate();
}

crimson::platform::nvidia::RefinedKeypointWriteRequest
request(CropKeypointEditorActionType action_type, uint64_t generation = 7,
        std::string archive_path = "archive.zarr") {
  crimson::platform::nvidia::RefinedKeypointWriteRequest request;
  request.session_generation = generation;
  request.archive_path = std::move(archive_path);
  request.action.type = action_type;
  RefinedKeypointSelection selection;
  selection.roi_index = 12;
  request.selection = std::move(selection);
  request.reset_crop_editor = true;
  request.reset_full_frame_editor = true;
  return request;
}

bool testValidationBusySuccessAndStaleCompletion() {
  using namespace crimson::platform::nvidia;
  std::promise<void> release;
  std::shared_future<void> release_future = release.get_future().share();
  std::atomic<int> calls{0};
  RefinedKeypointWriteSession session([&](const RefinedKeypointWriteRequest &) {
    if (calls.fetch_add(1) == 0) {
      release_future.wait();
    }
    RefinedKeypointWriteWorkerResult result;
    result.ok = true;
    result.edit_result.changed = true;
    result.edit_result.summary_updated = true;
    result.edit_result.stale_eye_mask_runs = 2;
    return result;
  });

  auto ignored = request(CropKeypointEditorActionType::None);
  CHECK(session.start(std::move(ignored)).status ==
        RefinedKeypointWriteStartStatus::Ignored);
  auto invalid = request(CropKeypointEditorActionType::Save);
  invalid.selection.reset();
  const auto invalid_outcome = session.start(std::move(invalid));
  CHECK(invalid_outcome.status ==
        RefinedKeypointWriteStartStatus::RejectedInvalid);
  CHECK(invalid_outcome.status_message ==
        "Keypoint write failed: No keypoint selection.");

  const auto accepted =
      session.start(request(CropKeypointEditorActionType::Save));
  CHECK(accepted.accepted());
  CHECK(accepted.status_message == "Saving keypoint edit: roi=12 ...");
  CHECK(waitUntil([&] { return calls.load() == 1; }));
  const auto busy = session.start(request(CropKeypointEditorActionType::Save));
  CHECK(busy.status == RefinedKeypointWriteStartStatus::RejectedBusy);
  CHECK(busy.status_message == "Keypoint write already in progress.");

  release.set_value();
  std::optional<RefinedKeypointWriteCompletion> completion;
  CHECK(waitUntil([&] {
    completion = session.takeReady(7, "archive.zarr");
    return completion.has_value();
  }));
  CHECK(completion->worker_result.ok);
  CHECK(!completion->stale_session);
  CHECK(completion->request.reset_crop_editor);
  CHECK(completion->request.reset_full_frame_editor);
  CHECK(completion->status_message ==
        "Keypoint edit saved: roi=12 summary=updated stale_eye_masks=2");
  CHECK(completion->queue_wait_ms >= 0.0);
  CHECK(completion->service_ms >= 0.0);

  CHECK(session.start(request(CropKeypointEditorActionType::Save)).accepted());
  completion.reset();
  CHECK(waitUntil([&] {
    completion = session.takeReady(8, "other.zarr");
    return completion.has_value();
  }));
  CHECK(completion->stale_session);
  CHECK(completion->status_message ==
        "Keypoint edit saved: roi=12 summary=updated stale_eye_masks=2 "
        "(active Zarr changed; skipped reload)");
  return true;
}

bool testWorkerFailureAndException() {
  using namespace crimson::platform::nvidia;
  RefinedKeypointWriteSession failed_session(
      [](const RefinedKeypointWriteRequest &) {
        RefinedKeypointWriteWorkerResult result;
        result.error = "write denied";
        return result;
      });
  CHECK(failed_session
            .start(request(CropKeypointEditorActionType::MarkNoKeypoints))
            .accepted());
  std::optional<RefinedKeypointWriteCompletion> completion;
  CHECK(waitUntil([&] {
    completion = failed_session.takeReady(7, "archive.zarr");
    return completion.has_value();
  }));
  CHECK(completion->status_message == "Mark no keypoints failed: write denied");

  RefinedKeypointWriteSession throwing_session(
      [](const RefinedKeypointWriteRequest &)
          -> RefinedKeypointWriteWorkerResult {
        throw std::runtime_error("boom");
      });
  CHECK(throwing_session.start(request(CropKeypointEditorActionType::Save))
            .accepted());
  completion.reset();
  CHECK(waitUntil([&] {
    completion = throwing_session.takeReady(7, "archive.zarr");
    return completion.has_value();
  }));
  CHECK(completion->status_message ==
        "Keypoint edit failed: Worker exception: boom");
  return true;
}

} // namespace

int main() {
  if (!testValidationBusySuccessAndStaleCompletion() ||
      !testWorkerFailureAndException()) {
    return 1;
  }
  std::cout << "nvidia_refined_keypoint_write_session_tests: PASS\n";
  return 0;
}
