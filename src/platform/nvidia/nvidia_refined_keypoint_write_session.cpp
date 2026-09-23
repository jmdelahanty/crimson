#include "platform/nvidia/nvidia_refined_keypoint_write_session.h"

#include "async_single_flight_job.h"

#include <sstream>
#include <utility>

namespace crimson::platform::nvidia {

namespace {

std::string failurePrefix(CropKeypointEditorActionType action_type) {
  switch (action_type) {
  case CropKeypointEditorActionType::Save:
    return "Keypoint edit failed: ";
  case CropKeypointEditorActionType::MarkNoKeypoints:
    return "Mark no keypoints failed: ";
  case CropKeypointEditorActionType::MarkDetectionIssue:
    return "Mark detection issue failed: ";
  case CropKeypointEditorActionType::Reset:
  case CropKeypointEditorActionType::None:
  default:
    return "Keypoint write failed: ";
  }
}

std::string startStatus(CropKeypointEditorActionType action_type,
                        const RefinedKeypointSelection &selection) {
  std::ostringstream status;
  switch (action_type) {
  case CropKeypointEditorActionType::Save:
    status << "Saving keypoint edit";
    break;
  case CropKeypointEditorActionType::MarkNoKeypoints:
    status << "Marking fish_present_no_keypoints";
    break;
  case CropKeypointEditorActionType::MarkDetectionIssue:
    status << "Marking detection_issue";
    break;
  case CropKeypointEditorActionType::Reset:
  case CropKeypointEditorActionType::None:
  default:
    status << "Writing keypoint update";
    break;
  }
  status << ": roi=" << selection.roi_index << " ...";
  return status.str();
}

std::string successStatus(CropKeypointEditorActionType action_type,
                          const RefinedKeypointSelection &selection,
                          const RefinedKeypointEditResult &edit_result) {
  std::ostringstream status;
  if (action_type == CropKeypointEditorActionType::Save) {
    status << (edit_result.changed ? "Keypoint edit saved"
                                   : "Keypoint edit was a no-op");
  } else if (action_type == CropKeypointEditorActionType::MarkNoKeypoints) {
    status << "Marked fish_present_no_keypoints";
  } else if (action_type == CropKeypointEditorActionType::MarkDetectionIssue) {
    status << "Marked detection_issue";
  } else {
    status << "Keypoint write";
  }
  status << ": roi=" << selection.roi_index;
  if (edit_result.summary_updated) {
    status << " summary=updated";
  }
  if (edit_result.stale_eye_mask_runs > 0) {
    status << " stale_eye_masks=" << edit_result.stale_eye_mask_runs;
  }
  return status.str();
}

} // namespace

struct RefinedKeypointWriteSession::Impl {
  explicit Impl(RefinedKeypointWriteWorker requested_worker)
      : worker(std::move(requested_worker)) {}

  RefinedKeypointWriteWorker worker;
  tasks::AsyncSingleFlightJob job;
  std::optional<RefinedKeypointWriteRequest> request;
  std::shared_ptr<RefinedKeypointWriteWorkerResult> result;
};

bool RefinedKeypointWriteStartOutcome::accepted() const {
  return status == RefinedKeypointWriteStartStatus::Accepted;
}

RefinedKeypointWriteSession::RefinedKeypointWriteSession(
    RefinedKeypointWriteWorker worker)
    : impl_(std::make_unique<Impl>(std::move(worker))) {}

RefinedKeypointWriteSession::~RefinedKeypointWriteSession() { close(); }

RefinedKeypointWriteStartOutcome
RefinedKeypointWriteSession::start(RefinedKeypointWriteRequest request) {
  RefinedKeypointWriteStartOutcome outcome;
  if (request.action.type == CropKeypointEditorActionType::None) {
    return outcome;
  }
  if (!request.selection.has_value()) {
    outcome.status = RefinedKeypointWriteStartStatus::RejectedInvalid;
    outcome.status_message = "Keypoint write failed: No keypoint selection.";
    return outcome;
  }
  if (impl_->job.active()) {
    outcome.status = RefinedKeypointWriteStartStatus::RejectedBusy;
    outcome.status_message = "Keypoint write already in progress.";
    return outcome;
  }
  if (!impl_->worker) {
    outcome.status = RefinedKeypointWriteStartStatus::RejectedUnavailable;
    outcome.status_message = "Keypoint write failed: No worker is available.";
    return outcome;
  }

  auto result = std::make_shared<RefinedKeypointWriteWorkerResult>();
  const RefinedKeypointWriteWorker worker = impl_->worker;
  const auto job_outcome =
      impl_->job.start("refined_keypoint_write", [request, worker, result] {
        *result = worker(request);
      });
  if (!job_outcome.accepted()) {
    outcome.status =
        job_outcome.status == tasks::SingleFlightJobStartStatus::RejectedBusy
            ? RefinedKeypointWriteStartStatus::RejectedBusy
            : RefinedKeypointWriteStartStatus::RejectedUnavailable;
    outcome.status_message =
        outcome.status == RefinedKeypointWriteStartStatus::RejectedBusy
            ? "Keypoint write already in progress."
            : "Keypoint write failed: " + job_outcome.reason;
    return outcome;
  }

  impl_->request = std::move(request);
  impl_->result = std::move(result);
  outcome.status = RefinedKeypointWriteStartStatus::Accepted;
  outcome.status_message =
      startStatus(impl_->request->action.type, *impl_->request->selection);
  return outcome;
}

std::optional<RefinedKeypointWriteCompletion>
RefinedKeypointWriteSession::takeReady(uint64_t active_session_generation,
                                       const std::string &active_archive_path) {
  const auto job_completion = impl_->job.takeReady();
  if (!job_completion.has_value()) {
    return std::nullopt;
  }

  RefinedKeypointWriteCompletion completion;
  if (impl_->request.has_value()) {
    completion.request = std::move(*impl_->request);
  }
  if (impl_->result != nullptr) {
    completion.worker_result = std::move(*impl_->result);
  }
  completion.queue_wait_ms = job_completion->queue_wait_ms;
  completion.service_ms = job_completion->service_ms;
  impl_->request.reset();
  impl_->result.reset();

  if (!job_completion->succeeded()) {
    completion.worker_result.ok = false;
    completion.worker_result.error =
        "Worker exception: " + job_completion->error;
  }
  if (!completion.worker_result.ok) {
    completion.status_message = failurePrefix(completion.request.action.type) +
                                completion.worker_result.error;
    return completion;
  }

  completion.status_message = successStatus(
      completion.request.action.type, *completion.request.selection,
      completion.worker_result.edit_result);
  completion.stale_session =
      completion.request.session_generation != active_session_generation ||
      completion.request.archive_path != active_archive_path;
  if (completion.stale_session) {
    completion.status_message += " (active Zarr changed; skipped reload)";
  }
  return completion;
}

bool RefinedKeypointWriteSession::active() const { return impl_->job.active(); }

void RefinedKeypointWriteSession::close() {
  impl_->job.close();
  impl_->request.reset();
  impl_->result.reset();
}

} // namespace crimson::platform::nvidia
