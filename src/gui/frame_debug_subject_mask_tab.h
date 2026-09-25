#pragma once

#include "gui/frame_debug_window.h"

const char* frameDebugSubjectMaskTabTitle(
    const FrameDebugWindowContext& context);

void drawSubjectMaskTab(
    const FrameDebugWindowContext& context,
    FrameDebugWindowState& state,
    FrameDebugWindowResult& result,
    const ZarrDetectionLoader::ReviewArtifactSummary* artifact);
