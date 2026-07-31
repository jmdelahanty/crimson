#pragma once

#include "playback_seek.h"

#include <chrono>
#include <cstdint>

namespace crimson::playback {

enum class PlaybackTransportCommandKind : uint8_t {
  Play,
  Pause,
  Toggle,
  Seek,
  Step,
  SetRate,
};

struct PlaybackTransportCommand {
  PlaybackTransportCommandKind kind = PlaybackTransportCommandKind::Pause;
  int64_t frame = 0;
  double rate = 1.0;

  static PlaybackTransportCommand play();
  static PlaybackTransportCommand pause();
  static PlaybackTransportCommand toggle();
  static PlaybackTransportCommand seek(int64_t frame_number);
  static PlaybackTransportCommand step(int64_t delta_frames);
  static PlaybackTransportCommand setRate(double playback_rate);
};

enum class PlaybackTransportRejection : uint8_t {
  None,
  NotConfigured,
  ControlsDisabled,
  InvalidRate,
};

struct PlaybackTransportTransition {
  bool accepted = false;
  bool state_changed = false;
  bool seek_requested = false;
  bool playing = false;
  int64_t target_frame = 0;
  PlaybackTransportRejection rejection = PlaybackTransportRejection::None;
};

struct PlaybackTransportTick {
  int64_t requested_frame = 0;
  bool playing = false;
  bool reached_end = false;
};

struct PlaybackTransportSnapshot {
  bool configured = false;
  bool controls_enabled = false;
  bool playing = false;
  double frames_per_second = 0.0;
  double playback_rate = 1.0;
  int64_t frame_count = 0;
  int64_t requested_frame = 0;
};

// Owns renderer- and decoder-neutral playback timing and transport policy.
// Platform adapters execute any decoder seek or buffer transition implied by
// an accepted command.
class PlaybackTransportController {
public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  void configure(double frames_per_second, int64_t frame_count,
                 TimePoint now = Clock::now(), int64_t initial_frame = 0,
                 bool controls_enabled = true);
  void updateTimeline(double frames_per_second, int64_t frame_count,
                      TimePoint now = Clock::now());

  bool setControlsEnabled(bool enabled, TimePoint now = Clock::now());
  PlaybackTransportTransition apply(const PlaybackTransportCommand &command,
                                    TimePoint now = Clock::now());
  PlaybackTransportTick update(TimePoint now = Clock::now());

  bool play(TimePoint now = Clock::now());
  void pause(TimePoint now = Clock::now());
  void seek(int64_t frame_number, TimePoint now = Clock::now());
  bool setPlaybackRate(double rate, TimePoint now = Clock::now());

  bool configured() const;
  bool controlsEnabled() const { return controls_enabled_; }
  bool isPlaying() const { return playing_; }
  double framesPerSecond() const { return frames_per_second_; }
  double playbackRate() const { return playback_rate_; }
  double effectiveFramesPerSecond() const {
    return frames_per_second_ * playback_rate_;
  }
  int64_t frameCount() const { return frame_count_; }
  double requestedFramePosition(TimePoint now = Clock::now()) const;
  int64_t requestedFrame(TimePoint now = Clock::now()) const;
  PlaybackTransportSnapshot snapshot(TimePoint now = Clock::now()) const;
  PlaybackSeekCoordinator &seekCoordinator() { return seek_coordinator_; }
  const PlaybackSeekCoordinator &seekCoordinator() const {
    return seek_coordinator_;
  }

private:
  int64_t clampFrame(int64_t frame_number) const;

  double frames_per_second_ = 0.0;
  double playback_rate_ = 1.0;
  int64_t frame_count_ = 0;
  int64_t anchor_frame_ = 0;
  TimePoint anchor_time_{};
  bool controls_enabled_ = false;
  bool playing_ = false;
  PlaybackSeekCoordinator seek_coordinator_;
};

} // namespace crimson::playback

// Compatibility name for platform helpers that still describe this object as
// a clock. New cross-platform ownership should use PlaybackTransportController.
using LogicalPlaybackClock = crimson::playback::PlaybackTransportController;
