#include "subject_mask_presentation_coordinator.h"

#include "zarr/subject_mask_overlay_scene_adapter.h"

#include <algorithm>

namespace crimson::overlay {
namespace {

ReadOnlyOverlayFrameCandidateStatus
candidateStatus(zarr::SubjectMaskOverlayStatus status) {
  switch (status) {
  case zarr::SubjectMaskOverlayStatus::Mapped:
    return ReadOnlyOverlayFrameCandidateStatus::Mapped;
  case zarr::SubjectMaskOverlayStatus::Missing:
  case zarr::SubjectMaskOverlayStatus::OutOfRange:
    return ReadOnlyOverlayFrameCandidateStatus::Missing;
  case zarr::SubjectMaskOverlayStatus::InvalidDimensions:
  case zarr::SubjectMaskOverlayStatus::ReadFailed:
    return ReadOnlyOverlayFrameCandidateStatus::Failed;
  }
  return ReadOnlyOverlayFrameCandidateStatus::Failed;
}

} // namespace

SubjectMaskPresentationResult SubjectMaskPresentationCoordinator::update(
    const SubjectMaskPresentationInput &input, SubjectMaskOverlayBuffer &buffer,
    const zarr::SubjectMaskOverlayDescriptor &descriptor,
    ReadOnlyOverlayInput &scene, std::string *error_message) {
  SubjectMaskPresentationResult result;
  const auto plan = frame_coordinator_.beginFrame(
      {input.camera_frame, input.demand_enabled, input.layer_enabled,
       input.source_available, input.presentation_discontinuity});
  if (!plan.issue_request) {
    return result;
  }

  ReadOnlyOverlayFrameCandidate candidate;
  candidate.request_accepted = buffer.requestFrame(
      input.camera_frame, input.full_frame_width, input.full_frame_height,
      plan.request_discontinuity, error_message);
  const auto resolution =
      candidate.request_accepted ? buffer.frame(input.camera_frame) : nullptr;
  if (resolution) {
    candidate.camera_frame = resolution->camera_frame;
    candidate.status = candidateStatus(resolution->status);
  }

  const auto decision = frame_coordinator_.finishFrame(plan, candidate);
  result.action = decision.action;
  if (decision.action != ReadOnlyOverlayFrameAction::Present || !resolution) {
    return result;
  }

  result.overlay_ready = zarr::appendSubjectMaskOverlaySceneInput(
      descriptor, *resolution, input.camera_frame, &scene);
  if (!result.overlay_ready) {
    return result;
  }
  result.detection_count = resolution->detections.size();
  for (const auto &detection : resolution->detections) {
    result.component_count += static_cast<size_t>(
        std::count_if(detection.components.begin(), detection.components.end(),
                      [](const auto &component) {
                        return component.present || !component.contour.empty();
                      }));
  }
  return result;
}

void SubjectMaskPresentationCoordinator::reset() { frame_coordinator_.reset(); }

const ReadOnlyOverlayFrameMetrics &
SubjectMaskPresentationCoordinator::metrics() const {
  return frame_coordinator_.metrics();
}

} // namespace crimson::overlay
