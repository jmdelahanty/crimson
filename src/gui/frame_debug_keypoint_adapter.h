#pragma once

#include "gui/frame_debug_window.h"
#include "gui/frame_inspect_keypoint_module.h"

crimson::gui::KeypointInspectPresentation
makeFrameDebugKeypointInspectPresentation(
    const FrameDebugWindowContext &context);
