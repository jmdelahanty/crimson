#pragma once

#include "perf_log_writer.h"
#include "platform/nvidia/nvidia_playback_trace_model.h"
#include "playback_diagnostics.h"

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace crimson::platform::nvidia::diagnostics {

using Json = nlohmann::json;

struct NvidiaDiagnosticsEnvironment {
  bool frame_sync_trace_enabled = false;
  std::filesystem::path frame_sync_trace_path;
  bool clipped_frame_trace_enabled = false;
  std::filesystem::path clipped_frame_trace_path;
  std::string clipped_texture_dump_frame;
  std::filesystem::path clipped_texture_dump_path;
};

struct NvidiaDiagnosticsOptions {
  std::filesystem::path default_buffer_dump_root;
  std::filesystem::path perf_log_path;
  bool mask_perf_log_enabled = false;
  std::filesystem::path mask_perf_log_path;
  int mask_perf_sample_every = 1;
  std::filesystem::path playback_trace_log_path;
  std::filesystem::path frame_sync_trace_log_path;
  NvidiaDiagnosticsEnvironment environment;
};

struct ClippedTextureDumpConfig {
  bool enabled = false;
  int parent_frame = -1;
  std::filesystem::path output_path;
  bool dumped = false;
};

struct NvidiaDiagnosticsConfig {
  std::filesystem::path perf_log_path;
  std::filesystem::path mask_perf_log_path;
  int mask_perf_sample_every = 1;
  std::filesystem::path playback_trace_log_path;
  std::filesystem::path frame_sync_trace_log_path;
  std::filesystem::path clipped_frame_trace_log_path;
  ClippedTextureDumpConfig clipped_texture_dump;
  std::vector<std::string> diagnostics;
};

NvidiaDiagnosticsEnvironment captureNvidiaDiagnosticsEnvironment();

NvidiaDiagnosticsConfig
resolveNvidiaDiagnosticsConfig(const NvidiaDiagnosticsOptions &options);

class NvidiaDiagnosticsSession {
public:
  NvidiaDiagnosticsSession() = default;
  ~NvidiaDiagnosticsSession();

  NvidiaDiagnosticsSession(const NvidiaDiagnosticsSession &) = delete;
  NvidiaDiagnosticsSession &
  operator=(const NvidiaDiagnosticsSession &) = delete;

  void open(const NvidiaDiagnosticsOptions &options, std::ostream &output,
            std::ostream &error);
  void close();

  bool playbackTraceEnabled() const;
  bool frameSyncTraceEnabled() const;
  bool clippedFrameTraceEnabled() const;

  void writePlaybackTrace(Json sample, bool force_flush = false);
  void writeFrameSyncTrace(Json sample, bool force_flush = false);
  void writeClippedFrameTrace(Json sample, bool force_flush = false);
  void writeClippedFrameTraceSummary(const char *reason);

  PerfLogWriter &perfLogWriter();
  MaskPerfLogWriter &maskPerfLogWriter();
  trace::ClippedFrameTraceStats &clippedFrameTraceStats();
  const trace::ClippedFrameTraceStats &clippedFrameTraceStats() const;
  ClippedTextureDumpConfig &clippedTextureDump();
  const ClippedTextureDumpConfig &clippedTextureDump() const;
  const NvidiaDiagnosticsConfig &config() const;

private:
  static bool
  openTrace(crimson::playback::diagnostics::PlaybackTraceJsonlWriter &writer,
            const std::filesystem::path &path, const char *label,
            std::ostream &output, std::ostream &error);

  NvidiaDiagnosticsConfig config_;
  PerfLogWriter perf_log_writer_;
  MaskPerfLogWriter mask_perf_log_writer_;
  crimson::playback::diagnostics::PlaybackTraceJsonlWriter
      playback_trace_writer_;
  crimson::playback::diagnostics::PlaybackTraceJsonlWriter
      frame_sync_trace_writer_;
  crimson::playback::diagnostics::PlaybackTraceJsonlWriter
      clipped_frame_trace_writer_;
  trace::ClippedFrameTraceStats clipped_frame_trace_stats_;
  bool closed_ = true;
};

} // namespace crimson::platform::nvidia::diagnostics
