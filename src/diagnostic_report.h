#pragma once

#include "frame_presentation.h"
#include "loading_progress.h"
#include "session_lifecycle.h"

#include <iosfwd>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace crimson::diagnostics {

struct DiagnosticRecord {
  std::string channel;
  std::vector<std::pair<std::string, std::string>> fields;
};

struct RuntimeDiagnosticsSnapshot {
  std::optional<session::SessionSnapshot> session;
  std::optional<loading::LoadingProgressSnapshot> loading;
  std::optional<playback::FramePresentationMetrics> frame_presentation;
};

std::string formatDiagnosticRecord(const DiagnosticRecord &record);
void writeDiagnosticRecord(std::ostream &output,
                           const DiagnosticRecord &record);

DiagnosticRecord sessionRecord(const std::string &channel,
                               const session::SessionSnapshot &snapshot);
DiagnosticRecord loadingRecord(const std::string &channel,
                               const loading::LoadingProgressSnapshot &snapshot);
DiagnosticRecord framePresentationRecord(
    const std::string &channel,
    const playback::FramePresentationMetrics &metrics);
std::vector<DiagnosticRecord>
runtimeRecords(const std::string &channel_prefix,
               const RuntimeDiagnosticsSnapshot &snapshot);
void writeRuntimeDiagnostics(std::ostream &output,
                             const std::string &channel_prefix,
                             const RuntimeDiagnosticsSnapshot &snapshot);

} // namespace crimson::diagnostics
