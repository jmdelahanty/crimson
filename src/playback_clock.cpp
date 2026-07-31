#include "playback_clock.h"

#include <algorithm>
#include <cmath>

namespace crimson::playback {

PlaybackTransportCommand PlaybackTransportCommand::play() {
  return {PlaybackTransportCommandKind::Play};
}

PlaybackTransportCommand PlaybackTransportCommand::pause() {
  return {PlaybackTransportCommandKind::Pause};
}

PlaybackTransportCommand PlaybackTransportCommand::toggle() {
  return {PlaybackTransportCommandKind::Toggle};
}

PlaybackTransportCommand PlaybackTransportCommand::seek(int64_t frame_number) {
  return {PlaybackTransportCommandKind::Seek, frame_number};
}

PlaybackTransportCommand PlaybackTransportCommand::step(int64_t delta_frames) {
  return {PlaybackTransportCommandKind::Step, delta_frames};
}

PlaybackTransportCommand
PlaybackTransportCommand::setRate(double playback_rate) {
  return {PlaybackTransportCommandKind::SetRate, 0, playback_rate};
}

void PlaybackTransportController::configure(double frames_per_second,
                                            int64_t frame_count, TimePoint now,
                                            int64_t initial_frame,
                                            bool controls_enabled) {
  seek_coordinator_.reset();
  frames_per_second_ =
      std::isfinite(frames_per_second) && frames_per_second > 0.0
          ? frames_per_second
          : 0.0;
  frame_count_ = std::max<int64_t>(0, frame_count);
  playback_rate_ = 1.0;
  anchor_frame_ = clampFrame(initial_frame);
  anchor_time_ = now;
  controls_enabled_ = controls_enabled;
  playing_ = false;
}

void PlaybackTransportController::updateTimeline(double frames_per_second,
                                                 int64_t frame_count,
                                                 TimePoint now) {
  const int64_t current_frame = requestedFrame(now);
  const bool was_playing = playing_;
  frames_per_second_ =
      std::isfinite(frames_per_second) && frames_per_second > 0.0
          ? frames_per_second
          : 0.0;
  frame_count_ = std::max<int64_t>(0, frame_count);
  anchor_frame_ = clampFrame(current_frame);
  anchor_time_ = now;
  playing_ = was_playing && configured() && controls_enabled_;
}

bool PlaybackTransportController::setControlsEnabled(bool enabled,
                                                     TimePoint now) {
  if (controls_enabled_ == enabled) {
    return false;
  }
  controls_enabled_ = enabled;
  if (!enabled && playing_) {
    pause(now);
    return true;
  }
  return false;
}

PlaybackTransportTransition
PlaybackTransportController::apply(const PlaybackTransportCommand &command,
                                   TimePoint now) {
  PlaybackTransportTransition result;
  result.playing = playing_;
  result.target_frame = requestedFrame(now);
  if (!configured()) {
    result.rejection = PlaybackTransportRejection::NotConfigured;
    return result;
  }
  if (!controls_enabled_ &&
      command.kind != PlaybackTransportCommandKind::Pause) {
    result.rejection = PlaybackTransportRejection::ControlsDisabled;
    return result;
  }

  result.accepted = true;
  switch (command.kind) {
  case PlaybackTransportCommandKind::Play: {
    const bool was_playing = playing_;
    play(now);
    result.state_changed = was_playing != playing_;
    break;
  }
  case PlaybackTransportCommandKind::Pause: {
    const bool was_playing = playing_;
    pause(now);
    result.state_changed = was_playing != playing_;
    break;
  }
  case PlaybackTransportCommandKind::Toggle:
    if (playing_) {
      pause(now);
    } else {
      play(now);
    }
    result.state_changed = true;
    break;
  case PlaybackTransportCommandKind::Seek:
    result.target_frame = clampFrame(command.frame);
    result.state_changed = result.target_frame != requestedFrame(now);
    result.seek_requested = true;
    seek(result.target_frame, now);
    break;
  case PlaybackTransportCommandKind::Step:
    result.target_frame = clampFrame(requestedFrame(now) + command.frame);
    result.state_changed = result.target_frame != requestedFrame(now);
    result.seek_requested = true;
    seek(result.target_frame, now);
    break;
  case PlaybackTransportCommandKind::SetRate:
    if (!std::isfinite(command.rate) || command.rate <= 0.0) {
      result.accepted = false;
      result.rejection = PlaybackTransportRejection::InvalidRate;
      break;
    }
    result.state_changed = setPlaybackRate(command.rate, now);
    break;
  }
  result.playing = playing_;
  result.target_frame = requestedFrame(now);
  return result;
}

PlaybackTransportTick PlaybackTransportController::update(TimePoint now) {
  PlaybackTransportTick result;
  result.requested_frame = requestedFrame(now);
  if (playing_ && frame_count_ > 0 &&
      result.requested_frame >= frame_count_ - 1) {
    anchor_frame_ = frame_count_ - 1;
    anchor_time_ = now;
    playing_ = false;
    result.reached_end = true;
  }
  result.playing = playing_;
  return result;
}

bool PlaybackTransportController::play(TimePoint now) {
  if (playing_ || !configured() || !controls_enabled_) {
    return false;
  }
  anchor_time_ = now;
  playing_ = true;
  return true;
}

void PlaybackTransportController::pause(TimePoint now) {
  if (!playing_) {
    return;
  }
  anchor_frame_ = requestedFrame(now);
  anchor_time_ = now;
  playing_ = false;
}

void PlaybackTransportController::seek(int64_t frame_number, TimePoint now) {
  anchor_frame_ = clampFrame(frame_number);
  anchor_time_ = now;
}

bool PlaybackTransportController::setPlaybackRate(double rate, TimePoint now) {
  if (!std::isfinite(rate) || rate <= 0.0 || !configured()) {
    return false;
  }
  const double bounded_rate = std::clamp(rate, 0.1, 1.0);
  if (bounded_rate == playback_rate_) {
    return false;
  }
  if (playing_) {
    anchor_frame_ = requestedFrame(now);
    anchor_time_ = now;
  }
  playback_rate_ = bounded_rate;
  return true;
}

bool PlaybackTransportController::configured() const {
  return frames_per_second_ > 0.0 && frame_count_ > 0;
}

double
PlaybackTransportController::requestedFramePosition(TimePoint now) const {
  if (!playing_ || !configured()) {
    return static_cast<double>(clampFrame(anchor_frame_));
  }
  const double elapsed =
      std::chrono::duration<double>(now - anchor_time_).count();
  const double position = static_cast<double>(anchor_frame_) +
                          std::max(0.0, elapsed) * effectiveFramesPerSecond();
  return std::clamp(position, 0.0, static_cast<double>(frame_count_ - 1));
}

int64_t PlaybackTransportController::requestedFrame(TimePoint now) const {
  return clampFrame(
      static_cast<int64_t>(std::floor(requestedFramePosition(now))));
}

PlaybackTransportSnapshot
PlaybackTransportController::snapshot(TimePoint now) const {
  return {configured(),       controls_enabled_, playing_,
          frames_per_second_, playback_rate_,    frame_count_,
          requestedFrame(now)};
}

int64_t PlaybackTransportController::clampFrame(int64_t frame_number) const {
  if (frame_count_ <= 0) {
    return 0;
  }
  return std::clamp<int64_t>(frame_number, 0, frame_count_ - 1);
}

} // namespace crimson::playback
