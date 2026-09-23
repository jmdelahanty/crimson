#pragma once

#include "app_update_status.h"
#include <utility>
#include <vector>
#include <optional>

struct FrameDebugWindowContext;

struct DiagnosticsRuntimeStatus {
    crimson::app::AppUpdateStatus app_update;
    std::vector<std::pair<std::string, std::string>> worker_errors;
    int swap_interval = -1;
};

struct DiagnosticsWindowResult {
    bool request_dump_decode_buffers = false;
    bool request_random_seek_dump = false;
    bool request_refresh_update_check = false;
    bool request_clear_worker_errors = false;
    std::optional<int> requested_swap_interval;
};

DiagnosticsWindowResult drawDiagnosticsWindow(
    const FrameDebugWindowContext* context,
    const DiagnosticsRuntimeStatus& runtime_status = {});
