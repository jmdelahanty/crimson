#pragma once

#include "gui/frame_debug_window.h"
#include "gui/frame_inspect_subject_mask_module.h"

crimson::gui::SubjectMaskInspectPresentation
makeFrameDebugSubjectMaskInspectPresentation(
    const FrameDebugWindowContext &context);
