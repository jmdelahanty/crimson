#include "playback_clock.h"

#include <algorithm>
#include <cmath>

void LogicalPlaybackClock::configure(double frames_per_second,
                                     int64_t frame_count, TimePoint now) {
    frames_per_second_ =
        std::isfinite(frames_per_second) && frames_per_second > 0.0
            ? frames_per_second
            : 0.0;
    frame_count_ = std::max<int64_t>(0, frame_count);
    anchor_frame_ = 0;
    anchor_time_ = now;
    playing_ = false;
}

void LogicalPlaybackClock::play(TimePoint now) {
    if (playing_ || frames_per_second_ <= 0.0 || frame_count_ <= 0) {
        return;
    }
    anchor_time_ = now;
    playing_ = true;
}

void LogicalPlaybackClock::pause(TimePoint now) {
    if (!playing_) {
        return;
    }
    anchor_frame_ = requestedFrame(now);
    anchor_time_ = now;
    playing_ = false;
}

void LogicalPlaybackClock::seek(int64_t frame_number, TimePoint now) {
    anchor_frame_ = clampFrame(frame_number);
    anchor_time_ = now;
}

int64_t LogicalPlaybackClock::requestedFrame(TimePoint now) const {
    if (!playing_ || frames_per_second_ <= 0.0 || frame_count_ <= 0) {
        return clampFrame(anchor_frame_);
    }
    const double elapsed =
        std::chrono::duration<double>(now - anchor_time_).count();
    const int64_t elapsed_frames = static_cast<int64_t>(
        std::floor(std::max(0.0, elapsed) * frames_per_second_));
    return clampFrame(anchor_frame_ + elapsed_frames);
}

int64_t LogicalPlaybackClock::clampFrame(int64_t frame_number) const {
    if (frame_count_ <= 0) {
        return 0;
    }
    return std::clamp<int64_t>(frame_number, 0, frame_count_ - 1);
}
