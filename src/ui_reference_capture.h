#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace crimson::ui_reference {

enum class CapturePhase : uint8_t {
  Disabled,
  WaitingForStableFrame,
  CaptureRequested,
  CaptureComplete,
  Published,
  TimedOut,
  Failed,
};

const char *capturePhaseName(CapturePhase phase);

struct CapturePolicy {
  int required_stable_frames = 60;
  double timeout_seconds = 60.0;
};

struct CaptureSnapshot {
  CapturePhase phase = CapturePhase::Disabled;
  int stable_frame_count = 0;
  double elapsed_seconds = 0.0;
  std::string failure_reason;

  bool active() const;
  bool terminal() const;
};

class CaptureCoordinator {
public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  explicit CaptureCoordinator(CapturePolicy policy = {});

  bool start(TimePoint now = Clock::now());
  bool observeFrame(bool exact_frame_presented, bool state_ready,
                    TimePoint now = Clock::now());
  bool pollTimeout(TimePoint now = Clock::now());
  bool markCaptureComplete();
  bool markPublished();
  void fail(std::string reason);

  const CapturePolicy &policy() const;
  CaptureSnapshot snapshot(TimePoint now = Clock::now()) const;
  CapturePhase phase() const;
  int stableFrameCount() const;
  bool waitingForStableFrame() const;
  bool captureRequested() const;
  bool readyToPublish() const;
  bool published() const;
  bool terminal() const;

private:
  bool activePhase() const;
  double elapsedSeconds(TimePoint now) const;

  CapturePolicy policy_;
  CapturePhase phase_ = CapturePhase::Disabled;
  TimePoint started_at_{};
  bool started_ = false;
  int stable_frame_count_ = 0;
  std::string failure_reason_;
};

std::filesystem::path
uiReferenceImagePath(const std::filesystem::path &marker_path);

bool prepareUiReferenceOutput(const std::filesystem::path &marker_path,
                              std::string *error);

bool writeUiReferenceMarkerAtomically(const std::filesystem::path &marker_path,
                                      const nlohmann::json &contents,
                                      std::string *error);

} // namespace crimson::ui_reference
