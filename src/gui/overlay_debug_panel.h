#pragma once

#include "gui/frame_debug_window.h"

void drawKeypointHeadingOverlayPanel(const FrameDebugWindowContext& context,
                                     FrameDebugWindowResult& result);
void drawEyeMaskOverlayPanel(const FrameDebugWindowContext& context,
                             FrameDebugWindowResult& result);
void drawTrackKinematicsOverlayPanel(const FrameDebugWindowContext& context,
                                     FrameDebugWindowResult& result);
