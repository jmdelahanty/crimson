#pragma once

#include "gui/frame_debug_window.h"

struct DiagnosticsWindowResult {
    bool request_dump_decode_buffers = false;
    bool request_random_seek_dump = false;
};

DiagnosticsWindowResult drawDiagnosticsWindow(
    const FrameDebugWindowContext& context);
