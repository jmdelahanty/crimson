#pragma once

#include "gui/frame_debug_window.h"

struct FrameDebugModuleCatalog {
    std::vector<ZarrDetectionLoader::ReviewArtifactSummary> review_artifacts;
};

FrameDebugModuleCatalog buildFrameDebugModuleCatalog(
    const FrameDebugWindowContext& context);

void drawFrameDebugStatusHeader(const FrameDebugWindowContext& context,
                                FrameDebugWindowState& state);

bool frameDebugModuleAvailable(
    const FrameDebugWindowContext& context,
    const FrameDebugModuleCatalog& catalog,
    crimson::workspace::FrameInspectView view);

const char* frameDebugModuleLabel(
    const FrameDebugWindowContext& context,
    crimson::workspace::FrameInspectView view);

void drawFrameDebugStatusModule(
    const FrameDebugWindowContext& context,
    const FrameDebugModuleCatalog& catalog,
    FrameDebugWindowState& state,
    FrameDebugWindowResult& result,
    crimson::workspace::FrameInspectView view);
