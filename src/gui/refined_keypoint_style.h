#pragma once

#include "imgui.h"

#include <cstddef>
#include <string>

ImVec4 chooseRefinedKeypointColor(const std::string& label, size_t kp_idx);

ImU32 chooseRefinedKeypointColorU32(const std::string& label,
                                    size_t kp_idx,
                                    float alpha = 1.0f);
