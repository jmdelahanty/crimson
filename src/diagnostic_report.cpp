#include "diagnostic_report.h"

#include <iomanip>
#include <ostream>
#include <sstream>

namespace crimson::diagnostics {

namespace {

std::string sanitize(std::string value) {
  for (char &character : value) {
    if (character == '\n' || character == '\r' || character == '\t') {
      character = ' ';
    }
  }
  if (value.find_first_of(" =\"") == std::string::npos) {
    return value;
  }
  std::string quoted = "\"";
  for (const char character : value) {
    if (character == '\\' || character == '\"') {
      quoted.push_back('\\');
    }
    quoted.push_back(character);
  }
  quoted.push_back('\"');
  return quoted;
}

std::string number(double value) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(1) << value;
  return stream.str();
}

} // namespace

std::string formatDiagnosticRecord(const DiagnosticRecord &record) {
  std::ostringstream stream;
  stream << '[' << (record.channel.empty() ? "Crimson" : record.channel) << ']';
  for (const auto &[key, value] : record.fields) {
    stream << ' ' << key << '=' << sanitize(value);
  }
  return stream.str();
}

void writeDiagnosticRecord(std::ostream &output,
                           const DiagnosticRecord &record) {
  output << formatDiagnosticRecord(record) << '\n';
}

DiagnosticRecord sessionRecord(const std::string &channel,
                               const session::SessionSnapshot &snapshot) {
  DiagnosticRecord record{channel};
  record.fields = {
      {"state", session::sessionPhaseName(snapshot.phase)},
      {"generation", std::to_string(snapshot.generation)},
      {"video", snapshot.active.video_path},
      {"zarr", snapshot.active.zarr_path},
      {"stimulus", snapshot.active.stimulus_video_path},
      {"error", snapshot.error},
  };
  return record;
}

DiagnosticRecord loadingRecord(
    const std::string &channel,
    const loading::LoadingProgressSnapshot &snapshot) {
  DiagnosticRecord record{channel};
  record.fields = {
      {"state", loading::loadingStateName(snapshot.state)},
      {"phase", snapshot.phase},
      {"product", snapshot.product},
      {"completed", std::to_string(snapshot.completed_products)},
      {"total", std::to_string(snapshot.total_products)},
      {"error", snapshot.error},
  };
  return record;
}

DiagnosticRecord framePresentationRecord(
    const std::string &channel,
    const playback::FramePresentationMetrics &metrics) {
  DiagnosticRecord record{channel};
  record.fields = {
      {"presentations", std::to_string(metrics.presentation_count)},
      {"exact", std::to_string(metrics.exact_presentations)},
      {"repeated", std::to_string(metrics.repeated_presentations)},
      {"skipped", std::to_string(metrics.skipped_source_frames)},
      {"late", std::to_string(metrics.late_presentations)},
      {"discontinuities", std::to_string(metrics.discontinuities)},
      {"requested", std::to_string(metrics.last_requested_frame)},
      {"presented", std::to_string(metrics.last_presented_frame)},
      {"max_lag_frames", number(metrics.max_lag_frames)},
  };
  return record;
}

std::vector<DiagnosticRecord>
runtimeRecords(const std::string &channel_prefix,
               const RuntimeDiagnosticsSnapshot &snapshot) {
  std::vector<DiagnosticRecord> records;
  if (snapshot.session) {
    records.push_back(
        sessionRecord(channel_prefix + "Session", *snapshot.session));
  }
  if (snapshot.loading) {
    records.push_back(
        loadingRecord(channel_prefix + "Loading", *snapshot.loading));
  }
  if (snapshot.frame_presentation) {
    records.push_back(framePresentationRecord(
        channel_prefix + "FramePresentation", *snapshot.frame_presentation));
  }
  return records;
}

void writeRuntimeDiagnostics(std::ostream &output,
                             const std::string &channel_prefix,
                             const RuntimeDiagnosticsSnapshot &snapshot) {
  for (const auto &record : runtimeRecords(channel_prefix, snapshot)) {
    writeDiagnosticRecord(output, record);
  }
}

} // namespace crimson::diagnostics
