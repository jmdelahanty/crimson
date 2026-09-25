#pragma once

#include "imgui.h"

#include <string>

namespace camera_view_overlay {

ImVec4 subjectMaskComponentColor(const std::string& label);
ImVec4 subjectMaskContourColor(const std::string& label);
bool isEyeMaskComponent(const std::string& label);
std::string shortSubjectMaskLabel(const std::string& label);

}  // namespace camera_view_overlay
