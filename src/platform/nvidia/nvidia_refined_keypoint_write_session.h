#pragma once

#include "gui/crop_keypoint_editor.h"
#include "gui/full_frame_keypoint_edit_overlay.h"
#include "refined_keypoint_repository.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

class ZarrDetectionLoader;

namespace crimson::platform::nvidia {

struct RefinedKeypointWriteRequest {
  uint64_t session_generation = 0;
  std::string archive_path;
  CropKeypointEditorAction action;
  std::optional<RefinedKeypointSelection> selection;
  bool reset_crop_editor = false;
  bool reset_full_frame_editor = false;
};

struct RefinedKeypointWriteWorkerResult {
  bool ok = false;
  RefinedKeypointEditResult edit_result;
  std::string error;
};

using RefinedKeypointWriteWorker =
    std::function<RefinedKeypointWriteWorkerResult(
        const RefinedKeypointWriteRequest &)>;

enum class RefinedKeypointWriteStartStatus : uint8_t {
  Ignored,
  Accepted,
  RejectedInvalid,
  RejectedBusy,
  RejectedUnavailable,
};

struct RefinedKeypointWriteStartOutcome {
  RefinedKeypointWriteStartStatus status =
      RefinedKeypointWriteStartStatus::Ignored;
  std::string status_message;

  bool accepted() const;
};

struct RefinedKeypointWriteCompletion {
  RefinedKeypointWriteRequest request;
  RefinedKeypointWriteWorkerResult worker_result;
  bool stale_session = false;
  std::string status_message;
  double queue_wait_ms = 0.0;
  double service_ms = 0.0;
};

class RefinedKeypointWriteSession {
public:
  explicit RefinedKeypointWriteSession(RefinedKeypointWriteWorker worker);
  ~RefinedKeypointWriteSession();

  RefinedKeypointWriteSession(const RefinedKeypointWriteSession &) = delete;
  RefinedKeypointWriteSession &
  operator=(const RefinedKeypointWriteSession &) = delete;

  RefinedKeypointWriteStartOutcome start(RefinedKeypointWriteRequest request);
  std::optional<RefinedKeypointWriteCompletion>
  takeReady(uint64_t active_session_generation,
            const std::string &active_archive_path);

  bool active() const;
  void close();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

RefinedKeypointWriteWorkerResult
executeLegacyRefinedKeypointWrite(const RefinedKeypointWriteRequest &request);

bool pollAndApplyLegacyRefinedKeypointWrite(
    RefinedKeypointWriteSession &session, uint64_t active_session_generation,
    ZarrDetectionLoader &loader, CropKeypointEditorState &crop_editor_state,
    FullFrameKeypointEditState &full_frame_editor_state,
    std::string &status_out,
    const std::function<bool(std::string &)> &reload_active_zarr,
    const std::function<void()> &invalidate_after_write);

} // namespace crimson::platform::nvidia
