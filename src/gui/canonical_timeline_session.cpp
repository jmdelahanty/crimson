#include "gui/canonical_timeline_session.h"

#include "zarr/archive_context.h"
#include "zarr/tensorstore_analysis_series_timeline_repository.h"
#include "zarr/tensorstore_eye_angle_timeline_repository.h"
#include "zarr/tensorstore_swim_bout_timeline_repository.h"

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>

namespace crimson::gui {
namespace {

using Clock = std::chrono::steady_clock;
using json = nlohmann::json;

void assignError(std::string* destination, const std::string& value) {
  if (destination) {
    *destination = value;
  }
}

bool validName(const std::string& value) {
  return !value.empty() && value != "." && value != ".." &&
         value.find('/') == std::string::npos;
}

std::string stringValue(const json& object, const char* key) {
  const auto found = object.find(key);
  return found != object.end() && found->is_string()
             ? found->get<std::string>()
             : std::string{};
}

int64_t integerValue(const json& object, const char* key,
                     int64_t fallback = -1) {
  const auto found = object.find(key);
  return found != object.end() && found->is_number_integer()
             ? found->get<int64_t>()
             : fallback;
}

std::optional<json> readAttributes(const std::filesystem::path& root,
                                   const std::string& group) {
  std::ifstream input(root / group / "zarr.json");
  if (!input) {
    return std::nullopt;
  }
  json metadata;
  try {
    input >> metadata;
  } catch (...) {
    return std::nullopt;
  }
  const auto attributes = metadata.find("attributes");
  return attributes != metadata.end() && attributes->is_object()
             ? std::optional<json>(*attributes)
             : std::nullopt;
}

std::optional<std::string> selectedRun(const std::filesystem::path& root,
                                       const std::string& group,
                                       std::string* error) {
  const auto attributes = readAttributes(root, group);
  if (!attributes) {
    assignError(error, "Selector metadata is unavailable: " + group);
    return std::nullopt;
  }
  const std::string latest = stringValue(*attributes, "latest");
  const std::string complete = stringValue(*attributes, "latest_complete");
  if (latest.empty() || complete.empty() || latest != complete) {
    assignError(error, "latest/latest_complete do not select one exact run: " +
                           group);
    return std::nullopt;
  }
  return latest;
}

struct SelectedSources {
  std::string eye;
  std::string motion_scope;
  std::string motion;
  int64_t motion_track = -1;
  std::string bout;
  bool motion_bout_mismatch = false;
  std::string mismatch_error;
};

bool resolveSelectedSources(const CanonicalTimelineOpenRequest& request,
                            SelectedSources* selected,
                            std::string* fatal_error,
                            std::string* eye_error,
                            std::string* motion_error,
                            std::string* bout_error) {
  const std::filesystem::path root(request.archive_path);
  selected->motion_scope =
      request.motion_scope.empty() ? "offline" : request.motion_scope;
  selected->motion_track = request.motion_track_id;

  auto resolveSimple = [&](const std::string& group,
                           const std::string& requested, std::string* output,
                           std::string* product_error) {
    std::string selector_error;
    const auto chosen = selectedRun(root, group, &selector_error);
    if (!chosen) {
      *product_error = std::move(selector_error);
      return;
    }
    if (!requested.empty() && requested != *chosen) {
      *product_error = "Requested run disagrees with selected run: " + group;
      return;
    }
    if (!validName(*chosen)) {
      *product_error = "Selected run name is invalid: " + group;
      return;
    }
    *output = *chosen;
  };
  resolveSimple("analysis/eye_angle_runs", request.eye_angle_run,
                &selected->eye, eye_error);
  resolveSimple("analysis/swim_bout_runs", request.swim_bout_run,
                &selected->bout, bout_error);

  std::string selector_error;
  const auto qualified =
      selectedRun(root, "analysis/track_kinematics_runs", &selector_error);
  if (!qualified) {
    *motion_error = std::move(selector_error);
  } else {
    const size_t separator = qualified->find('/');
    if (separator == std::string::npos || separator == 0 ||
        separator + 1 >= qualified->size()) {
      *motion_error = "Track-kinematics selector is not scope-qualified";
    } else {
      const std::string scope = qualified->substr(0, separator);
      const std::string run = qualified->substr(separator + 1);
      if (scope != selected->motion_scope || !validName(run) ||
          (!request.motion_run.empty() && request.motion_run != run)) {
        *motion_error =
            "Requested motion source disagrees with the qualified selector";
      } else {
        selected->motion = run;
      }
    }
  }
  if (selected->motion_track < 0) {
    selected->motion_track = 0;
  }

  auto validatePublication = [&](const std::string& group, const std::string& run,
                                 std::string* error) {
    if (run.empty() || !error->empty()) return;
    const auto attributes = readAttributes(root, group + "/" + run);
    if (!attributes ||
        stringValue(*attributes, "palette_run_completion_status") != "complete" ||
        !attributes->contains("stage_selector_eligible") ||
        !(*attributes)["stage_selector_eligible"].is_boolean() ||
        !(*attributes)["stage_selector_eligible"].get<bool>()) {
      *error = "Selected timeline run is incomplete or selector-ineligible: " +
               group + "/" + run;
    }
  };
  validatePublication("analysis/eye_angle_runs", selected->eye, eye_error);
  validatePublication("analysis/track_kinematics_runs/" + selected->motion_scope,
                      selected->motion, motion_error);
  validatePublication("analysis/swim_bout_runs", selected->bout, bout_error);

  if (!selected->bout.empty()) {
    const auto attributes = readAttributes(
        root, "analysis/swim_bout_runs/" + selected->bout);
    if (!attributes) {
      *bout_error = "Selected swim-bout run metadata is unavailable";
    } else if (stringValue(*attributes, "schema_id") !=
                   "palette.swim_bout_runs" ||
               integerValue(*attributes, "schema_version") != 8 ||
               stringValue(*attributes, "layout") !=
                   "compact_tabular_v2") {
      *bout_error = "Selected swim-bout run is not compact schema 8";
    } else if (!selected->motion.empty()) {
      const std::string bound_run =
          stringValue(*attributes, "source_track_kinematics_run");
      const std::string bound_scope =
          stringValue(*attributes, "source_track_kinematics_scope");
      const int64_t bound_track = integerValue(*attributes, "track_id");
      if (bound_run != selected->motion ||
          bound_scope != selected->motion_scope ||
          bound_track != selected->motion_track) {
        selected->motion_bout_mismatch = true;
        selected->mismatch_error =
            "Selected motion and swim-bout runs have different bound source "
            "identities";
      }
    }
  }
  if (request.archive_path.empty() || request.expected_frame_count == 0 ||
      !std::isfinite(request.frames_per_second) ||
      request.frames_per_second <= 0.0) {
    *fatal_error = "Canonical timeline archive/frame/time identity is invalid";
    return false;
  }
  return true;
}

template <typename Window>
CanonicalTimelineProductState windowState(
    const std::shared_ptr<const Window>& window);

template <>
CanonicalTimelineProductState windowState(
    const std::shared_ptr<const timeline::EyeAngleTimelineWindow>& window) {
  if (!window) return CanonicalTimelineProductState::Pending;
  switch (window->status) {
    case timeline::EyeAngleTimelineStatus::Mapped:
      return window->traces.empty() ? CanonicalTimelineProductState::Empty
                                    : CanonicalTimelineProductState::Ready;
    case timeline::EyeAngleTimelineStatus::Missing:
    case timeline::EyeAngleTimelineStatus::OutOfRange:
      return CanonicalTimelineProductState::Empty;
    default:
      return CanonicalTimelineProductState::Error;
  }
}

template <>
CanonicalTimelineProductState windowState(
    const std::shared_ptr<const timeline::AnalysisSeriesTimelineWindow>& window) {
  if (!window) return CanonicalTimelineProductState::Pending;
  switch (window->status) {
    case timeline::AnalysisSeriesTimelineStatus::Mapped:
      return window->traces.empty() ? CanonicalTimelineProductState::Empty
                                    : CanonicalTimelineProductState::Ready;
    case timeline::AnalysisSeriesTimelineStatus::Missing:
    case timeline::AnalysisSeriesTimelineStatus::OutOfRange:
      return CanonicalTimelineProductState::Empty;
    default:
      return CanonicalTimelineProductState::Error;
  }
}

template <>
CanonicalTimelineProductState windowState(
    const std::shared_ptr<const timeline::SwimBoutTimelineWindow>& window) {
  if (!window) return CanonicalTimelineProductState::Pending;
  switch (window->status) {
    case timeline::SwimBoutTimelineStatus::Mapped:
      // Detector samples can still be present when the temporal
      // segmentation contains no events. Preserve that as ready-empty bouts,
      // distinct from a pending/unavailable product.
      return window->intervals.empty() ? CanonicalTimelineProductState::Empty
                                       : CanonicalTimelineProductState::Ready;
    case timeline::SwimBoutTimelineStatus::Missing:
    case timeline::SwimBoutTimelineStatus::OutOfRange:
      return CanonicalTimelineProductState::Empty;
    default:
      return CanonicalTimelineProductState::Error;
  }
}

}  // namespace

struct CanonicalTimelineSession::Impl {
  explicit Impl(std::shared_ptr<data::DataAccessScheduler> scheduler_value)
      : scheduler(std::move(scheduler_value)) {}

  mutable std::mutex lifecycle_mutex;
  mutable std::mutex mutex;
  mutable std::condition_variable condition;
  std::shared_ptr<data::DataAccessScheduler> scheduler;
  std::shared_ptr<zarr::ArchiveContext> archive;
  std::shared_ptr<EyeAngleTimelineBuffer> eye;
  std::shared_ptr<AnalysisSeriesTimelineBuffer> motion;
  std::shared_ptr<SwimBoutTimelineBuffer> bouts;
  std::thread open_worker;
  std::atomic<bool> cancel_requested{false};
  CanonicalTimelineOpenRequest request;
  SelectedSources selected;
  CanonicalTimelineSessionMetrics observed;

  void closeWithLifecycleLock() {
    cancel_requested.store(true, std::memory_order_release);
    std::shared_ptr<EyeAngleTimelineBuffer> old_eye;
    std::shared_ptr<AnalysisSeriesTimelineBuffer> old_motion;
    std::shared_ptr<SwimBoutTimelineBuffer> old_bouts;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (observed.state == CanonicalTimelineSessionState::Opening) {
        ++observed.cancelled_opens;
      }
      ++observed.generation;
      observed.state = CanonicalTimelineSessionState::Closed;
      old_eye = std::move(eye);
      old_motion = std::move(motion);
      old_bouts = std::move(bouts);
      archive.reset();
      condition.notify_all();
    }
    if (old_eye) old_eye->close();
    if (old_motion) old_motion->close();
    if (old_bouts) old_bouts->close();
    if (open_worker.joinable()) open_worker.join();
  }
};

CanonicalTimelineSession::CanonicalTimelineSession(
    std::shared_ptr<data::DataAccessScheduler> scheduler)
    : impl_(std::make_unique<Impl>(std::move(scheduler))) {}

CanonicalTimelineSession::~CanonicalTimelineSession() { close(); }

bool CanonicalTimelineSession::beginOpen(CanonicalTimelineOpenRequest request,
                                         std::string* error) {
  std::lock_guard<std::mutex> lifecycle(impl_->lifecycle_mutex);
  impl_->closeWithLifecycleLock();
  if (!impl_->scheduler || !impl_->scheduler->running()) {
    assignError(error, "Canonical timeline scheduler is unavailable");
    return false;
  }
  if (request.archive_path.empty() || request.expected_frame_count == 0 ||
      !std::isfinite(request.frames_per_second) ||
      request.frames_per_second <= 0.0 || request.page_span_frames < 3 ||
      request.page_step_frames == 0 ||
      request.page_step_frames > request.page_span_frames ||
      request.max_points_per_trace < 3 || request.cache_pages == 0) {
    assignError(error, "Canonical timeline open request is invalid");
    return false;
  }
  uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->cancel_requested.store(false, std::memory_order_release);
    impl_->request = request;
    impl_->selected = {};
    impl_->observed.state = CanonicalTimelineSessionState::Opening;
    impl_->observed.archive_path = request.archive_path;
    impl_->observed.eye_angle_run = request.eye_angle_run;
    impl_->observed.motion_run = request.motion_run;
    impl_->observed.swim_bout_run = request.swim_bout_run;
    impl_->observed.eye_angle_error.clear();
    impl_->observed.motion_error.clear();
    impl_->observed.swim_bout_error.clear();
    impl_->observed.last_error.clear();
    ++impl_->observed.open_attempts;
    generation = ++impl_->observed.generation;
  }

  impl_->open_worker = std::thread(
      [this, request = std::move(request), generation]() mutable {
        try {
        const auto started = Clock::now();
        SelectedSources selected;
        std::string fatal_error;
        std::string eye_error;
        std::string motion_error;
        std::string bout_error;
        // Selector discovery is deliberately on this worker. It reads only
        // small per-group/run zarr.json files, never root consolidated JSON.
        const bool selection_ready = resolveSelectedSources(
            request, &selected, &fatal_error, &eye_error, &motion_error,
            &bout_error);
        if (impl_->cancel_requested.load(std::memory_order_acquire)) return;
        std::string archive_error;
        auto archive = selection_ready
                           ? zarr::ArchiveContext::Open(request.archive_path,
                                                       &archive_error)
                           : nullptr;
        if (impl_->cancel_requested.load(std::memory_order_acquire)) return;
        std::shared_ptr<EyeAngleTimelineBuffer> eye;
        std::shared_ptr<AnalysisSeriesTimelineBuffer> motion;
        std::shared_ptr<SwimBoutTimelineBuffer> bouts;

        if (archive && !selected.eye.empty() && eye_error.empty()) {
          auto repository = zarr::OpenEyeAngleTimelineRepository(
              archive, selected.eye, &eye_error, {});
          if (repository &&
              repository->descriptor().frame_count !=
                  request.expected_frame_count) {
            eye_error = "Eye-angle camera-frame count disagrees with video";
            repository.reset();
          }
          if (repository) {
            eye = std::make_shared<EyeAngleTimelineBuffer>(
                impl_->scheduler, request.archive_path);
            if (!eye->open(std::move(repository), request.page_span_frames,
                           request.page_step_frames,
                           request.max_points_per_trace, request.cache_pages,
                           &eye_error)) {
              eye.reset();
            }
          }
        }
        if (impl_->cancel_requested.load(std::memory_order_acquire)) return;

        if (archive && !selected.motion.empty() && motion_error.empty()) {
          zarr::MotionSeriesTimelineOpenRequest open_request;
          open_request.frame_count_hint = request.expected_frame_count;
          open_request.scope = selected.motion_scope;
          open_request.run_name = selected.motion;
          open_request.track_id = selected.motion_track;
          open_request.require_source_identity = true;
          auto repository = zarr::OpenMotionSeriesTimelineRepository(
              archive, open_request, &motion_error, {});
          if (repository &&
              repository->descriptor().frame_count !=
                  request.expected_frame_count) {
            motion_error = "Motion camera-frame count disagrees with video";
            repository.reset();
          }
          if (repository) {
            motion = std::make_shared<AnalysisSeriesTimelineBuffer>(
                impl_->scheduler, request.archive_path);
            if (!motion->open(std::move(repository), request.page_span_frames,
                              request.page_step_frames,
                              request.max_points_per_trace,
                              request.cache_pages, &motion_error)) {
              motion.reset();
            }
          }
        }
        if (impl_->cancel_requested.load(std::memory_order_acquire)) return;

        if (archive && !selected.bout.empty() && bout_error.empty()) {
          auto repository = zarr::OpenSwimBoutTimelineRepository(
              archive, request.expected_frame_count, selected.bout,
              &bout_error);
          if (repository &&
              repository->descriptor().frame_count !=
                  request.expected_frame_count) {
            bout_error = "Swim-bout camera-frame count disagrees with video";
            repository.reset();
          }
          if (repository) {
            bouts = std::make_shared<SwimBoutTimelineBuffer>(
                impl_->scheduler, request.archive_path);
            if (!bouts->open(std::move(repository), request.page_span_frames,
                             request.page_step_frames,
                             request.max_points_per_trace,
                             request.cache_pages, &bout_error)) {
              bouts.reset();
            }
          }
        }
        if (impl_->cancel_requested.load(std::memory_order_acquire)) return;

        if (selected.motion_bout_mismatch) {
          motion.reset();
          bouts.reset();
          motion_error = selected.mismatch_error;
          bout_error = selected.mismatch_error;
        } else if (motion && bouts) {
          const auto motion_descriptor = motion->descriptor();
          const auto bout_descriptor = bouts->descriptor();
          const auto* motion_source = timeline::findAnalysisSeriesSource(
              motion_descriptor, motion_descriptor.default_source);
          const auto* bout_candidate = timeline::findSwimBoutCandidate(
              bout_descriptor, bout_descriptor.default_candidate);
          if (!motion_source || !bout_candidate ||
              !timeline::swimBoutCandidateCompatible(*bout_candidate,
                                                      *motion_source)) {
            motion.reset();
            bouts.reset();
            motion_error = "Motion/bout runtime source identities disagree";
            bout_error = motion_error;
          }
        }

        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(Clock::now() - started)
                .count();
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->cancel_requested.load(std::memory_order_acquire) ||
            generation != impl_->observed.generation) {
          return;
        }
        impl_->observed.last_open_ms = elapsed_ms;
        impl_->selected = selected;
        impl_->observed.eye_angle_run = selected.eye;
        impl_->observed.motion_run = selected.motion;
        impl_->observed.swim_bout_run = selected.bout;
        if (!selection_ready || !archive) {
          impl_->observed.state = CanonicalTimelineSessionState::Failed;
          impl_->observed.last_error =
              !fatal_error.empty() ? fatal_error : archive_error;
          ++impl_->observed.failed_opens;
        } else {
          impl_->archive = std::move(archive);
          impl_->eye = std::move(eye);
          impl_->motion = std::move(motion);
          impl_->bouts = std::move(bouts);
          if (!eye_error.empty()) impl_->observed.eye_angle_error = eye_error;
          if (!motion_error.empty()) impl_->observed.motion_error = motion_error;
          if (!bout_error.empty()) impl_->observed.swim_bout_error = bout_error;
          if (impl_->eye || impl_->motion || impl_->bouts) {
            impl_->observed.state = CanonicalTimelineSessionState::Ready;
            ++impl_->observed.successful_opens;
          } else {
            impl_->observed.state = CanonicalTimelineSessionState::Failed;
            impl_->observed.last_error =
                "No selected canonical timeline product could be opened";
            ++impl_->observed.failed_opens;
          }
        }
        impl_->condition.notify_all();
        } catch (const std::exception& exception) {
          std::lock_guard<std::mutex> lock(impl_->mutex);
          if (!impl_->cancel_requested.load(std::memory_order_acquire) &&
              generation == impl_->observed.generation) {
            impl_->observed.state = CanonicalTimelineSessionState::Failed;
            impl_->observed.last_error =
                std::string("Canonical timeline open threw: ") +
                exception.what();
            ++impl_->observed.failed_opens;
            impl_->condition.notify_all();
          }
        } catch (...) {
          std::lock_guard<std::mutex> lock(impl_->mutex);
          if (!impl_->cancel_requested.load(std::memory_order_acquire) &&
              generation == impl_->observed.generation) {
            impl_->observed.state = CanonicalTimelineSessionState::Failed;
            impl_->observed.last_error =
                "Canonical timeline open threw an unknown exception";
            ++impl_->observed.failed_opens;
            impl_->condition.notify_all();
          }
        }
      });
  if (error) error->clear();
  return true;
}

void CanonicalTimelineSession::close() {
  std::lock_guard<std::mutex> lifecycle(impl_->lifecycle_mutex);
  impl_->closeWithLifecycleLock();
}

CanonicalTimelineSessionState CanonicalTimelineSession::state() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->observed.state;
}

bool CanonicalTimelineSession::waitUntilOpen(
    std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  return impl_->condition.wait_for(lock, timeout, [&] {
    return impl_->observed.state != CanonicalTimelineSessionState::Opening;
  });
}

bool CanonicalTimelineSession::requestFrame(int64_t frame, bool discontinuity,
                                            std::string* error) {
  std::shared_ptr<EyeAngleTimelineBuffer> eye;
  std::shared_ptr<AnalysisSeriesTimelineBuffer> motion;
  std::shared_ptr<SwimBoutTimelineBuffer> bouts;
  CanonicalTimelineOpenRequest request;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->observed.state != CanonicalTimelineSessionState::Ready ||
        frame < 0 ||
        static_cast<uint64_t>(frame) >= impl_->request.expected_frame_count) {
      assignError(error, "Canonical timeline frame request is unavailable");
      return false;
    }
    ++impl_->observed.frame_requests;
    eye = impl_->eye;
    motion = impl_->motion;
    bouts = impl_->bouts;
    request = impl_->request;
  }
  bool accepted = false;
  std::string product_error;
  if (eye) {
    accepted |= eye->requestFrame(frame, {}, request.frames_per_second,
                                  discontinuity, &product_error);
  }
  if (motion) {
    accepted |= motion->requestFrame(frame, {}, request.frames_per_second,
                                     discontinuity, &product_error);
  }
  if (bouts) {
    accepted |= bouts->requestFrame(frame, {}, request.frames_per_second,
                                    true, discontinuity, &product_error);
  }
  assignError(error, accepted ? std::string{} : product_error);
  return accepted;
}

CanonicalTimelineSnapshot CanonicalTimelineSession::snapshot(
    int64_t frame) const {
  CanonicalTimelineSnapshot result;
  std::shared_ptr<EyeAngleTimelineBuffer> eye;
  std::shared_ptr<AnalysisSeriesTimelineBuffer> motion;
  std::shared_ptr<SwimBoutTimelineBuffer> bouts;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    result.generation = impl_->observed.generation;
    result.requested_frame = frame;
    result.state = impl_->observed.state;
    result.eye_angles.error = impl_->observed.eye_angle_error;
    result.motion.error = impl_->observed.motion_error;
    result.swim_bouts.error = impl_->observed.swim_bout_error;
    eye = impl_->eye;
    motion = impl_->motion;
    bouts = impl_->bouts;
  }
  auto unopenedState = [&](const std::string& product_error) {
    if (!product_error.empty()) return CanonicalTimelineProductState::Error;
    return result.state == CanonicalTimelineSessionState::Opening
               ? CanonicalTimelineProductState::Opening
               : CanonicalTimelineProductState::Unavailable;
  };
  result.eye_angles.state = unopenedState(result.eye_angles.error);
  result.motion.state = unopenedState(result.motion.error);
  result.swim_bouts.state = unopenedState(result.swim_bouts.error);
  if (eye) {
    result.eye_angles.descriptor = eye->descriptor();
    result.eye_angles.source_identity =
        result.eye_angles.descriptor.source_group + "/" +
        result.eye_angles.descriptor.run_name;
    result.eye_angles.window = eye->window(frame, {});
    result.eye_angles.state = windowState(result.eye_angles.window);
    if (result.eye_angles.window &&
        result.eye_angles.state == CanonicalTimelineProductState::Error) {
      result.eye_angles.error = result.eye_angles.window->error;
    }
  }
  if (motion) {
    result.motion.descriptor = motion->descriptor();
    result.motion.source_identity = result.motion.descriptor.default_source;
    result.motion.window = motion->window(frame, {});
    result.motion.state = windowState(result.motion.window);
    if (result.motion.window &&
        result.motion.state == CanonicalTimelineProductState::Error) {
      result.motion.error = result.motion.window->error;
    }
  }
  if (bouts) {
    result.swim_bouts.descriptor = bouts->descriptor();
    result.swim_bouts.source_identity =
        result.swim_bouts.descriptor.default_candidate;
    result.swim_bouts.window = bouts->window(frame, {}, true);
    result.swim_bouts.state = windowState(result.swim_bouts.window);
    if (result.swim_bouts.window &&
        result.swim_bouts.state == CanonicalTimelineProductState::Error) {
      result.swim_bouts.error = result.swim_bouts.window->error;
    }
  }
  return result;
}

CanonicalTimelineSessionMetrics CanonicalTimelineSession::metrics() const {
  CanonicalTimelineSessionMetrics result;
  std::shared_ptr<EyeAngleTimelineBuffer> eye;
  std::shared_ptr<AnalysisSeriesTimelineBuffer> motion;
  std::shared_ptr<SwimBoutTimelineBuffer> bouts;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    result = impl_->observed;
    eye = impl_->eye;
    motion = impl_->motion;
    bouts = impl_->bouts;
  }
  if (eye) result.eye_angles = eye->metrics();
  if (motion) result.motion = motion->metrics();
  if (bouts) result.swim_bouts = bouts->metrics();
  return result;
}

const char* canonicalTimelineSessionStateName(
    CanonicalTimelineSessionState state) {
  switch (state) {
    case CanonicalTimelineSessionState::Closed: return "closed";
    case CanonicalTimelineSessionState::Opening: return "opening";
    case CanonicalTimelineSessionState::Ready: return "ready";
    case CanonicalTimelineSessionState::Failed: return "failed";
  }
  return "unknown";
}

const char* canonicalTimelineProductStateName(
    CanonicalTimelineProductState state) {
  switch (state) {
    case CanonicalTimelineProductState::Unavailable: return "unavailable";
    case CanonicalTimelineProductState::Opening: return "opening";
    case CanonicalTimelineProductState::Pending: return "pending";
    case CanonicalTimelineProductState::Ready: return "ready";
    case CanonicalTimelineProductState::Empty: return "empty";
    case CanonicalTimelineProductState::Error: return "error";
  }
  return "unknown";
}

}  // namespace crimson::gui
