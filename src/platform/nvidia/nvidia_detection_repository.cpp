#include "platform/nvidia/nvidia_detection_repository.h"

#include "coordinate_contract.h"
#include "zarr/archive_context.h"

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace crimson::platform::nvidia {
namespace {

using Clock = std::chrono::steady_clock;

void assignError(std::string *destination, const std::string &value) {
  if (destination) {
    *destination = value;
  }
}

double elapsedMilliseconds(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}

bool validOpenRequest(const NvidiaDetectionOpenRequest &request,
                      bool require_storage_identity, std::string *error) {
  if (require_storage_identity && request.archive_path.empty()) {
    assignError(error, "Canonical detection archive path is required");
    return false;
  }
  if (require_storage_identity && request.canonical_raw_run.empty()) {
    assignError(error, "Exact canonical raw detection run is required");
    return false;
  }
  if (!std::isfinite(request.frames_per_second) ||
      request.frames_per_second < 0.0) {
    assignError(error, "Canonical detection frame rate is invalid");
    return false;
  }
  if (request.page_frames == 0 || request.cache_pages < 2) {
    assignError(error, "Canonical detection page/cache policy is invalid");
    return false;
  }
  return true;
}

std::unique_ptr<crimson::zarr::CanonicalDetectionRepository>
openProductionStorage(
    const NvidiaDetectionOpenRequest &request,
    crimson::zarr::DetectionRepositorySelectionMetrics *metrics,
    std::string *error) {
  auto archive =
      crimson::zarr::ArchiveContext::Open(request.archive_path, error);
  if (!archive) {
    return nullptr;
  }
  crimson::zarr::DetectionRepositorySelectionRequest selection;
  selection.canonical_raw_run = request.canonical_raw_run;
  selection.raw_fallback_policy = crimson::zarr::DetectionRawFallbackPolicy::
      AllowOnlyWhenNoRefinedAuthority;
  return crimson::zarr::OpenSelectedDetectionRepository(
      archive, selection, error, metrics);
}

bool validateCanonicalRepository(
    const crimson::zarr::CanonicalDetectionRepository &repository,
    const NvidiaDetectionOpenRequest &request, std::string *error) {
  const auto &descriptor = repository.descriptor();
  if (!descriptor.ready()) {
    assignError(error, "Canonical detection repository is not ready");
    return false;
  }
  if (descriptor.surface_kind !=
      crimson::zarr::DetectionSurfaceKind::CanonicalRawV1) {
    assignError(error,
                "Selected repository is not the requested canonical raw run");
    return false;
  }
  if (!descriptor.hasValidatedInstanceKeys()) {
    assignError(error,
                "Canonical raw detection observation identity is unavailable");
    return false;
  }
  if (!request.canonical_raw_run.empty() &&
      descriptor.run_name != request.canonical_raw_run) {
    assignError(error,
                "Canonical detection repository returned a different run");
    return false;
  }
  if (descriptor.source_width == 0 || descriptor.source_height == 0 ||
      descriptor.source_width >
          static_cast<size_t>(std::numeric_limits<int>::max()) ||
      descriptor.source_height >
          static_cast<size_t>(std::numeric_limits<int>::max())) {
    assignError(error, "Canonical detection source dimensions are invalid");
    return false;
  }
  if (request.expected_camera_frame_count != 0 &&
      descriptor.camera_frame_count != request.expected_camera_frame_count) {
    assignError(error,
                "Canonical detection frame count does not match indexed video");
    return false;
  }
  if ((request.expected_source_width != 0 &&
       descriptor.source_width != request.expected_source_width) ||
      (request.expected_source_height != 0 &&
       descriptor.source_height != request.expected_source_height)) {
    assignError(
        error,
        "Canonical detection dimensions do not match the presented video");
    return false;
  }
  if (!request.expected_recording_identity.empty() &&
      descriptor.recording_identity != request.expected_recording_identity) {
    assignError(error,
                "Canonical detection recording identity does not match the "
                "presented video");
    return false;
  }
  return true;
}

crimson::zarr::DetectionRepositoryDescriptor makeDescriptor(
    const crimson::zarr::CanonicalDetectionDescriptor &canonical,
    const NvidiaDetectionOpenRequest &request, bool available) {
  crimson::zarr::DetectionRepositoryDescriptor result;
  result.archive_path = request.archive_path;
  result.run_name = canonical.run_name;
  result.total_frames = canonical.camera_frame_count;
  result.frames_per_second = request.frames_per_second;
  result.source_width = static_cast<int>(canonical.source_width);
  result.source_height = static_cast<int>(canonical.source_height);
  result.active_dataset = crimson::zarr::DetectionDataset::RawDetect;
  result.available = available;
  result.has_scores = true;
  result.has_class_ids = true;
  // DetectionRepository observations exposed by this bridge are converted to
  // full-frame continuous pixel xyxy, matching the NVIDIA legacy contract.
  result.coordinates_normalized = false;
  result.interpolation_available = false;
  result.clipped_collection = false;
  result.active_dataset_has_synthetic_observations = false;
  result.has_validated_instance_keys = canonical.hasValidatedInstanceKeys();
  return result;
}

} // namespace

struct NvidiaDetectionRepository::Impl {
  explicit Impl(std::shared_ptr<crimson::data::DataAccessScheduler> scheduler,
                NvidiaDetectionStorageOpener opener)
      : scheduler(std::move(scheduler)),
        storage_opener(opener ? std::move(opener)
                              : NvidiaDetectionStorageOpener(
                                    openProductionStorage)) {}

  mutable std::mutex lifecycle_mutex;
  mutable std::mutex mutex;
  mutable std::condition_variable condition;
  std::shared_ptr<crimson::data::DataAccessScheduler> scheduler;
  std::shared_ptr<CanonicalDetectionBuffer> buffer;
  NvidiaDetectionStorageOpener storage_opener;
  std::thread open_worker;
  std::atomic<bool> cancel_requested{false};
  NvidiaDetectionOpenRequest request;
  crimson::zarr::CanonicalDetectionDescriptor canonical_descriptor;
  crimson::zarr::DetectionRepositoryDescriptor detection_descriptor;
  NvidiaDetectionRepositoryMetrics observed;

  void markFailedLocked(const NvidiaDetectionOpenRequest &failed_request,
                        std::string error) {
    request = failed_request;
    canonical_descriptor = {};
    detection_descriptor = {};
    observed.state = NvidiaDetectionState::Failed;
    observed.archive_path = failed_request.archive_path;
    observed.canonical_raw_run = failed_request.canonical_raw_run;
    observed.last_error = std::move(error);
    ++observed.failed_opens;
    condition.notify_all();
  }

  void closeWithLifecycleLock() {
    cancel_requested.store(true, std::memory_order_release);
    std::shared_ptr<CanonicalDetectionBuffer> retiring_buffer;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (observed.state == NvidiaDetectionState::Opening) {
        ++observed.cancelled_opens;
      }
      // Invalidate the public surface before waiting for storage or range work.
      // In-flight resolves retain their buffer safely but fail their final
      // generation/source check.
      ++observed.generation;
      observed.state = NvidiaDetectionState::Closed;
      retiring_buffer = std::move(buffer);
      request = {};
      canonical_descriptor = {};
      detection_descriptor = {};
      observed.last_error.clear();
      condition.notify_all();
    }
    if (retiring_buffer) {
      const auto buffer_metrics = retiring_buffer->metrics();
      const auto repository_metrics = retiring_buffer->repositoryMetrics();
      retiring_buffer->close();
      std::lock_guard<std::mutex> lock(mutex);
      observed.buffer = buffer_metrics;
      observed.repository = repository_metrics;
    }
    if (open_worker.joinable()) {
      open_worker.join();
    }
  }

  bool adopt(
      std::unique_ptr<crimson::zarr::CanonicalDetectionRepository> repository,
      const NvidiaDetectionOpenRequest &adopt_request, uint64_t generation,
      std::string *error) {
    if (!repository) {
      assignError(error, "Canonical detection repository is unavailable");
      return false;
    }
    if (!validateCanonicalRepository(*repository, adopt_request, error)) {
      return false;
    }
    const auto descriptor = repository->descriptor();
    auto candidate = std::make_shared<CanonicalDetectionBuffer>(
        scheduler, adopt_request.archive_path.empty()
                       ? std::string("in_memory_nvidia_detection")
                       : adopt_request.archive_path);
    if (!candidate->open(std::move(repository), adopt_request.page_frames,
                         adopt_request.cache_pages, error)) {
      return false;
    }
    if (cancel_requested.load(std::memory_order_acquire)) {
      candidate->close();
      assignError(error, "Canonical detection open was cancelled");
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (generation != observed.generation ||
        observed.state != NvidiaDetectionState::Opening) {
      candidate->close();
      assignError(error, "Canonical detection open result is stale");
      return false;
    }
    request = adopt_request;
    canonical_descriptor = descriptor;
    detection_descriptor = makeDescriptor(descriptor, adopt_request, true);
    buffer = std::move(candidate);
    observed.state = NvidiaDetectionState::Ready;
    observed.archive_path = adopt_request.archive_path;
    observed.canonical_raw_run = descriptor.run_name;
    observed.last_error.clear();
    ++observed.successful_opens;
    condition.notify_all();
    return true;
  }
};

NvidiaDetectionRepository::NvidiaDetectionRepository(
    std::shared_ptr<crimson::data::DataAccessScheduler> scheduler,
    NvidiaDetectionStorageOpener storage_opener)
    : impl_(std::make_unique<Impl>(std::move(scheduler),
                                  std::move(storage_opener))) {}

NvidiaDetectionRepository::~NvidiaDetectionRepository() { close(); }

bool NvidiaDetectionRepository::beginOpen(NvidiaDetectionOpenRequest request,
                                          std::string *error) {
  std::lock_guard<std::mutex> lifecycle_lock(impl_->lifecycle_mutex);
  impl_->closeWithLifecycleLock();

  std::string validation_error;
  if (!validOpenRequest(request, true, &validation_error) ||
      !impl_->scheduler || !impl_->scheduler->running() ||
      !impl_->storage_opener) {
    if (validation_error.empty()) {
      validation_error = !impl_->scheduler || !impl_->scheduler->running()
                             ? "Canonical detection scheduler is unavailable"
                             : "Canonical detection storage opener is unavailable";
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    ++impl_->observed.open_attempts;
    ++impl_->observed.generation;
    impl_->markFailedLocked(request, validation_error);
    assignError(error, validation_error);
    return false;
  }

  uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->cancel_requested.store(false, std::memory_order_release);
    impl_->request = request;
    impl_->canonical_descriptor = {};
    impl_->detection_descriptor = {};
    impl_->observed.state = NvidiaDetectionState::Opening;
    impl_->observed.archive_path = request.archive_path;
    impl_->observed.canonical_raw_run = request.canonical_raw_run;
    impl_->observed.last_error.clear();
    impl_->observed.selection = {};
    ++impl_->observed.open_attempts;
    generation = ++impl_->observed.generation;
  }

  try {
    impl_->open_worker = std::thread([this, request = std::move(request),
                                      generation] {
      const auto started = Clock::now();
      crimson::zarr::DetectionRepositorySelectionMetrics selection;
      std::string open_error;
      try {
        auto repository =
            impl_->storage_opener(request, &selection, &open_error);
        const double elapsed_ms = elapsedMilliseconds(started);

        {
          std::lock_guard<std::mutex> lock(impl_->mutex);
          if (generation == impl_->observed.generation) {
            impl_->observed.selection = selection;
            impl_->observed.last_open_ms = elapsed_ms;
          }
        }
        if (impl_->cancel_requested.load(std::memory_order_acquire)) {
          return;
        }
        if (!repository) {
          if (open_error.empty()) {
            open_error = "Canonical detection storage open failed";
          }
          std::lock_guard<std::mutex> lock(impl_->mutex);
          if (generation == impl_->observed.generation &&
              impl_->observed.state == NvidiaDetectionState::Opening) {
            impl_->markFailedLocked(request, open_error);
          }
          return;
        }
        if (!impl_->adopt(std::move(repository), request, generation,
                          &open_error)) {
          if (impl_->cancel_requested.load(std::memory_order_acquire)) {
            return;
          }
          if (open_error.empty()) {
            open_error = "Canonical detection repository adoption failed";
          }
          std::lock_guard<std::mutex> lock(impl_->mutex);
          if (generation == impl_->observed.generation &&
              impl_->observed.state == NvidiaDetectionState::Opening) {
            impl_->markFailedLocked(request, open_error);
          }
        }
      } catch (const std::exception &exception) {
        open_error = "Canonical detection open threw: " +
                     std::string(exception.what());
      } catch (...) {
        open_error = "Canonical detection open threw an unknown exception";
      }
      if (!open_error.empty() &&
          !impl_->cancel_requested.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (generation == impl_->observed.generation &&
            impl_->observed.state == NvidiaDetectionState::Opening) {
          impl_->observed.selection = selection;
          impl_->observed.last_open_ms = elapsedMilliseconds(started);
          impl_->markFailedLocked(request, open_error);
        }
      }
    });
  } catch (const std::exception &exception) {
    const std::string start_error =
        "Could not start canonical detection open worker: " +
        std::string(exception.what());
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->markFailedLocked(request, start_error);
    assignError(error, start_error);
    return false;
  }
  return true;
}

bool NvidiaDetectionRepository::openForTesting(
    std::unique_ptr<crimson::zarr::CanonicalDetectionRepository> repository,
    NvidiaDetectionOpenRequest request, std::string *error) {
  std::lock_guard<std::mutex> lifecycle_lock(impl_->lifecycle_mutex);
  impl_->closeWithLifecycleLock();

  std::string validation_error;
  if (!validOpenRequest(request, false, &validation_error) ||
      !impl_->scheduler || !impl_->scheduler->running()) {
    if (validation_error.empty()) {
      validation_error = "Canonical detection scheduler is unavailable";
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    ++impl_->observed.open_attempts;
    ++impl_->observed.generation;
    impl_->markFailedLocked(request, validation_error);
    assignError(error, validation_error);
    return false;
  }

  uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->cancel_requested.store(false, std::memory_order_release);
    impl_->observed.state = NvidiaDetectionState::Opening;
    impl_->observed.archive_path = request.archive_path;
    impl_->observed.canonical_raw_run = request.canonical_raw_run;
    impl_->observed.last_error.clear();
    ++impl_->observed.open_attempts;
    generation = ++impl_->observed.generation;
  }
  std::string adopt_error;
  if (!impl_->adopt(std::move(repository), request, generation, &adopt_error)) {
    if (adopt_error.empty()) {
      adopt_error = "Canonical detection test repository adoption failed";
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->markFailedLocked(request, adopt_error);
    assignError(error, adopt_error);
    return false;
  }
  return true;
}

void NvidiaDetectionRepository::close() {
  std::lock_guard<std::mutex> lifecycle_lock(impl_->lifecycle_mutex);
  impl_->closeWithLifecycleLock();
}

NvidiaDetectionState NvidiaDetectionRepository::state() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->observed.state;
}

bool NvidiaDetectionRepository::waitUntilOpen(
    std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  return impl_->condition.wait_for(lock, timeout, [&] {
    return impl_->observed.state != NvidiaDetectionState::Opening;
  });
}

bool NvidiaDetectionRepository::requestPresentedFrame(int64_t camera_frame,
                                                      bool discontinuity,
                                                      std::string *error) {
  std::shared_ptr<CanonicalDetectionBuffer> buffer;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    ++impl_->observed.frame_requests;
    if (impl_->observed.state != NvidiaDetectionState::Ready) {
      ++impl_->observed.rejected_frame_requests;
      assignError(error, "Canonical detection repository is not ready");
      return false;
    }
    buffer = impl_->buffer;
  }
  if (!buffer || !buffer->requestFrame(camera_frame, discontinuity, error)) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    ++impl_->observed.rejected_frame_requests;
    return false;
  }
  return true;
}

crimson::zarr::DetectionRepositoryDescriptor
NvidiaDetectionRepository::descriptor() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->detection_descriptor;
}

std::vector<crimson::zarr::DetectionDatasetOption>
NvidiaDetectionRepository::availableDatasets() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->observed.state != NvidiaDetectionState::Ready) {
    return {};
  }
  return {{crimson::zarr::DetectionDataset::RawDetect, "Raw detections"}};
}

bool NvidiaDetectionRepository::selectDataset(
    crimson::zarr::DetectionDataset dataset) {
  return dataset == crimson::zarr::DetectionDataset::RawDetect &&
         isDatasetAvailable(dataset);
}

bool NvidiaDetectionRepository::isDatasetAvailable(
    crimson::zarr::DetectionDataset dataset) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->observed.state == NvidiaDetectionState::Ready &&
         dataset == crimson::zarr::DetectionDataset::RawDetect;
}

size_t NvidiaDetectionRepository::observationCount(size_t frame_id) const {
  const auto frame = resolveFrame(frame_id, false);
  return frame.ready() ? frame.observations.size() : 0;
}

bool NvidiaDetectionRepository::isFrameInterpolated(size_t) const {
  return false;
}

crimson::zarr::DetectionFrame NvidiaDetectionRepository::resolveFrame(
    size_t frame_id, bool) const {
  crimson::zarr::DetectionFrame result;
  result.frame_id = frame_id;

  crimson::zarr::DetectionRepositoryDescriptor descriptor_snapshot;
  std::shared_ptr<CanonicalDetectionBuffer> buffer;
  uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->observed.state != NvidiaDetectionState::Ready) {
      ++impl_->observed.cache_misses;
      return result;
    }
    descriptor_snapshot = impl_->detection_descriptor;
    buffer = impl_->buffer;
    generation = impl_->observed.generation;
    if (frame_id >= descriptor_snapshot.total_frames ||
        frame_id > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
      result.status = crimson::zarr::DetectionFrameStatus::OutOfRange;
      return result;
    }
  }

  const auto canonical =
      buffer ? buffer->frame(static_cast<int64_t>(frame_id)) : nullptr;
  if (!canonical) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->observed.state != NvidiaDetectionState::Ready ||
        impl_->observed.generation != generation ||
        impl_->buffer != buffer) {
      ++impl_->observed.stale_frames_discarded;
    } else {
      ++impl_->observed.cache_misses;
    }
    return result;
  }
  if (canonical->camera_frame != static_cast<int64_t>(frame_id)) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    ++impl_->observed.stale_frames_discarded;
    return result;
  }

  crimson::coordinates::TransformAuthority authority;
  authority.source_dimensions = {descriptor_snapshot.source_width,
                                 descriptor_snapshot.source_height};
  result.observations.reserve(canonical->detections.size());
  for (size_t index = 0; index < canonical->detections.size(); ++index) {
    const auto &detection = canonical->detections[index];
    if (!detection.instance_key_valid) {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      if (impl_->observed.state == NvidiaDetectionState::Ready &&
          impl_->observed.generation == generation &&
          impl_->buffer == buffer) {
        ++impl_->observed.invalid_geometry_frames;
        impl_->observed.last_error =
            "Canonical detection is missing validated observation identity";
      } else {
        ++impl_->observed.stale_frames_discarded;
      }
      result.observations.clear();
      return result;
    }
    const auto &box = detection.normalized_cxcywh;
    const auto pixels =
        crimson::coordinates::normalizedCenterSizeBoxToContinuousPixelXyxy(
            {box[0], box[1], box[2], box[3]}, authority);
    if (!pixels) {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      if (impl_->observed.state == NvidiaDetectionState::Ready &&
          impl_->observed.generation == generation &&
          impl_->buffer == buffer) {
        ++impl_->observed.invalid_geometry_frames;
        impl_->observed.last_error =
            "Canonical detection contains invalid normalized geometry";
      } else {
        ++impl_->observed.stale_frames_discarded;
      }
      result.observations.clear();
      return result;
    }
    crimson::zarr::DetectionObservation observation;
    observation.ordinal = index;
    observation.canonical_row_index = detection.row_index;
    observation.instance_key = detection.instance_key;
    observation.instance_key_valid = detection.instance_key_valid;
    observation.box_xyxy = {static_cast<float>(pixels->x_min),
                            static_cast<float>(pixels->y_min),
                            static_cast<float>(pixels->x_max),
                            static_cast<float>(pixels->y_max)};
    observation.score = detection.score;
    observation.class_id = detection.class_id;
    observation.source_kind = detection.source_kind_code;
    observation.score_valid = detection.score_valid;
    observation.class_id_valid = true;
    result.observations.push_back(std::move(observation));
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->observed.state != NvidiaDetectionState::Ready ||
        impl_->observed.generation != generation ||
        impl_->buffer != buffer) {
      ++impl_->observed.stale_frames_discarded;
      result = {};
      result.frame_id = frame_id;
      return result;
    }
    ++impl_->observed.cache_resolves;
  }
  result.status = crimson::zarr::DetectionFrameStatus::Ready;
  result.interpolated = false;
  return result;
}

NvidiaDetectionRepositoryMetrics NvidiaDetectionRepository::metrics() const {
  NvidiaDetectionRepositoryMetrics result;
  std::shared_ptr<CanonicalDetectionBuffer> buffer;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    result = impl_->observed;
    buffer = impl_->buffer;
  }
  if (buffer) {
    result.buffer = buffer->metrics();
    result.repository = buffer->repositoryMetrics();
  }
  if (impl_->scheduler) {
    result.scheduler = impl_->scheduler->metrics();
  }
  return result;
}

const char *nvidiaDetectionStateName(NvidiaDetectionState state) {
  switch (state) {
  case NvidiaDetectionState::Closed:
    return "closed";
  case NvidiaDetectionState::Opening:
    return "opening";
  case NvidiaDetectionState::Ready:
    return "ready";
  case NvidiaDetectionState::Failed:
    return "failed";
  }
  return "unknown";
}

} // namespace crimson::platform::nvidia
