#pragma once

#include <chrono>
#include <cstdint>

class LogicalPlaybackClock {
  public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    void configure(double frames_per_second, int64_t frame_count,
                   TimePoint now = Clock::now());
    void play(TimePoint now = Clock::now());
    void pause(TimePoint now = Clock::now());
    void seek(int64_t frame_number, TimePoint now = Clock::now());

    bool isPlaying() const { return playing_; }
    double framesPerSecond() const { return frames_per_second_; }
    int64_t frameCount() const { return frame_count_; }
    int64_t requestedFrame(TimePoint now = Clock::now()) const;

  private:
    int64_t clampFrame(int64_t frame_number) const;

    double frames_per_second_ = 0.0;
    int64_t frame_count_ = 0;
    int64_t anchor_frame_ = 0;
    TimePoint anchor_time_{};
    bool playing_ = false;
};
