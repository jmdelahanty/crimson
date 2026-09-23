#include "gui/canonical_overlay_session.h"

#include "keypoint_overlay_buffer.h"
#include "subject_mask_overlay_buffer.h"
#include "subject_shape_overlay_buffer.h"

#include <condition_variable>
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace crimson::gui {
namespace {
using Clock = std::chrono::steady_clock;
std::atomic<uint64_t> next_session_identity{1};
template <typename Product, typename Buffer>
void populate(Product& output, const std::shared_ptr<Buffer>& buffer,
              const std::string& open_error, int64_t frame) {
  output.error = open_error;
  if (!buffer) {
    output.state = open_error.empty() ? CanonicalOverlayState::Unavailable
                                     : CanonicalOverlayState::Failed;
    return;
  }
  output.descriptor = buffer->descriptor();
  output.frame = buffer->frame(frame);
  if (!output.frame) { output.state = CanonicalOverlayState::Pending; return; }
  if (output.frame->camera_frame != frame) {
    output.frame.reset();
    output.state = CanonicalOverlayState::Failed;
    output.error = "Overlay snapshot frame disagrees with presented frame";
    return;
  }
  // All three typed repository statuses have the same named states, but do
  // not depend on enum ordinals.
  using Status = decltype(output.frame->status);
  if (output.frame->status == Status::Mapped) {
    output.state = output.frame->detections.empty() ? CanonicalOverlayState::Empty
                                                   : CanonicalOverlayState::Ready;
    if (!output.frame->error.empty()) output.error = output.frame->error;
  } else if (output.frame->status == Status::Missing) {
    output.state = CanonicalOverlayState::Empty;
  } else {
    output.state = CanonicalOverlayState::Failed;
    output.error = output.frame->error;
    if (output.error.empty() && output.frame->status == Status::OutOfRange)
      output.error = "Overlay reader frame domain disagrees with indexed video";
  }
}
} // namespace

struct CanonicalOverlaySession::Impl {
  struct Epoch {
    CanonicalOverlayOpenRequest request;
    std::shared_ptr<const zarr::CanonicalOverlaySelection> selection;
    std::shared_ptr<KeypointOverlayBuffer> keypoints;
    std::shared_ptr<SubjectMaskOverlayBuffer> masks;
    std::shared_ptr<SubjectMaskOverlayBuffer> mask_contours;
    std::shared_ptr<SubjectShapeOverlayBuffer> shapes;
    std::string keypoint_error, mask_error, mask_contour_error, shape_error;
    double open_ms = 0;
    void retire() {
      if (keypoints) keypoints->close();
      if (masks) masks->close();
      if (mask_contours) mask_contours->close();
      if (shapes) shapes->close();
    }
  };
  std::shared_ptr<data::DataAccessScheduler> scheduler;
  CanonicalOverlayOpenFunction opener;
  mutable std::mutex mutex;
  mutable std::condition_variable condition;
  std::thread worker;
  bool stopping = false;
  const uint64_t session_identity = next_session_identity.fetch_add(1);
  uint64_t generation = 0;
  CanonicalOverlayState state = CanonicalOverlayState::Closed;
  CanonicalOverlayOpenRequest desired;
  std::shared_ptr<Epoch> active;
  std::string error;

  Impl(std::shared_ptr<data::DataAccessScheduler> shared,
       CanonicalOverlayOpenFunction open)
      : scheduler(std::move(shared)), opener(std::move(open)) {
    if (!opener) opener = openCanonicalOverlayRepositories;
    worker = std::thread([this] { run(); });
  }

  void run() {
    uint64_t seen = 0;
    std::shared_ptr<Epoch> owned;
    for (;;) {
      CanonicalOverlayOpenRequest request;
      uint64_t version;
      bool open = false;
      {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return stopping || generation != seen; });
        version = generation;
        seen = version;
        request = desired;
        open = state == CanonicalOverlayState::Opening && !stopping;
      }
      // This worker, not the UI's last shared_ptr release, owns retirement.
      if (owned) { owned->retire(); owned.reset(); }
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping) return;
        if (version != generation || !open) continue;
      }
      const auto started = Clock::now();
      std::shared_ptr<Epoch> candidate;
      std::string open_error;
      try {
        auto repositories = opener(request);
        open_error = repositories.error;
        if (open_error.empty()) {
          candidate = std::make_shared<Epoch>();
          candidate->request = request;
          candidate->selection = std::make_shared<const zarr::CanonicalOverlaySelection>(
              std::move(repositories.selection));
          candidate->keypoint_error = std::move(repositories.keypoint_error);
          candidate->mask_error = std::move(repositories.mask_error);
          candidate->mask_contour_error =
              std::move(repositories.mask_contour_error);
          candidate->shape_error = std::move(repositories.shape_error);
          const auto identity = request.archive_path + ":canonical-overlay:" +
                                std::to_string(session_identity) + ":" + std::to_string(version);
          // Overlay demand is bounded by each typed buffer policy. Mask
          // read-ahead remains opt-in per request and runs through the shared
          // scheduler rather than a repository-owned prefetch worker.
          if (repositories.keypoints) {
            candidate->keypoints = std::make_shared<KeypointOverlayBuffer>(scheduler, identity);
            if (!candidate->keypoints->open(std::move(repositories.keypoints), 2, 8,
                                           &candidate->keypoint_error))
              candidate->keypoints.reset();
          }
          if (repositories.masks) {
            candidate->masks = std::make_shared<SubjectMaskOverlayBuffer>(scheduler, identity);
            SubjectMaskOverlayBufferPolicy mask_policy;
            if (!candidate->masks->open(std::move(repositories.masks),
                                       mask_policy, &candidate->mask_error))
              candidate->masks.reset();
          }
          if (repositories.mask_contours) {
            candidate->mask_contours = std::make_shared<SubjectMaskOverlayBuffer>(
                scheduler, identity + ":mask-contours");
            SubjectMaskOverlayBufferPolicy contour_policy;
            contour_policy.maximum_cached_frames = 32;
            contour_policy.maximum_cached_payload_bytes = 64ULL * 1024ULL * 1024ULL;
            if (!candidate->mask_contours->open(
                    std::move(repositories.mask_contours), contour_policy,
                    &candidate->mask_contour_error))
              candidate->mask_contours.reset();
          }
          if (repositories.shapes) {
            candidate->shapes = std::make_shared<SubjectShapeOverlayBuffer>(scheduler, identity);
            if (!candidate->shapes->open(std::move(repositories.shapes), 2, 8,
                                        &candidate->shape_error))
              candidate->shapes.reset();
          }
          candidate->open_ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
          if (!candidate->keypoints && !candidate->masks && !candidate->shapes)
            open_error = "No bound canonical overlay product could be opened";
        }
      } catch (const std::exception& exception) {
        open_error = exception.what();
      } catch (...) {
        open_error = "Canonical overlay opener threw an unknown exception";
      }
      owned = std::move(candidate);
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (!stopping && version == generation) {
          active = owned;
          error = std::move(open_error);
          state = error.empty() ? CanonicalOverlayState::Ready : CanonicalOverlayState::Failed;
          condition.notify_all();
        }
      }
    }
  }
};

CanonicalOverlaySession::CanonicalOverlaySession(
    std::shared_ptr<data::DataAccessScheduler> scheduler,
    CanonicalOverlayOpenFunction opener)
    : impl_(std::make_unique<Impl>(std::move(scheduler), std::move(opener))) {}
CanonicalOverlaySession::~CanonicalOverlaySession() { shutdown(); }
void CanonicalOverlaySession::shutdown() {
  close();
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->stopping = true;
    impl_->condition.notify_all();
  }
  if (impl_->worker.joinable()) impl_->worker.join();
}
bool CanonicalOverlaySession::beginOpen(CanonicalOverlayOpenRequest request,
                                       std::string* error) {
  if (!impl_->scheduler || !impl_->scheduler->running() || request.archive_path.empty() ||
      request.recording_id.empty() ||
      !request.frame_count || request.source_width <= 0 || request.source_height <= 0) {
    if (error) *error = "Canonical overlay archive/frame/dimension/scheduler request is invalid";
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->stopping) {
    if (error) *error = "Canonical overlay session is shut down";
    return false;
  }
  ++impl_->generation;
  impl_->active.reset();
  impl_->desired = std::move(request);
  impl_->error.clear();
  impl_->state = CanonicalOverlayState::Opening;
  impl_->condition.notify_all();
  return true;
}
void CanonicalOverlaySession::close() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  ++impl_->generation;
  impl_->active.reset();
  impl_->state = CanonicalOverlayState::Closed;
  impl_->error.clear();
  impl_->condition.notify_all();
}
bool CanonicalOverlaySession::requestFrame(int64_t frame, bool keypoints,
                                          bool masks, bool shapes,
                                          bool discontinuity,
                                          CanonicalOverlayPlaybackDemand playback,
                                          bool mask_contours) {
  std::shared_ptr<Impl::Epoch> epoch;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->state != CanonicalOverlayState::Ready) return false;
    epoch = impl_->active;
  }
  if (!epoch || frame < 0 || static_cast<uint64_t>(frame) >= epoch->request.frame_count) return false;
  const int width = epoch->request.source_width, height = epoch->request.source_height;
  bool accepted = true;
  if (keypoints && epoch->keypoints)
    accepted = epoch->keypoints->requestFrame(frame, width, height, discontinuity) && accepted;
  if ((masks && epoch->masks) || (mask_contours && epoch->mask_contours)) {
    SubjectMaskPlaybackDemand mask_playback;
    mask_playback.read_ahead = playback.read_ahead;
    mask_playback.source_frames_per_second =
        playback.source_frames_per_second;
    mask_playback.playback_rate = playback.playback_rate;
    switch (playback.direction) {
      case CanonicalOverlayPlaybackDirection::Paused:
        mask_playback.direction = SubjectMaskPlaybackDirection::Paused;
        break;
      case CanonicalOverlayPlaybackDirection::Forward:
        mask_playback.direction = SubjectMaskPlaybackDirection::Forward;
        break;
      case CanonicalOverlayPlaybackDirection::Reverse:
        mask_playback.direction = SubjectMaskPlaybackDirection::Reverse;
        break;
    }
    if (masks && epoch->masks)
      accepted = epoch->masks->requestFrame(
                     frame, width, height, mask_playback, discontinuity) &&
                 accepted;
    if (mask_contours && epoch->mask_contours)
      accepted = epoch->mask_contours->requestFrame(
                     frame, width, height, mask_playback, discontinuity) &&
                 accepted;
  }
  if (shapes && epoch->shapes)
    accepted = epoch->shapes->requestFrame(frame, width, height, discontinuity) && accepted;
  return accepted;
}
CanonicalOverlaySnapshot CanonicalOverlaySession::snapshot(int64_t frame) const {
  CanonicalOverlaySnapshot result;
  std::shared_ptr<Impl::Epoch> epoch;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    result.generation = impl_->generation;
    result.state = impl_->state;
    result.error = impl_->error;
    epoch = impl_->active;
  }
  result.requested_frame = frame;
  if (epoch) {
    result.selection = epoch->selection;
    result.open_ms = epoch->open_ms;
    result.mask_contour_error = epoch->mask_contour_error;
    populate(result.keypoints, epoch->keypoints, epoch->keypoint_error, frame);
    populate(result.masks, epoch->masks, epoch->mask_error, frame);
    populate(result.mask_contours, epoch->mask_contours,
             epoch->mask_contour_error, frame);
    populate(result.shapes, epoch->shapes, epoch->shape_error, frame);
    if (epoch->masks) {
      result.mask_metrics = epoch->masks->repositoryMetrics();
      result.mask_buffer_metrics = epoch->masks->metrics();
    }
    if (epoch->mask_contours) {
      result.mask_contour_metrics = epoch->mask_contours->repositoryMetrics();
      result.mask_contour_buffer_metrics = epoch->mask_contours->metrics();
    }
  } else if (result.state == CanonicalOverlayState::Opening) {
    result.keypoints.state = result.masks.state = result.mask_contours.state =
        result.shapes.state = CanonicalOverlayState::Opening;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (result.generation != impl_->generation) {
      result = {};
      result.generation = impl_->generation;
      result.state = impl_->state;
      result.requested_frame = frame;
    }
  }
  return result;
}
bool CanonicalOverlaySession::waitUntilOpen(std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  return impl_->condition.wait_for(lock, timeout, [&] {
    return impl_->state != CanonicalOverlayState::Opening;
  }) && impl_->state == CanonicalOverlayState::Ready;
}
const char* canonicalOverlayStateName(CanonicalOverlayState state) {
  switch (state) {
    case CanonicalOverlayState::Closed: return "closed";
    case CanonicalOverlayState::Opening: return "opening";
    case CanonicalOverlayState::Pending: return "pending";
    case CanonicalOverlayState::Ready: return "ready";
    case CanonicalOverlayState::Empty: return "empty";
    case CanonicalOverlayState::Unavailable: return "unavailable";
    case CanonicalOverlayState::Failed: return "failed";
  }
  return "failed";
}
} // namespace crimson::gui
