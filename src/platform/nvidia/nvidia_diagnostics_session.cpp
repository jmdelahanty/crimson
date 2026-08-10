#include "platform/nvidia/nvidia_diagnostics_session.h"

#include "debug_flags.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <utility>

namespace crimson::platform::nvidia::diagnostics {
namespace {

std::filesystem::path environmentPath(const char *name) {
  const char *value = std::getenv(name);
  return value != nullptr && value[0] != '\0' ? std::filesystem::path(value)
                                              : std::filesystem::path{};
}

std::string environmentString(const char *name) {
  const char *value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string{};
}

bool parseNonnegativeInt(const std::string &text, int &value) {
  if (text.empty()) {
    return false;
  }
  char *end = nullptr;
  const long parsed = std::strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != '\0' || parsed < 0 ||
      parsed > std::numeric_limits<int>::max()) {
    return false;
  }
  value = static_cast<int>(parsed);
  return true;
}

std::filesystem::path pathWithStemSuffix(const std::filesystem::path &path,
                                         const std::string &suffix) {
  return path.parent_path() /
         (path.stem().string() + suffix + path.extension().string());
}

} // namespace

NvidiaDiagnosticsEnvironment captureNvidiaDiagnosticsEnvironment() {
  NvidiaDiagnosticsEnvironment environment;
  environment.frame_sync_trace_enabled =
      crimson_env_flag_enabled("CRIMSON_FRAME_SYNC_TRACE");
  environment.frame_sync_trace_path =
      environmentPath("CRIMSON_FRAME_SYNC_TRACE_PATH");
  environment.clipped_frame_trace_enabled =
      crimson_env_flag_enabled("CRIMSON_CLIPPED_FRAME_TRACE");
  environment.clipped_frame_trace_path =
      environmentPath("CRIMSON_CLIPPED_FRAME_TRACE_PATH");
  environment.clipped_texture_dump_frame =
      environmentString("CRIMSON_CLIPPED_TEXTURE_DUMP_FRAME");
  environment.clipped_texture_dump_path =
      environmentPath("CRIMSON_CLIPPED_TEXTURE_DUMP_PATH");
  return environment;
}

NvidiaDiagnosticsConfig
resolveNvidiaDiagnosticsConfig(const NvidiaDiagnosticsOptions &options) {
  NvidiaDiagnosticsConfig config;
  config.perf_log_path = options.perf_log_path;
  config.mask_perf_sample_every = options.mask_perf_sample_every;
  if (options.mask_perf_log_enabled) {
    config.mask_perf_log_path =
        options.mask_perf_log_path.empty()
            ? defaultMaskPerfLogPath(options.default_buffer_dump_root)
            : options.mask_perf_log_path;
  }
  config.playback_trace_log_path = options.playback_trace_log_path;

  const bool frame_sync_enabled =
      options.environment.frame_sync_trace_enabled ||
      !options.environment.frame_sync_trace_path.empty() ||
      !options.frame_sync_trace_log_path.empty();
  if (frame_sync_enabled) {
    config.frame_sync_trace_log_path =
        options.default_buffer_dump_root / "frame_sync_trace_latest.jsonl";
    if (!options.environment.frame_sync_trace_path.empty()) {
      config.frame_sync_trace_log_path =
          options.environment.frame_sync_trace_path;
    }
    if (!options.frame_sync_trace_log_path.empty()) {
      config.frame_sync_trace_log_path = options.frame_sync_trace_log_path;
    }
  }

  if (!options.environment.clipped_texture_dump_frame.empty()) {
    int dump_frame = -1;
    if (!parseNonnegativeInt(options.environment.clipped_texture_dump_frame,
                             dump_frame)) {
      config.diagnostics.push_back(
          "[ClippedTextureDump] Invalid CRIMSON_CLIPPED_TEXTURE_DUMP_FRAME='" +
          options.environment.clipped_texture_dump_frame + "'");
    } else {
      config.clipped_texture_dump.enabled = true;
      config.clipped_texture_dump.parent_frame = dump_frame;
      config.clipped_texture_dump.output_path =
          options.environment.clipped_texture_dump_path.empty()
              ? options.default_buffer_dump_root /
                    ("clipped_bound_texture_parent_" +
                     std::to_string(dump_frame) + ".png")
              : options.environment.clipped_texture_dump_path;
      if (config.clipped_texture_dump.output_path.extension().empty()) {
        config.clipped_texture_dump.output_path.replace_extension(".png");
      }
    }
  }

  const bool clipped_trace_enabled =
      options.environment.clipped_frame_trace_enabled ||
      config.clipped_texture_dump.enabled ||
      !options.environment.clipped_frame_trace_path.empty();
  if (clipped_trace_enabled) {
    config.clipped_frame_trace_log_path =
        options.default_buffer_dump_root / "clipped_frame_trace_latest.jsonl";
    if (!options.environment.clipped_frame_trace_path.empty()) {
      config.clipped_frame_trace_log_path =
          options.environment.clipped_frame_trace_path;
    }
  }
  return config;
}

NvidiaDiagnosticsSession::~NvidiaDiagnosticsSession() { close(); }

void NvidiaDiagnosticsSession::open(const NvidiaDiagnosticsOptions &options,
                                    std::ostream &output, std::ostream &error) {
  close();
  config_ = resolveNvidiaDiagnosticsConfig(options);
  clipped_frame_trace_stats_ = {};
  closed_ = false;

  for (const auto &diagnostic : config_.diagnostics) {
    error << diagnostic << std::endl;
  }
  if (!config_.perf_log_path.empty()) {
    (void)perf_log_writer_.open(config_.perf_log_path);
  }
  if (!config_.mask_perf_log_path.empty()) {
    (void)mask_perf_log_writer_.open(config_.mask_perf_log_path);
    if (config_.mask_perf_sample_every > 1) {
      output << "[MaskPerfLog] Sampling every "
             << config_.mask_perf_sample_every << " frames" << std::endl;
    }
  }
  (void)openTrace(playback_trace_writer_, config_.playback_trace_log_path,
                  "PlaybackTrace", output, error);
  (void)openTrace(frame_sync_trace_writer_, config_.frame_sync_trace_log_path,
                  "FrameSyncTrace", output, error);
  if (config_.clipped_texture_dump.enabled) {
    output << "[ClippedTextureDump] Will dump parent frame "
           << config_.clipped_texture_dump.parent_frame << " to "
           << config_.clipped_texture_dump.output_path << " and "
           << pathWithStemSuffix(config_.clipped_texture_dump.output_path,
                                 "_flip_y")
           << std::endl;
  }
  (void)openTrace(clipped_frame_trace_writer_,
                  config_.clipped_frame_trace_log_path, "ClippedFrameTrace",
                  output, error);
}

void NvidiaDiagnosticsSession::close() {
  if (closed_) {
    return;
  }
  writeClippedFrameTraceSummary("shutdown");
  playback_trace_writer_.close();
  frame_sync_trace_writer_.close();
  clipped_frame_trace_writer_.close();
  perf_log_writer_.close();
  mask_perf_log_writer_.close();
  closed_ = true;
}

bool NvidiaDiagnosticsSession::playbackTraceEnabled() const {
  return playback_trace_writer_.enabled();
}

bool NvidiaDiagnosticsSession::frameSyncTraceEnabled() const {
  return frame_sync_trace_writer_.enabled();
}

bool NvidiaDiagnosticsSession::clippedFrameTraceEnabled() const {
  return clipped_frame_trace_writer_.enabled();
}

void NvidiaDiagnosticsSession::writePlaybackTrace(Json sample,
                                                  bool force_flush) {
  playback_trace_writer_.write(std::move(sample), force_flush);
}

void NvidiaDiagnosticsSession::writeFrameSyncTrace(Json sample,
                                                   bool force_flush) {
  frame_sync_trace_writer_.write(std::move(sample), force_flush);
}

void NvidiaDiagnosticsSession::writeClippedFrameTrace(Json sample,
                                                      bool force_flush) {
  clipped_frame_trace_writer_.write(std::move(sample), force_flush);
}

void NvidiaDiagnosticsSession::writeClippedFrameTraceSummary(
    const char *reason) {
  if (!clipped_frame_trace_writer_.enabled() ||
      clipped_frame_trace_stats_.frames_traced == 0) {
    return;
  }
  clipped_frame_trace_writer_.write(
      Json{{"event", "clipped_frame_trace_summary"},
           {"reason", reason != nullptr ? reason : "unknown"},
           {"summary", clipped_frame_trace_stats_.summaryJson()}},
      true);
}

PerfLogWriter &NvidiaDiagnosticsSession::perfLogWriter() {
  return perf_log_writer_;
}

MaskPerfLogWriter &NvidiaDiagnosticsSession::maskPerfLogWriter() {
  return mask_perf_log_writer_;
}

trace::ClippedFrameTraceStats &
NvidiaDiagnosticsSession::clippedFrameTraceStats() {
  return clipped_frame_trace_stats_;
}

const trace::ClippedFrameTraceStats &
NvidiaDiagnosticsSession::clippedFrameTraceStats() const {
  return clipped_frame_trace_stats_;
}

ClippedTextureDumpConfig &NvidiaDiagnosticsSession::clippedTextureDump() {
  return config_.clipped_texture_dump;
}

const ClippedTextureDumpConfig &
NvidiaDiagnosticsSession::clippedTextureDump() const {
  return config_.clipped_texture_dump;
}

const NvidiaDiagnosticsConfig &NvidiaDiagnosticsSession::config() const {
  return config_;
}

bool NvidiaDiagnosticsSession::openTrace(
    crimson::playback::diagnostics::PlaybackTraceJsonlWriter &writer,
    const std::filesystem::path &path, const char *label, std::ostream &output,
    std::ostream &error) {
  if (path.empty()) {
    return false;
  }
  const std::string log_label = label != nullptr ? label : "PlaybackTrace";
  if (writer.open(path, log_label)) {
    output << '[' << log_label << "] Writing JSONL samples to " << writer.path()
           << std::endl;
    return true;
  }
  error << '[' << log_label << "] Failed to open " << path
        << " for writing: " << writer.lastError() << std::endl;
  return false;
}

} // namespace crimson::platform::nvidia::diagnostics
