#pragma once

#include "platform/macos/apple_video_frame_provider.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct AppleVideoPlaybackBufferMetrics {
    uint64_t decoded_frames = 0;
    uint64_t evicted_frames = 0;
    uint64_t catchup_discarded_frames = 0;
    uint64_t catchup_seeks = 0;
    int64_t last_decoded_frame = -1;
    size_t buffered_frames = 0;
    size_t peak_buffered_frames = 0;
    double startup_ms = 0.0;
    double last_seek_ms = 0.0;
    std::string last_error;
};

class AppleVideoPlaybackBuffer {
  public:
    AppleVideoPlaybackBuffer();
    ~AppleVideoPlaybackBuffer();

    AppleVideoPlaybackBuffer(const AppleVideoPlaybackBuffer&) = delete;
    AppleVideoPlaybackBuffer& operator=(const AppleVideoPlaybackBuffer&) = delete;

    bool open(const std::string& path, const std::string& stream_id,
              size_t capacity, std::string* error = nullptr);
    void close();
    void suspend();

    bool isOpen() const;
    const AppleVideoAssetInfo& info() const;
    size_t capacity() const;

    void setTargetFrame(int64_t frame_number);
    void setPlaybackState(int64_t frame_number, bool playing,
                          double frames_per_second);
    bool requestSeek(int64_t frame_number, std::string* error = nullptr);

    std::optional<AppleDecodedVideoFrame>
    frameForTarget(int64_t frame_number, bool exact_only) const;
    bool waitForFrame(int64_t frame_number, std::chrono::milliseconds timeout);

    AppleVideoPlaybackBufferMetrics metrics() const;
    std::vector<int64_t> bufferedFrameNumbers() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
