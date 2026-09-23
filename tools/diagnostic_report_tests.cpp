#include "diagnostic_report.h"

#include <cstdlib>
#include <iostream>
#include <sstream>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition      \
                << '\n';                                                       \
      return false;                                                            \
    }                                                                          \
  } while (false)

bool testStableFormatting() {
  crimson::diagnostics::DiagnosticRecord record{"Test",
                                                {{"plain", "value"},
                                                 {"quoted", "two words"},
                                                 {"line", "one\ntwo"},
                                                 {"path", "a\\\"b"}}};
  CHECK(crimson::diagnostics::formatDiagnosticRecord(record) ==
        "[Test] plain=value quoted=\"two words\" line=\"one two\" "
        "path=\"a\\\\\\\"b\"");
  std::ostringstream output;
  crimson::diagnostics::writeDiagnosticRecord(output, record);
  CHECK(output.str().back() == '\n');
  return true;
}

bool testTypedRecords() {
  crimson::session::SessionLifecycle lifecycle;
  const auto generation = lifecycle.beginOpen({"camera.mp4", "data.zarr", {}});
  CHECK(lifecycle.completeOpen(generation));
  const auto session =
      crimson::diagnostics::sessionRecord("Session", lifecycle.snapshot());
  CHECK(crimson::diagnostics::formatDiagnosticRecord(session).find(
            "state=ready") != std::string::npos);

  const auto clipped_generation =
      lifecycle.beginOpen({{}, "clips.zarr", {}, "recording_clip_index.json"});
  CHECK(lifecycle.completeOpen(clipped_generation));
  const auto clipped =
      crimson::diagnostics::sessionRecord("Session", lifecycle.snapshot());
  CHECK(crimson::diagnostics::formatDiagnosticRecord(clipped).find(
            "clip_index=recording_clip_index.json") != std::string::npos);

  crimson::playback::FramePresentationTracker tracker;
  tracker.record(5, 5, false);
  const auto frame = crimson::diagnostics::framePresentationRecord(
      "Frames", tracker.metrics());
  CHECK(crimson::diagnostics::formatDiagnosticRecord(frame).find(
            "presentations=1") != std::string::npos);
  const auto records = crimson::diagnostics::runtimeRecords(
      "Apple", {lifecycle.snapshot(), std::nullopt, tracker.metrics()});
  CHECK(records.size() == 2);
  CHECK(records[0].channel == "AppleSession");
  CHECK(records[1].channel == "AppleFramePresentation");
  return true;
}

} // namespace

int main() {
  if (!testStableFormatting() || !testTypedRecords()) {
    return EXIT_FAILURE;
  }
  std::cout << "diagnostic_report_tests: PASS\n";
  return EXIT_SUCCESS;
}
