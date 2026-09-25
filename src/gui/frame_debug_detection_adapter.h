#pragma once

#include "gui/frame_debug_window.h"
#include "gui/frame_inspect_detection_module.h"

crimson::gui::DetectionInspectPresentation
makeFrameDebugDetectionInspectPresentation(
    const FrameDebugWindowContext &context);
