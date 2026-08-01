#include "apple_video_playback_buffer.h"

#include "frame_selection.h"
#include "recording_clip_index.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

void assignError(std::string *destination, const std::string &value) {
  if (destination != nullptr) {
    *destination = value;
  }
}

} // namespace

struct AppleVideoPlaybackBuffer::Impl {
  mutable std::mutex mutex;
  std::condition_variable changed;
  AppleVideoFrameProvider provider;
  AppleVideoAssetInfo asset_info;
  std::optional<crimson::media::RecordingClipIndex> clip_index;
  size_t active_clip_index = std::numeric_limits<size_t>::max();
  std::string stream_id;
  std::deque<AppleDecodedVideoFrame> frames;
  std::thread worker;
  size_t queue_capacity = 0;
  int64_t target_frame = 0;
  Clock::time_point target_anchor_time = Clock::now();
  double playback_frames_per_second = 0.0;
  bool playback_running = false;
  std::optional<int64_t> requested_seek;
  uint64_t request_generation = 0;
  bool stop = false;
  bool open = false;
  bool suspended = false;
  bool end_of_stream = false;
  AppleVideoPlaybackBufferMetrics current_metrics;

  bool activateClipForParentFrame(int64_t parent_frame,
                                  int64_t *clip_local_frame, bool *switched,
                                  std::string *error) {
    if (switched != nullptr) {
      *switched = false;
    }
    if (!clip_index) {
      *clip_local_frame = parent_frame;
      return true;
    }
    const auto mapping = clip_index->resolveParentFrame(parent_frame);
    if (!mapping) {
      assignError(error, "requested parent frame is outside the clip index");
      return false;
    }
    *clip_local_frame = mapping->clip_local_frame;
    if (mapping->clip->index == active_clip_index) {
      return true;
    }
    if (!provider.open(mapping->clip->resolved_video_path.string(), stream_id,
                       error)) {
      return false;
    }
    const auto &local = provider.info();
    if (local.frame_count != mapping->clip->frame_count ||
        local.width != asset_info.width || local.height != asset_info.height ||
        std::fabs(local.nominal_frame_rate - asset_info.nominal_frame_rate) >
            0.01) {
      assignError(error, "clip media disagrees with the recording clip index");
      provider.close();
      return false;
    }
    active_clip_index = mapping->clip->index;
    if (switched != nullptr) {
      *switched = true;
    }
    return true;
  }

  bool mapDecodedFrameToParent(AppleDecodedVideoFrame *frame,
                               std::string *error) const {
    if (!clip_index) {
      return true;
    }
    const auto *clip = clip_index->clip(active_clip_index);
    if (frame == nullptr || clip == nullptr) {
      assignError(error, "clip decoder has no active clip mapping");
      return false;
    }
    const int64_t local_frame = frame->metadata.frame_number;
    if (local_frame < clip->local_start_frame ||
        local_frame > clip->local_stop_frame) {
      assignError(error, "decoded clip frame is outside its declared range");
      return false;
    }
    const int64_t parent_frame =
        clip->parent_start_frame + local_frame - clip->local_start_frame;
    frame->metadata.local_frame_number = static_cast<int>(local_frame);
    frame->metadata.frame_number = static_cast<int>(parent_frame);
    if (frame->metadata.time_base.isValid() &&
        asset_info.nominal_frame_rate > 0.0) {
      const double ticks =
          static_cast<double>(parent_frame) / asset_info.nominal_frame_rate *
          static_cast<double>(frame->metadata.time_base.denominator) /
          static_cast<double>(frame->metadata.time_base.numerator);
      frame->metadata.frame_pts = static_cast<int64_t>(std::llround(ticks));
    }
    return true;
  }

  bool hasFrameLocked(int64_t frame_number) const {
    return std::any_of(frames.begin(), frames.end(),
                       [frame_number](const AppleDecodedVideoFrame &frame) {
                         return frame.metadata.frame_number == frame_number;
                       });
  }

  int64_t currentTargetLocked(Clock::time_point now = Clock::now()) const {
    if (!playback_running || playback_frames_per_second <= 0.0) {
      return target_frame;
    }
    const double elapsed =
        std::chrono::duration<double>(now - target_anchor_time).count();
    return std::min<int64_t>(asset_info.frame_count - 1,
                             target_frame + static_cast<int64_t>(std::floor(
                                                std::max(0.0, elapsed) *
                                                playback_frames_per_second)));
  }

  void prunePresentedFramesLocked(int64_t current_target) {
    while (frames.size() > 1 &&
           frames[1].metadata.frame_number <= current_target) {
      frames.pop_front();
      ++current_metrics.evicted_frames;
    }
    current_metrics.buffered_frames = frames.size();
  }

  void workerMain() {
    while (true) {
      std::optional<int64_t> seek;
      uint64_t generation = 0;
      {
        std::unique_lock<std::mutex> lock(mutex);
        while (true) {
          if (stop) {
            return;
          }
          prunePresentedFramesLocked(currentTargetLocked());
          if (requested_seek.has_value() ||
              (!end_of_stream && frames.size() < queue_capacity)) {
            break;
          }
          if (playback_running) {
            changed.wait_for(lock, std::chrono::milliseconds(2));
          } else {
            changed.wait(lock);
          }
        }
        if (requested_seek) {
          seek = requested_seek;
          requested_seek.reset();
          generation = request_generation;
          end_of_stream = false;
        } else {
          generation = request_generation;
        }
      }

      if (seek) {
        const auto seek_start = Clock::now();
        std::string seek_error;
        int64_t local_seek = *seek;
        bool switched = false;
        const bool activated = activateClipForParentFrame(
            *seek, &local_seek, &switched, &seek_error);
        const bool positioned =
            activated && provider.seekToFrame(local_seek, &seek_error);
        const double seek_ms =
            std::chrono::duration<double, std::milli>(Clock::now() - seek_start)
                .count();
        std::lock_guard<std::mutex> lock(mutex);
        if (generation != request_generation) {
          continue;
        }
        current_metrics.last_seek_ms = seek_ms;
        if (switched) {
          ++current_metrics.clip_switches;
          current_metrics.active_clip_index =
              static_cast<int64_t>(active_clip_index);
        }
        if (!positioned) {
          current_metrics.last_error = seek_error;
          end_of_stream = true;
          changed.notify_all();
          continue;
        }
        current_metrics.last_error.clear();
      }

      std::string decode_error;
      auto frame = provider.readNext(&decode_error);
      if (frame && !mapDecodedFrameToParent(&*frame, &decode_error)) {
        frame.reset();
      }
      if (!frame && decode_error.empty() && clip_index) {
        const auto *active = clip_index->clip(active_clip_index);
        const auto *next =
            active == nullptr ? nullptr : clip_index->clip(active->index + 1);
        if (next != nullptr) {
          int64_t local_start = 0;
          bool switched = false;
          if (activateClipForParentFrame(next->parent_start_frame, &local_start,
                                         &switched, &decode_error)) {
            std::lock_guard<std::mutex> lock(mutex);
            if (generation == request_generation && switched) {
              ++current_metrics.clip_switches;
              current_metrics.active_clip_index =
                  static_cast<int64_t>(active_clip_index);
            }
            continue;
          }
        }
      }
      std::lock_guard<std::mutex> lock(mutex);
      if (generation != request_generation) {
        continue;
      }
      if (!frame) {
        if (!decode_error.empty()) {
          current_metrics.last_error = decode_error;
        }
        end_of_stream = true;
        changed.notify_all();
        continue;
      }
      ++current_metrics.decoded_frames;
      current_metrics.last_decoded_frame = frame->metadata.frame_number;
      const int64_t current_target = currentTargetLocked();
      if (frame->metadata.frame_number < current_target) {
        ++current_metrics.catchup_discarded_frames;
        const int64_t catchup_seek_threshold = std::max<int64_t>(
            1, static_cast<int64_t>(std::llround(playback_frames_per_second)));
        if (playback_running && current_target - frame->metadata.frame_number >=
                                    catchup_seek_threshold) {
          current_metrics.evicted_frames += frames.size();
          frames.clear();
          current_metrics.buffered_frames = 0;
          requested_seek = current_target;
          ++request_generation;
          ++current_metrics.catchup_seeks;
          end_of_stream = false;
        }
        changed.notify_all();
        continue;
      }
      frames.push_back(std::move(*frame));
      current_metrics.buffered_frames = frames.size();
      current_metrics.peak_buffered_frames =
          std::max(current_metrics.peak_buffered_frames, frames.size());
      changed.notify_all();
    }
  }
};

AppleVideoPlaybackBuffer::AppleVideoPlaybackBuffer()
    : impl_(std::make_unique<Impl>()) {}

AppleVideoPlaybackBuffer::~AppleVideoPlaybackBuffer() { close(); }

bool AppleVideoPlaybackBuffer::open(const std::string &path,
                                    const std::string &stream_id,
                                    size_t capacity, std::string *error) {
  close();
  if (capacity < 2) {
    assignError(error, "playback buffer capacity must be at least two");
    return false;
  }
  const auto startup = Clock::now();
  if (!impl_->provider.open(path, stream_id, error)) {
    return false;
  }
  impl_->asset_info = impl_->provider.info();
  impl_->clip_index.reset();
  impl_->active_clip_index = std::numeric_limits<size_t>::max();
  impl_->stream_id = stream_id.empty() ? "camera-main" : stream_id;
  impl_->queue_capacity = capacity;
  impl_->target_frame = 0;
  impl_->target_anchor_time = Clock::now();
  impl_->playback_frames_per_second = impl_->asset_info.nominal_frame_rate;
  impl_->playback_running = false;
  impl_->request_generation = 0;
  impl_->stop = false;
  impl_->open = true;
  impl_->suspended = false;
  impl_->end_of_stream = false;
  impl_->current_metrics = {};
  impl_->current_metrics.startup_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - startup).count();
  impl_->worker = std::thread([impl = impl_.get()] { impl->workerMain(); });
  return true;
}

bool AppleVideoPlaybackBuffer::openClipIndex(const std::string &index_path,
                                             const std::string &stream_id,
                                             size_t capacity,
                                             std::string *error) {
  close();
  if (capacity < 2) {
    assignError(error, "playback buffer capacity must be at least two");
    return false;
  }
  const auto startup = Clock::now();
  auto index = crimson::media::RecordingClipIndex::Open(index_path, error);
  if (!index || index->clips().empty()) {
    return false;
  }
  const std::string resolved_stream_id =
      stream_id.empty() ? "camera-main" : stream_id;
  const auto &first = index->clips().front();
  if (!impl_->provider.open(first.resolved_video_path.string(),
                            resolved_stream_id, error)) {
    return false;
  }
  const auto local_info = impl_->provider.info();
  if (local_info.frame_count != first.frame_count ||
      std::fabs(local_info.nominal_frame_rate - index->framesPerSecond()) >
          0.01) {
    assignError(error,
                "first clip media disagrees with the recording clip index");
    impl_->provider.close();
    return false;
  }
  impl_->asset_info = local_info;
  impl_->asset_info.path = index_path;
  impl_->asset_info.frame_count = index->totalFrameCount();
  impl_->asset_info.nominal_frame_rate = index->framesPerSecond();
  impl_->asset_info.duration_seconds =
      static_cast<double>(index->totalFrameCount()) / index->framesPerSecond();
  impl_->clip_index = std::move(*index);
  impl_->active_clip_index = 0;
  impl_->stream_id = resolved_stream_id;
  impl_->queue_capacity = capacity;
  impl_->target_frame = 0;
  impl_->target_anchor_time = Clock::now();
  impl_->playback_frames_per_second = impl_->asset_info.nominal_frame_rate;
  impl_->playback_running = false;
  impl_->request_generation = 0;
  impl_->stop = false;
  impl_->open = true;
  impl_->suspended = false;
  impl_->end_of_stream = false;
  impl_->current_metrics = {};
  impl_->current_metrics.active_clip_index = 0;
  impl_->current_metrics.startup_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - startup).count();
  impl_->worker = std::thread([impl = impl_.get()] { impl->workerMain(); });
  return true;
}

void AppleVideoPlaybackBuffer::close() {
  if (!impl_) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->stop = true;
    impl_->changed.notify_all();
  }
  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
  impl_->provider.close();
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->frames.clear();
  impl_->asset_info = {};
  impl_->clip_index.reset();
  impl_->active_clip_index = std::numeric_limits<size_t>::max();
  impl_->stream_id.clear();
  impl_->queue_capacity = 0;
  impl_->requested_seek.reset();
  impl_->open = false;
  impl_->suspended = false;
  impl_->end_of_stream = false;
  impl_->current_metrics.buffered_frames = 0;
}

void AppleVideoPlaybackBuffer::suspend() {
  if (!impl_) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->open || impl_->suspended) {
      return;
    }
    impl_->stop = true;
    impl_->changed.notify_all();
  }
  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
  impl_->provider.suspendDecoding();
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->frames.clear();
  impl_->requested_seek.reset();
  impl_->stop = false;
  impl_->suspended = true;
  impl_->end_of_stream = true;
  impl_->current_metrics.buffered_frames = 0;
}

bool AppleVideoPlaybackBuffer::isOpen() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->open;
}

const AppleVideoAssetInfo &AppleVideoPlaybackBuffer::info() const {
  return impl_->asset_info;
}

size_t AppleVideoPlaybackBuffer::capacity() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->queue_capacity;
}

void AppleVideoPlaybackBuffer::setTargetFrame(int64_t frame_number) {
  setPlaybackState(frame_number, false, 0.0);
}

void AppleVideoPlaybackBuffer::setPlaybackState(int64_t frame_number,
                                                bool playing,
                                                double frames_per_second) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->target_frame = frame_number;
  impl_->target_anchor_time = Clock::now();
  impl_->playback_running = playing;
  impl_->playback_frames_per_second = frames_per_second;
  impl_->prunePresentedFramesLocked(frame_number);
  impl_->changed.notify_all();
}

bool AppleVideoPlaybackBuffer::selectBufferedFrame(int64_t frame_number) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->open || frame_number < 0 ||
      frame_number >= impl_->asset_info.frame_count ||
      !impl_->hasFrameLocked(frame_number)) {
    return false;
  }
  impl_->target_frame = frame_number;
  impl_->target_anchor_time = Clock::now();
  impl_->playback_running = false;
  impl_->current_metrics.last_error.clear();
  ++impl_->current_metrics.buffered_target_hits;
  impl_->changed.notify_all();
  return true;
}

bool AppleVideoPlaybackBuffer::requestSeek(int64_t frame_number,
                                           std::string *error) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->open) {
    assignError(error, "playback buffer is not open");
    return false;
  }
  if (frame_number < 0 || frame_number >= impl_->asset_info.frame_count) {
    assignError(error, "requested frame is outside the video sample range");
    return false;
  }
  impl_->target_frame = frame_number;
  impl_->target_anchor_time = Clock::now();
  impl_->current_metrics.evicted_frames += impl_->frames.size();
  impl_->frames.clear();
  impl_->current_metrics.buffered_frames = 0;
  impl_->requested_seek = frame_number;
  ++impl_->request_generation;
  impl_->end_of_stream = false;
  impl_->current_metrics.last_error.clear();
  if (impl_->suspended) {
    impl_->suspended = false;
    impl_->stop = false;
    impl_->worker = std::thread([impl = impl_.get()] { impl->workerMain(); });
  }
  impl_->changed.notify_all();
  return true;
}

std::optional<AppleDecodedVideoFrame>
AppleVideoPlaybackBuffer::frameForTarget(int64_t frame_number,
                                         bool exact_only) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<BufferedFrameCandidate> candidates;
  candidates.reserve(impl_->frames.size());
  for (size_t index = 0; index < impl_->frames.size(); ++index) {
    candidates.push_back(
        {static_cast<int>(index), impl_->frames[index].metadata});
  }
  FrameSelectionRequest request;
  request.target_frame = static_cast<int>(frame_number);
  request.fallback = exact_only
                         ? FrameSelectionFallback::ExactOnly
                         : FrameSelectionFallback::LatestAtOrBeforeThenNearest;
  const FrameSelectionResult selected =
      ::selectBufferedFrame(candidates, request);
  if (selected.slot_index < 0 ||
      selected.slot_index >= static_cast<int>(impl_->frames.size())) {
    return std::nullopt;
  }
  return impl_->frames[static_cast<size_t>(selected.slot_index)];
}

bool AppleVideoPlaybackBuffer::waitForFrame(int64_t frame_number,
                                            std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  return impl_->changed.wait_for(lock, timeout, [this, frame_number] {
    return impl_->stop || impl_->hasFrameLocked(frame_number) ||
           !impl_->current_metrics.last_error.empty();
  }) && impl_->hasFrameLocked(frame_number);
}

AppleVideoPlaybackBufferMetrics AppleVideoPlaybackBuffer::metrics() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  AppleVideoPlaybackBufferMetrics result = impl_->current_metrics;
  result.buffered_frames = impl_->frames.size();
  return result;
}

std::vector<int64_t> AppleVideoPlaybackBuffer::bufferedFrameNumbers() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<int64_t> result;
  result.reserve(impl_->frames.size());
  for (const auto &frame : impl_->frames) {
    result.push_back(frame.metadata.frame_number);
  }
  return result;
}
