#include "ui_reference_capture.h"

#include <cmath>
#include <fstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

namespace crimson::ui_reference {
namespace {

bool removeOutput(const std::filesystem::path &path, std::string *error) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (!ec) {
    return true;
  }
  if (error != nullptr) {
    *error = "failed to clear " + path.string() + ": " + ec.message();
  }
  return false;
}

} // namespace

const char *capturePhaseName(CapturePhase phase) {
  switch (phase) {
  case CapturePhase::Disabled:
    return "disabled";
  case CapturePhase::WaitingForStableFrame:
    return "waiting_for_stable_frame";
  case CapturePhase::CaptureRequested:
    return "capture_requested";
  case CapturePhase::CaptureComplete:
    return "capture_complete";
  case CapturePhase::Published:
    return "published";
  case CapturePhase::TimedOut:
    return "timed_out";
  case CapturePhase::Failed:
    return "failed";
  }
  return "unknown";
}

bool CaptureSnapshot::active() const {
  return phase == CapturePhase::WaitingForStableFrame ||
         phase == CapturePhase::CaptureRequested ||
         phase == CapturePhase::CaptureComplete;
}

bool CaptureSnapshot::terminal() const {
  return phase == CapturePhase::Published || phase == CapturePhase::TimedOut ||
         phase == CapturePhase::Failed;
}

CaptureCoordinator::CaptureCoordinator(CapturePolicy policy)
    : policy_(policy) {}

bool CaptureCoordinator::start(TimePoint now) {
  if (phase_ != CapturePhase::Disabled) {
    return false;
  }
  if (policy_.required_stable_frames < 1 ||
      !std::isfinite(policy_.timeout_seconds) ||
      policy_.timeout_seconds <= 0.0) {
    fail("invalid UI-reference capture policy");
    return false;
  }
  started_at_ = now;
  started_ = true;
  stable_frame_count_ = 0;
  failure_reason_.clear();
  phase_ = CapturePhase::WaitingForStableFrame;
  return true;
}

bool CaptureCoordinator::observeFrame(bool exact_frame_presented,
                                      bool state_ready, TimePoint now) {
  if (pollTimeout(now) || phase_ != CapturePhase::WaitingForStableFrame) {
    return false;
  }
  if (exact_frame_presented && state_ready) {
    ++stable_frame_count_;
  } else {
    stable_frame_count_ = 0;
  }
  if (stable_frame_count_ < policy_.required_stable_frames) {
    return false;
  }
  phase_ = CapturePhase::CaptureRequested;
  return true;
}

bool CaptureCoordinator::pollTimeout(TimePoint now) {
  if (!activePhase() || elapsedSeconds(now) <= policy_.timeout_seconds) {
    return false;
  }
  phase_ = CapturePhase::TimedOut;
  failure_reason_ = "UI-reference capture timed out";
  return true;
}

bool CaptureCoordinator::markCaptureComplete() {
  if (phase_ != CapturePhase::CaptureRequested) {
    return false;
  }
  phase_ = CapturePhase::CaptureComplete;
  return true;
}

bool CaptureCoordinator::markPublished() {
  if (phase_ != CapturePhase::CaptureComplete) {
    return false;
  }
  phase_ = CapturePhase::Published;
  return true;
}

void CaptureCoordinator::fail(std::string reason) {
  if (terminal()) {
    return;
  }
  failure_reason_ = std::move(reason);
  phase_ = CapturePhase::Failed;
}

const CapturePolicy &CaptureCoordinator::policy() const { return policy_; }

CaptureSnapshot CaptureCoordinator::snapshot(TimePoint now) const {
  return CaptureSnapshot{phase_, stable_frame_count_, elapsedSeconds(now),
                         failure_reason_};
}

CapturePhase CaptureCoordinator::phase() const { return phase_; }

int CaptureCoordinator::stableFrameCount() const { return stable_frame_count_; }

bool CaptureCoordinator::waitingForStableFrame() const {
  return phase_ == CapturePhase::WaitingForStableFrame;
}

bool CaptureCoordinator::captureRequested() const {
  return phase_ == CapturePhase::CaptureRequested;
}

bool CaptureCoordinator::readyToPublish() const {
  return phase_ == CapturePhase::CaptureComplete;
}

bool CaptureCoordinator::published() const {
  return phase_ == CapturePhase::Published;
}

bool CaptureCoordinator::terminal() const {
  return phase_ == CapturePhase::Published ||
         phase_ == CapturePhase::TimedOut || phase_ == CapturePhase::Failed;
}

bool CaptureCoordinator::activePhase() const {
  return phase_ == CapturePhase::WaitingForStableFrame ||
         phase_ == CapturePhase::CaptureRequested ||
         phase_ == CapturePhase::CaptureComplete;
}

double CaptureCoordinator::elapsedSeconds(TimePoint now) const {
  if (!started_) {
    return 0.0;
  }
  return std::chrono::duration<double>(now - started_at_).count();
}

std::filesystem::path
uiReferenceImagePath(const std::filesystem::path &marker_path) {
  std::filesystem::path image_path = marker_path;
  image_path += ".png";
  return image_path;
}

bool prepareUiReferenceOutput(const std::filesystem::path &marker_path,
                              std::string *error) {
  if (marker_path.empty()) {
    if (error != nullptr) {
      *error = "output path is empty";
    }
    return false;
  }
  std::filesystem::path temporary_path = marker_path;
  temporary_path += ".tmp";
  return removeOutput(marker_path, error) &&
         removeOutput(temporary_path, error) &&
         removeOutput(uiReferenceImagePath(marker_path), error);
}

bool writeUiReferenceMarkerAtomically(const std::filesystem::path &marker_path,
                                      const nlohmann::json &contents,
                                      std::string *error) {
  if (marker_path.empty()) {
    if (error != nullptr) {
      *error = "output path is empty";
    }
    return false;
  }
  std::error_code ec;
  if (marker_path.has_parent_path()) {
    std::filesystem::create_directories(marker_path.parent_path(), ec);
    if (ec) {
      if (error != nullptr) {
        *error = "failed to create parent directory: " + ec.message();
      }
      return false;
    }
  }

  std::filesystem::path temporary_path = marker_path;
  temporary_path += ".tmp";
  if (!removeOutput(temporary_path, error)) {
    return false;
  }
  {
    std::ofstream stream(temporary_path, std::ios::out | std::ios::trunc);
    if (!stream.is_open()) {
      if (error != nullptr) {
        *error = "failed to open temporary marker";
      }
      return false;
    }
    stream << contents.dump(2) << '\n';
    stream.flush();
    if (!stream.good()) {
      if (error != nullptr) {
        *error = "failed to write temporary marker";
      }
      return false;
    }
  }
  std::filesystem::remove(marker_path, ec);
  ec.clear();
  std::filesystem::rename(temporary_path, marker_path, ec);
  if (!ec) {
    return true;
  }
  if (error != nullptr) {
    *error = "failed to publish marker: " + ec.message();
  }
  std::filesystem::remove(temporary_path, ec);
  return false;
}

} // namespace crimson::ui_reference
