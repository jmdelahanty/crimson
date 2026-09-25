#include "gui/camera_view_overlay_style.h"

namespace camera_view_overlay {

ImVec4 subjectMaskComponentColor(const std::string& label) {
    if (label == "subject_body") {
        return ImVec4(0.1f, 0.85f, 0.55f, 0.18f);
    }
    if (label == "swim_bladder") {
        return ImVec4(1.0f, 0.8f, 0.18f, 0.48f);
    }
    if (label == "eye_left") {
        return ImVec4(0.2f, 0.6f, 1.0f, 0.35f);
    }
    if (label == "eye_right") {
        return ImVec4(1.0f, 0.3f, 0.6f, 0.35f);
    }
    return ImVec4(0.8f, 0.8f, 0.8f, 0.25f);
}

ImVec4 subjectMaskContourColor(const std::string& label) {
    ImVec4 color = subjectMaskComponentColor(label);
    color.w = (label == "subject_body") ? 0.75f : 0.95f;
    return color;
}

bool isEyeMaskComponent(const std::string& label) {
    return label == "eye_left" || label == "eye_right";
}

std::string shortSubjectMaskLabel(const std::string& label) {
    if (label == "subject_body") {
        return "Body";
    }
    if (label == "eye_left") {
        return "Left eye";
    }
    if (label == "eye_right") {
        return "Right eye";
    }
    if (label == "swim_bladder") {
        return "Swim bladder";
    }
    return label;
}

}  // namespace camera_view_overlay
