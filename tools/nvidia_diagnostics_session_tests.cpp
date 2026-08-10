#include "platform/nvidia/nvidia_diagnostics_session.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace diagnostics = crimson::platform::nvidia::diagnostics;

namespace {

[[noreturn]] void fail(const std::string &message) {
  std::cerr << "FAIL: " << message << '\n';
  std::exit(1);
}

void require(bool condition, const std::string &message) {
  if (!condition) {
    fail(message);
  }
}

std::vector<diagnostics::Json>
readJsonLines(const std::filesystem::path &path) {
  std::ifstream input(path);
  std::vector<diagnostics::Json> rows;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty()) {
      rows.push_back(diagnostics::Json::parse(line));
    }
  }
  return rows;
}

void testConfigPrecedenceAndTextureDump() {
  diagnostics::NvidiaDiagnosticsOptions options;
  options.default_buffer_dump_root = "/tmp/crimson-diagnostics-root";
  options.mask_perf_log_enabled = true;
  options.mask_perf_sample_every = 10;
  options.playback_trace_log_path = "/cli/playback.jsonl";
  options.frame_sync_trace_log_path = "/cli/frame-sync.jsonl";
  options.environment.frame_sync_trace_enabled = true;
  options.environment.frame_sync_trace_path = "/env/frame-sync.jsonl";
  options.environment.clipped_frame_trace_enabled = true;
  options.environment.clipped_frame_trace_path = "/env/clipped.jsonl";
  options.environment.clipped_texture_dump_frame = "+42";
  options.environment.clipped_texture_dump_path = "/env/texture-dump";

  const auto config = diagnostics::resolveNvidiaDiagnosticsConfig(options);
  require(config.mask_perf_log_path ==
              "/tmp/crimson-diagnostics-root/mask_perf_latest.jsonl",
          "enabled mask logging uses the default path");
  require(config.mask_perf_sample_every == 10,
          "mask sampling cadence is retained");
  require(config.playback_trace_log_path == "/cli/playback.jsonl",
          "playback trace uses its CLI path");
  require(config.frame_sync_trace_log_path == "/cli/frame-sync.jsonl",
          "frame-sync CLI path overrides the environment path");
  require(config.clipped_frame_trace_log_path == "/env/clipped.jsonl",
          "clipped trace uses its environment path");
  require(config.clipped_texture_dump.enabled &&
              config.clipped_texture_dump.parent_frame == 42,
          "valid texture frame enables the dump");
  require(config.clipped_texture_dump.output_path == "/env/texture-dump.png",
          "texture dump receives the historical PNG extension");
  require(config.diagnostics.empty(), "valid configuration has no errors");
}

void testInvalidTextureFrameFailsClosed() {
  diagnostics::NvidiaDiagnosticsOptions options;
  options.default_buffer_dump_root = "/tmp/crimson-diagnostics-root";
  options.environment.clipped_texture_dump_frame = "4x";
  const auto config = diagnostics::resolveNvidiaDiagnosticsConfig(options);
  require(!config.clipped_texture_dump.enabled,
          "invalid texture frame does not enable a dump");
  require(config.clipped_frame_trace_log_path.empty(),
          "invalid texture frame alone does not enable clipped tracing");
  require(config.diagnostics.size() == 1 &&
              config.diagnostics.front().find("Invalid") != std::string::npos,
          "invalid texture frame produces one diagnostic");
}

void testSinkLifecycleAndShutdownSummary() {
  const auto root = std::filesystem::temp_directory_path() /
                    "crimson-nvidia-diagnostics-session-test";
  std::error_code error;
  std::filesystem::remove_all(root, error);

  diagnostics::NvidiaDiagnosticsOptions options;
  options.default_buffer_dump_root = root;
  options.perf_log_path = root / "perf.csv";
  options.mask_perf_log_enabled = true;
  options.mask_perf_log_path = root / "mask.jsonl";
  options.mask_perf_sample_every = 3;
  options.playback_trace_log_path = root / "playback.jsonl";
  options.frame_sync_trace_log_path = root / "frame-sync.jsonl";
  options.environment.clipped_frame_trace_enabled = true;
  options.environment.clipped_frame_trace_path = root / "clipped.jsonl";

  std::ostringstream output;
  std::ostringstream errors;
  diagnostics::NvidiaDiagnosticsSession session;
  session.open(options, output, errors);
  require(errors.str().empty(), "valid sinks open without errors");
  require(session.perfLogWriter().enabled(), "perf sink is owned and open");
  require(session.maskPerfLogWriter().enabled(), "mask sink is owned and open");
  require(session.playbackTraceEnabled() && session.frameSyncTraceEnabled() &&
              session.clippedFrameTraceEnabled(),
          "all JSONL sinks are owned and open");

  session.writePlaybackTrace({{"event", "playback_test"}}, true);
  session.writeFrameSyncTrace({{"event", "frame_sync_test"}}, true);
  session.writeClippedFrameTrace({{"event", "clipped_test"}}, true);
  session.clippedFrameTraceStats().frames_traced = 2;
  session.close();
  session.close();

  require(!session.perfLogWriter().enabled() &&
              !session.maskPerfLogWriter().enabled() &&
              !session.playbackTraceEnabled() &&
              !session.frameSyncTraceEnabled() &&
              !session.clippedFrameTraceEnabled(),
          "close flushes and closes every owned sink");
  require(std::filesystem::exists(root / "perf.csv") &&
              std::filesystem::exists(root / "mask.jsonl"),
          "perf and mask paths were created");

  const auto playback = readJsonLines(root / "playback.jsonl");
  const auto frame_sync = readJsonLines(root / "frame-sync.jsonl");
  const auto clipped = readJsonLines(root / "clipped.jsonl");
  require(playback.size() == 1 &&
              playback.front().at("event") == "playback_test",
          "playback sink preserves the event");
  require(frame_sync.size() == 1 &&
              frame_sync.front().at("event") == "frame_sync_test",
          "frame-sync sink preserves the event");
  require(clipped.size() == 2 &&
              clipped.front().at("event") == "clipped_test" &&
              clipped.back().at("event") == "clipped_frame_trace_summary" &&
              clipped.back().at("reason") == "shutdown" &&
              clipped.back().at("summary").at("frames_traced") == 2,
          "close emits exactly one shutdown summary after prior events");

  std::filesystem::remove_all(root, error);
}

} // namespace

int main() {
  testConfigPrecedenceAndTextureDump();
  testInvalidTextureFrameFailsClosed();
  testSinkLifecycleAndShutdownSummary();
  std::cout << "PASS: NVIDIA diagnostics session tests\n";
  return 0;
}
