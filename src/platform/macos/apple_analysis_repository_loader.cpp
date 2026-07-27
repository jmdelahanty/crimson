#include "apple_analysis_repository_loader.h"

#include "zarr/detection_repository_selection.h"
#include "zarr/tensorstore_acquisition_crop_repository.h"
#include "zarr/tensorstore_analysis_crop_geometry_repository.h"
#include "zarr/tensorstore_analysis_series_timeline_repository.h"
#include "zarr/tensorstore_chaser_distance_polar_repository.h"
#include "zarr/tensorstore_eye_angle_timeline_repository.h"
#include "zarr/tensorstore_eye_geometry_overlay_repository.h"
#include "zarr/tensorstore_keypoint_overlay_repository.h"
#include "zarr/tensorstore_stimulus_context_timeline_repository.h"
#include "zarr/tensorstore_stimulus_repository.h"
#include "zarr/tensorstore_subject_mask_overlay_repository.h"
#include "zarr/tensorstore_subject_shape_overlay_repository.h"
#include "zarr/tensorstore_swim_bout_timeline_repository.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace {

double elapsedMilliseconds(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - start)
      .count();
}

struct LoadControl {
  std::atomic<bool> *cancel_requested = nullptr;
  std::function<void(std::string, std::string, bool)> report;

  bool cancelled() const {
    return cancel_requested != nullptr &&
           cancel_requested->load(std::memory_order_acquire);
  }
};

template <typename Open>
bool loadRepository(AppleAnalysisRepositoryBundle *bundle,
                    const std::string &product, const std::string &phase,
                    const LoadControl &control, Open open) {
  if (control.cancelled()) {
    bundle->cancelled = true;
    return false;
  }
  if (control.report) {
    control.report(product, phase, false);
  }
  std::string error;
  const auto started = std::chrono::steady_clock::now();
  const bool available = open(&error);
  bundle->timings.push_back(
      {product, elapsedMilliseconds(started), available, std::move(error)});
  if (control.report) {
    control.report(product, phase, true);
  }
  if (control.cancelled()) {
    bundle->cancelled = true;
    return false;
  }
  return true;
}

size_t productCount(const AppleAnalysisRepositoryLoadRequest &request) {
  size_t result = 6;
  result +=
      request.detection_run.empty() && request.refined_detection_run.empty()
          ? 0
          : 1;
  result += request.subject_masks_enabled ? 1 : 0;
  result += request.subject_shapes_enabled ? 1 : 0;
  result += request.eye_geometry_enabled ? 1 : 0;
  result += request.motion_timeline_enabled ? 1 : 0;
  result += request.swim_bout_timeline_enabled ? 1 : 0;
  result += request.eye_angle_timeline_enabled ? 1 : 0;
  result += request.tail_kinematics_timeline_enabled ? 1 : 0;
  result += request.stimulus_context_timeline_enabled ? 1 : 0;
  return result;
}

bool openSelectedDetections(const AppleAnalysisRepositoryLoadRequest &request,
                            AppleAnalysisRepositoryBundle *bundle,
                            std::string *error) {
  crimson::zarr::DetectionRepositorySelectionRequest selection;
  selection.explicit_refined_run = request.refined_detection_run;
  selection.canonical_raw_run = request.detection_run;
  selection.raw_fallback_policy =
      request.detection_run.empty()
          ? crimson::zarr::DetectionRawFallbackPolicy::Forbid
          : crimson::zarr::DetectionRawFallbackPolicy::
                AllowOnlyWhenNoRefinedAuthority;
  selection.allow_selector_ineligible_benchmark =
      request.allow_selector_ineligible_refined_run;
  bundle->canonical_detection = crimson::zarr::OpenSelectedDetectionRepository(
      bundle->archive, selection, error, &bundle->detection_selection_metrics);
  if (bundle->detection_selection_metrics.kind ==
      crimson::zarr::DetectionRepositorySelectionKind::
          ExplicitlyPermittedCanonicalRaw) {
    bundle->canonical_detection_open_metrics =
        bundle->detection_selection_metrics.canonical_open;
  } else {
    const auto &refined = bundle->detection_selection_metrics.refined_open;
    auto &common = bundle->canonical_detection_open_metrics;
    common.total_ms = refined.total_ms;
    common.root_metadata_ms = refined.root_metadata_ms;
    common.exact_handle_open_ms = refined.exact_handle_open_ms;
    common.offset_read_ms = refined.offset_read_ms;
    common.root_metadata_reads = refined.root_metadata_reads;
    common.consolidated_array_declarations =
        refined.consolidated_array_declarations;
    common.exact_handle_opens = refined.exact_handle_opens;
    common.offset_read_calls = refined.offset_read_calls;
    common.retained_offset_bytes = refined.retained_offset_bytes;
  }
  return bundle->canonical_detection != nullptr;
}

AppleAnalysisRepositoryBundle
openBundle(const AppleAnalysisRepositoryLoadRequest &request,
           const LoadControl &control) {
  AppleAnalysisRepositoryBundle bundle;
  const auto all_started = std::chrono::steady_clock::now();
  if (control.report) {
    control.report("archive", "Resolving archive", false);
  }
  bundle.archive = crimson::zarr::ArchiveContext::Open(request.archive_path,
                                                       &bundle.archive_error);
  if (control.report) {
    control.report("archive", "Resolving archive", true);
  }
  if (!bundle.archive || control.cancelled()) {
    bundle.cancelled = control.cancelled();
    bundle.total_elapsed_ms = elapsedMilliseconds(all_started);
    return bundle;
  }

  if (!loadRepository(
          &bundle, "chaser_polar", "Loading polar analysis", control,
          [&](std::string *error) {
            bundle.chaser_distance_polar =
                crimson::zarr::OpenTensorStoreChaserDistancePolarRepository(
                    bundle.archive, {}, error);
            return bundle.chaser_distance_polar != nullptr;
          })) {
    bundle.total_elapsed_ms = elapsedMilliseconds(all_started);
    return bundle;
  }
  if (!loadRepository(&bundle, "stimulus", "Resolving stimulus media", control,
                      [&](std::string *error) {
                        bundle.stimulus = crimson::zarr::OpenStimulusRepository(
                            bundle.archive, request.stimulus_run, error,
                            request.stimulus_video_override);
                        return bundle.stimulus != nullptr;
                      })) {
    bundle.total_elapsed_ms = elapsedMilliseconds(all_started);
    return bundle;
  }
  if ((!request.detection_run.empty() ||
       !request.refined_detection_run.empty()) &&
      !loadRepository(&bundle, "canonical_detection",
                      "Loading selected detections", control,
                      [&](std::string *error) {
                        return openSelectedDetections(request, &bundle, error);
                      })) {
    bundle.total_elapsed_ms = elapsedMilliseconds(all_started);
    return bundle;
  }
  if (!loadRepository(&bundle, "keypoints", "Loading keypoints", control,
                      [&](std::string *error) {
                        bundle.keypoints =
                            crimson::zarr::OpenKeypointOverlayRepository(
                                bundle.archive, {}, error,
                                &bundle.keypoint_open_metrics);
                        return bundle.keypoints != nullptr;
                      })) {
    bundle.total_elapsed_ms = elapsedMilliseconds(all_started);
    return bundle;
  }
  if (request.subject_masks_enabled &&
      !loadRepository(&bundle, "subject_masks", "Loading mask metadata",
                      control, [&](std::string *error) {
                        bundle.subject_masks =
                            crimson::zarr::OpenSubjectMaskOverlayRepository(
                                bundle.archive, {}, error);
                        return bundle.subject_masks != nullptr;
                      })) {
    bundle.total_elapsed_ms = elapsedMilliseconds(all_started);
    return bundle;
  }
  if (request.subject_shapes_enabled &&
      !loadRepository(&bundle, "subject_shape", "Loading subject shapes",
                      control, [&](std::string *error) {
                        bundle.subject_shape =
                            crimson::zarr::OpenSubjectShapeOverlayRepository(
                                bundle.archive, {}, error);
                        return bundle.subject_shape != nullptr;
                      })) {
    bundle.total_elapsed_ms = elapsedMilliseconds(all_started);
    return bundle;
  }
  if (request.eye_geometry_enabled &&
      !loadRepository(&bundle, "eye_geometry", "Loading eye geometry", control,
                      [&](std::string *error) {
                        bundle.eye_geometry =
                            crimson::zarr::OpenEyeGeometryOverlayRepository(
                                bundle.archive, {}, error);
                        return bundle.eye_geometry != nullptr;
                      })) {
    bundle.total_elapsed_ms = elapsedMilliseconds(all_started);
    return bundle;
  }

  uint64_t remaining_preload_bytes = request.small_trace_preload_budget_bytes;
  auto consumePreload = [&](uint64_t retained_bytes) {
    bundle.preloaded_trace_bytes += retained_bytes;
    remaining_preload_bytes -=
        std::min(remaining_preload_bytes, retained_bytes);
  };
  if (request.motion_timeline_enabled &&
      !loadRepository(
          &bundle, "motion", "Loading movement traces", control,
          [&](std::string *error) {
            bundle.motion = crimson::zarr::OpenMotionSeriesTimelineRepository(
                bundle.archive, request.camera_frame_count, error,
                {remaining_preload_bytes});
            if (bundle.motion) {
              consumePreload(bundle.motion->metrics().preloaded_retained_bytes);
            }
            return bundle.motion != nullptr;
          })) {
    bundle.total_elapsed_ms = elapsedMilliseconds(all_started);
    return bundle;
  }
  if (request.swim_bout_timeline_enabled &&
      !loadRepository(&bundle, "swim_bouts", "Loading swim-bout timeline",
                      control, [&](std::string *error) {
                        bundle.swim_bouts =
                            crimson::zarr::OpenSwimBoutTimelineRepository(
                                bundle.archive, request.camera_frame_count,
                                request.swim_bout_run, error);
                        return bundle.swim_bouts != nullptr;
                      })) {
    bundle.total_elapsed_ms = elapsedMilliseconds(all_started);
    return bundle;
  }
  if (request.eye_angle_timeline_enabled &&
      !loadRepository(
          &bundle, "eye_angles", "Loading eye-angle traces", control,
          [&](std::string *error) {
            bundle.eye_angles = crimson::zarr::OpenEyeAngleTimelineRepository(
                bundle.archive, {}, error, {remaining_preload_bytes});
            if (bundle.eye_angles) {
              consumePreload(
                  bundle.eye_angles->metrics().preloaded_retained_bytes);
            }
            return bundle.eye_angles != nullptr;
          })) {
    bundle.total_elapsed_ms = elapsedMilliseconds(all_started);
    return bundle;
  }
  if (request.tail_kinematics_timeline_enabled &&
      !loadRepository(
          &bundle, "tail_kinematics", "Loading tail traces", control,
          [&](std::string *error) {
            bundle.tail_kinematics =
                crimson::zarr::OpenTailKinematicsTimelineRepository(
                    bundle.archive, request.camera_frame_count, {}, error,
                    {remaining_preload_bytes});
            if (bundle.tail_kinematics) {
              consumePreload(
                  bundle.tail_kinematics->metrics().preloaded_retained_bytes);
            }
            return bundle.tail_kinematics != nullptr;
          })) {
    bundle.total_elapsed_ms = elapsedMilliseconds(all_started);
    return bundle;
  }
  if (request.stimulus_context_timeline_enabled &&
      !loadRepository(
          &bundle, "stimulus_context", "Loading stimulus timeline", control,
          [&](std::string *error) {
            bundle.stimulus_context =
                crimson::zarr::OpenStimulusContextTimelineRepository(
                    bundle.archive, request.camera_frame_count,
                    request.stimulus_run, error);
            return bundle.stimulus_context != nullptr;
          })) {
    bundle.total_elapsed_ms = elapsedMilliseconds(all_started);
    return bundle;
  }
  if (!loadRepository(&bundle, "crop_geometry", "Loading crop geometry",
                      control, [&](std::string *error) {
                        bundle.crop_geometry =
                            crimson::zarr::OpenAnalysisCropGeometryRepository(
                                bundle.archive, request.crop_run, error);
                        return bundle.crop_geometry != nullptr;
                      })) {
    bundle.total_elapsed_ms = elapsedMilliseconds(all_started);
    return bundle;
  }
  loadRepository(&bundle, "acquisition_crop", "Resolving crop media", control,
                 [&](std::string *error) {
                   bundle.acquisition_crop =
                       crimson::zarr::OpenAcquisitionCropRepository(
                           bundle.archive, error);
                   return bundle.acquisition_crop != nullptr;
                 });
  bundle.total_elapsed_ms = elapsedMilliseconds(all_started);
  return bundle;
}

} // namespace

std::string
AppleAnalysisRepositoryBundle::errorFor(const std::string &product) const {
  for (const auto &timing : timings) {
    if (timing.product == product) {
      return timing.error;
    }
  }
  return {};
}

bool AppleAnalysisRepositoryBundle::hasResultFor(
    const std::string &product) const {
  return std::any_of(timings.begin(), timings.end(), [&](const auto &timing) {
    return timing.product == product;
  });
}

AppleAnalysisRepositoryBundle OpenAppleAnalysisRepositoryBundle(
    const AppleAnalysisRepositoryLoadRequest &request) {
  return openBundle(request, {});
}

struct AppleAnalysisRepositoryLoader::Impl {
  mutable std::mutex mutex;
  std::thread coordinator;
  std::deque<AppleAnalysisRepositoryBundle> ready;
  std::shared_ptr<crimson::data::DataAccessScheduler> scheduler;
  std::vector<crimson::data::SourceIdentity> sources;
  std::atomic<bool> cancel_requested{false};
  crimson::loading::LoadingProgressTracker progress;
  size_t active_jobs = 0;
  bool submitting = false;
  bool owns_scheduler = false;
  bool running = false;
};

AppleAnalysisRepositoryLoader::AppleAnalysisRepositoryLoader()
    : impl_(std::make_unique<Impl>()) {}

AppleAnalysisRepositoryLoader::~AppleAnalysisRepositoryLoader() { close(); }

bool AppleAnalysisRepositoryLoader::start(
    AppleAnalysisRepositoryLoadRequest request, std::string *error) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->running || impl_->coordinator.joinable()) {
    if (error) {
      *error = "Analysis repository loader is already running";
    }
    return false;
  }
  impl_->cancel_requested.store(false, std::memory_order_release);
  impl_->ready.clear();
  impl_->sources.clear();
  impl_->active_jobs = 0;
  impl_->submitting = true;
  impl_->scheduler = request.scheduler;
  impl_->owns_scheduler = !impl_->scheduler;
  if (!impl_->scheduler) {
    impl_->scheduler =
        std::make_shared<crimson::data::DataAccessScheduler>(32, 3, 1, 1);
  }
  impl_->running = true;
  impl_->progress.start(productCount(request), "Starting analysis");
  impl_->coordinator = std::thread([this,
                                    request = std::move(request)]() mutable {
    const auto all_started = std::chrono::steady_clock::now();
    auto publish = [this, all_started](AppleAnalysisRepositoryBundle bundle,
                                       std::string phase) {
      bundle.total_elapsed_ms = elapsedMilliseconds(all_started);
      if (!bundle.timings.empty()) {
        const auto &timing = bundle.timings.front();
        impl_->progress.completeProduct(timing.product, phase, timing.available,
                                        timing.elapsed_ms, timing.error);
      } else {
        impl_->progress.setPhase(phase);
      }
      std::lock_guard<std::mutex> result_lock(impl_->mutex);
      impl_->ready.push_back(std::move(bundle));
    };
    auto finishJob = [this]() {
      bool finished = false;
      {
        std::lock_guard<std::mutex> result_lock(impl_->mutex);
        if (impl_->active_jobs > 0) {
          --impl_->active_jobs;
        }
        if (!impl_->submitting && impl_->active_jobs == 0) {
          impl_->running = false;
          finished = true;
        }
      }
      if (finished) {
        if (impl_->cancel_requested.load(std::memory_order_acquire)) {
          impl_->progress.cancel();
        } else {
          impl_->progress.finish("Analysis products ready");
        }
      }
    };

    AppleAnalysisRepositoryBundle archive_event;
    impl_->progress.startProduct("archive", "Resolving archive");
    const auto archive_started = std::chrono::steady_clock::now();
    archive_event.archive = crimson::zarr::ArchiveContext::Open(
        request.archive_path, &archive_event.archive_error);
    archive_event.timings.push_back(
        {"archive", elapsedMilliseconds(archive_started),
         archive_event.archive != nullptr, archive_event.archive_error});
    const auto archive = archive_event.archive;
    const std::string archive_open_error = archive_event.archive_error;
    publish(std::move(archive_event),
            archive ? "Archive ready" : "Archive unavailable");
    if (!archive || impl_->cancel_requested.load(std::memory_order_acquire)) {
      {
        std::lock_guard<std::mutex> result_lock(impl_->mutex);
        impl_->submitting = false;
        impl_->running = false;
      }
      if (impl_->cancel_requested.load(std::memory_order_acquire)) {
        impl_->progress.cancel();
      } else {
        impl_->progress.fail(archive_open_error.empty()
                                 ? "Analysis archive is unavailable"
                                 : archive_open_error,
                             "Archive unavailable");
      }
      return;
    }

    using Task =
        std::function<void(const crimson::data::DataCancellationToken &)>;
    struct Job {
      std::string name;
      std::vector<std::string> products;
      Task task;
    };
    std::vector<Job> jobs;
    auto addProduct = [&](std::string name, std::string product,
                          std::string phase, auto open) {
      jobs.push_back(
          {std::move(name),
           {product},
           [this, archive, product = std::move(product),
            phase = std::move(phase), open = std::move(open), publish](
               const crimson::data::DataCancellationToken &token) mutable {
             if (token.cancelled() ||
                 impl_->cancel_requested.load(std::memory_order_acquire)) {
               return;
             }
             impl_->progress.startProduct(product, phase);
             AppleAnalysisRepositoryBundle event;
             event.archive = archive;
             LoadControl control{&impl_->cancel_requested, {}};
             loadRepository(&event, product, phase, control,
                            [&](std::string *open_error) {
                              return open(&event, open_error);
                            });
             if (!event.cancelled) {
               publish(std::move(event), phase + " complete");
             }
           }});
    };

    addProduct(
        "chaser_polar", "chaser_polar", "Loading polar analysis",
        [archive](AppleAnalysisRepositoryBundle *event,
                  std::string *open_error) {
          event->chaser_distance_polar =
              crimson::zarr::OpenTensorStoreChaserDistancePolarRepository(
                  archive, {}, open_error);
          return event->chaser_distance_polar != nullptr;
        });
    addProduct(
        "stimulus", "stimulus", "Resolving stimulus media",
        [archive, stimulus_run = request.stimulus_run,
         video_override = request.stimulus_video_override](
            AppleAnalysisRepositoryBundle *event, std::string *open_error) {
          event->stimulus = crimson::zarr::OpenStimulusRepository(
              archive, stimulus_run, open_error, video_override);
          return event->stimulus != nullptr;
        });
    if (!request.detection_run.empty() ||
        !request.refined_detection_run.empty()) {
      addProduct(
          "canonical_detection", "canonical_detection",
          "Loading selected detections",
          [archive, detection_run = request.detection_run,
           refined_detection_run = request.refined_detection_run,
           allow_selector_ineligible =
               request.allow_selector_ineligible_refined_run](
              AppleAnalysisRepositoryBundle *event, std::string *open_error) {
            AppleAnalysisRepositoryLoadRequest detection_request;
            detection_request.detection_run = detection_run;
            detection_request.refined_detection_run = refined_detection_run;
            detection_request.allow_selector_ineligible_refined_run =
                allow_selector_ineligible;
            event->archive = archive;
            return openSelectedDetections(detection_request, event, open_error);
          });
    }
    addProduct(
        "keypoints", "keypoints", "Loading keypoint metadata",
        [archive](AppleAnalysisRepositoryBundle *event,
                  std::string *open_error) {
          event->keypoints = crimson::zarr::OpenKeypointOverlayRepository(
              archive, {}, open_error, &event->keypoint_open_metrics);
          return event->keypoints != nullptr;
        });
    if (request.subject_masks_enabled) {
      addProduct("subject_masks", "subject_masks", "Loading mask metadata",
                 [archive](AppleAnalysisRepositoryBundle *event,
                           std::string *open_error) {
                   event->subject_masks =
                       crimson::zarr::OpenSubjectMaskOverlayRepository(
                           archive, {}, open_error);
                   return event->subject_masks != nullptr;
                 });
    }
    if (request.subject_shapes_enabled) {
      addProduct("subject_shape", "subject_shape", "Loading subject shapes",
                 [archive](AppleAnalysisRepositoryBundle *event,
                           std::string *open_error) {
                   event->subject_shape =
                       crimson::zarr::OpenSubjectShapeOverlayRepository(
                           archive, {}, open_error);
                   return event->subject_shape != nullptr;
                 });
    }
    if (request.eye_geometry_enabled) {
      addProduct("eye_geometry", "eye_geometry", "Loading eye geometry",
                 [archive](AppleAnalysisRepositoryBundle *event,
                           std::string *open_error) {
                   event->eye_geometry =
                       crimson::zarr::OpenEyeGeometryOverlayRepository(
                           archive, {}, open_error);
                   return event->eye_geometry != nullptr;
                 });
    }

    const bool has_trace_job = request.motion_timeline_enabled ||
                               request.eye_angle_timeline_enabled ||
                               request.tail_kinematics_timeline_enabled;
    if (has_trace_job) {
      std::vector<std::string> products;
      if (request.motion_timeline_enabled) {
        products.push_back("motion");
      }
      if (request.eye_angle_timeline_enabled) {
        products.push_back("eye_angles");
      }
      if (request.tail_kinematics_timeline_enabled) {
        products.push_back("tail_kinematics");
      }
      jobs.push_back(
          {"small_traces", products,
           [this, archive, request, publish](
               const crimson::data::DataCancellationToken &token) mutable {
             uint64_t remaining = request.small_trace_preload_budget_bytes;
             auto openTrace = [&](const std::string &product,
                                  const std::string &phase, auto open) {
               if (token.cancelled() ||
                   impl_->cancel_requested.load(std::memory_order_acquire)) {
                 return false;
               }
               impl_->progress.startProduct(product, phase);
               AppleAnalysisRepositoryBundle event;
               event.archive = archive;
               LoadControl control{&impl_->cancel_requested, {}};
               loadRepository(&event, product, phase, control,
                              [&](std::string *open_error) {
                                return open(&event, open_error);
                              });
               if (event.cancelled) {
                 return false;
               }
               remaining -= std::min(remaining, event.preloaded_trace_bytes);
               publish(std::move(event), phase + " complete");
               return true;
             };
             if (request.motion_timeline_enabled &&
                 !openTrace(
                     "motion", "Loading movement traces",
                     [&](AppleAnalysisRepositoryBundle *event,
                         std::string *open_error) {
                       event->motion =
                           crimson::zarr::OpenMotionSeriesTimelineRepository(
                               archive, request.camera_frame_count, open_error,
                               {remaining});
                       if (event->motion) {
                         event->preloaded_trace_bytes =
                             event->motion->metrics().preloaded_retained_bytes;
                       }
                       return event->motion != nullptr;
                     })) {
               return;
             }
             if (request.eye_angle_timeline_enabled &&
                 !openTrace("eye_angles", "Loading eye-angle traces",
                            [&](AppleAnalysisRepositoryBundle *event,
                                std::string *open_error) {
                              event->eye_angles =
                                  crimson::zarr::OpenEyeAngleTimelineRepository(
                                      archive, {}, open_error, {remaining});
                              if (event->eye_angles) {
                                event->preloaded_trace_bytes =
                                    event->eye_angles->metrics()
                                        .preloaded_retained_bytes;
                              }
                              return event->eye_angles != nullptr;
                            })) {
               return;
             }
             if (request.tail_kinematics_timeline_enabled) {
               openTrace(
                   "tail_kinematics", "Loading tail traces",
                   [&](AppleAnalysisRepositoryBundle *event,
                       std::string *open_error) {
                     event->tail_kinematics =
                         crimson::zarr::OpenTailKinematicsTimelineRepository(
                             archive, request.camera_frame_count, {},
                             open_error, {remaining});
                     if (event->tail_kinematics) {
                       event->preloaded_trace_bytes =
                           event->tail_kinematics->metrics()
                               .preloaded_retained_bytes;
                     }
                     return event->tail_kinematics != nullptr;
                   });
             }
           }});
    }
    if (request.swim_bout_timeline_enabled) {
      addProduct(
          "swim_bouts", "swim_bouts", "Loading swim-bout timeline",
          [archive, frame_count = request.camera_frame_count,
           run = request.swim_bout_run](AppleAnalysisRepositoryBundle *event,
                                        std::string *open_error) {
            event->swim_bouts = crimson::zarr::OpenSwimBoutTimelineRepository(
                archive, frame_count, run, open_error);
            return event->swim_bouts != nullptr;
          });
    }
    if (request.stimulus_context_timeline_enabled) {
      addProduct(
          "stimulus_context", "stimulus_context", "Loading stimulus timeline",
          [archive, frame_count = request.camera_frame_count,
           run = request.stimulus_run](AppleAnalysisRepositoryBundle *event,
                                       std::string *open_error) {
            event->stimulus_context =
                crimson::zarr::OpenStimulusContextTimelineRepository(
                    archive, frame_count, run, open_error);
            return event->stimulus_context != nullptr;
          });
    }
    addProduct(
        "crop_geometry", "crop_geometry", "Loading crop geometry",
        [archive, run = request.crop_run](AppleAnalysisRepositoryBundle *event,
                                          std::string *open_error) {
          event->crop_geometry =
              crimson::zarr::OpenAnalysisCropGeometryRepository(archive, run,
                                                                open_error);
          return event->crop_geometry != nullptr;
        });
    addProduct("acquisition_crop", "acquisition_crop", "Resolving crop media",
               [archive](AppleAnalysisRepositoryBundle *event,
                         std::string *open_error) {
                 event->acquisition_crop =
                     crimson::zarr::OpenAcquisitionCropRepository(archive,
                                                                  open_error);
                 return event->acquisition_crop != nullptr;
               });

    auto publishRejected = [archive, publish](const Job &job,
                                              const std::string &message) {
      for (const auto &product : job.products) {
        AppleAnalysisRepositoryBundle event;
        event.archive = archive;
        event.timings.push_back({product, 0.0, false, message});
        publish(std::move(event), "Analysis task rejected");
      }
    };
    for (auto &job : jobs) {
      crimson::data::SourceIdentity source{request.archive_path,
                                           "analysis_initialization", job.name};
      crimson::data::DataRangeRequest data_request{
          source,
          {0, 0},
          crimson::data::FieldSelection::All(),
          crimson::data::RequestPriority::VisibleWindow,
          crimson::data::AccessPattern::Paused,
          1};
      {
        std::lock_guard<std::mutex> result_lock(impl_->mutex);
        impl_->sources.push_back(source);
        ++impl_->active_jobs;
      }
      auto outcome = impl_->scheduler->submit(
          std::move(data_request),
          [task = std::move(job.task), finishJob](
              const crimson::data::ScheduledDataRequest &scheduled) mutable {
            auto status = crimson::data::DataResultStatus::Ready;
            try {
              if (scheduled.cancellation.cancelled()) {
                status = crimson::data::DataResultStatus::Discarded;
              } else {
                task(scheduled.cancellation);
                if (scheduled.cancellation.cancelled()) {
                  status = crimson::data::DataResultStatus::Discarded;
                }
              }
            } catch (const std::exception &) {
              status = crimson::data::DataResultStatus::Failed;
            } catch (...) {
              status = crimson::data::DataResultStatus::Failed;
            }
            finishJob();
            return status;
          });
      if (!outcome.accepted()) {
        {
          std::lock_guard<std::mutex> result_lock(impl_->mutex);
          if (impl_->active_jobs > 0) {
            --impl_->active_jobs;
          }
        }
        publishRejected(job, "Analysis scheduler rejected the task");
      }
    }
    {
      std::lock_guard<std::mutex> result_lock(impl_->mutex);
      impl_->submitting = false;
      if (impl_->active_jobs == 0) {
        impl_->running = false;
      }
    }
    if (!loading()) {
      if (impl_->cancel_requested.load(std::memory_order_acquire)) {
        impl_->progress.cancel();
      } else {
        impl_->progress.finish("Analysis products ready");
      }
    } else {
      impl_->progress.setPhase("Loading analysis in background");
    }
  });
  if (error) {
    error->clear();
  }
  return true;
}

bool AppleAnalysisRepositoryLoader::loading() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->running;
}

AppleAnalysisRepositoryLoadProgress
AppleAnalysisRepositoryLoader::progress() const {
  return impl_->progress.snapshot();
}

std::optional<AppleAnalysisRepositoryBundle>
AppleAnalysisRepositoryLoader::takeReady() {
  std::optional<AppleAnalysisRepositoryBundle> result;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->ready.empty()) {
      return std::nullopt;
    }
    result = std::move(impl_->ready.front());
    impl_->ready.pop_front();
  }
  return result;
}

void AppleAnalysisRepositoryLoader::cancel() {
  impl_->cancel_requested.store(true, std::memory_order_release);
  if (loading()) {
    impl_->progress.cancel();
  }
}

void AppleAnalysisRepositoryLoader::close() {
  cancel();
  std::thread coordinator;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    coordinator = std::move(impl_->coordinator);
  }
  if (coordinator.joinable()) {
    coordinator.join();
  }
  std::shared_ptr<crimson::data::DataAccessScheduler> scheduler;
  std::vector<crimson::data::SourceIdentity> sources;
  bool owns_scheduler = false;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    scheduler = impl_->scheduler;
    sources = impl_->sources;
    owns_scheduler = impl_->owns_scheduler;
  }
  if (scheduler) {
    for (const auto &source : sources) {
      scheduler->cancelSource(source);
    }
    for (const auto &source : sources) {
      scheduler->waitForSourceIdle(source);
    }
    if (owns_scheduler) {
      scheduler->shutdown();
    }
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->running = false;
  impl_->submitting = false;
  impl_->active_jobs = 0;
  impl_->ready.clear();
  impl_->sources.clear();
  impl_->scheduler.reset();
}
