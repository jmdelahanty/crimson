#include "gui/refined_keypoint_style.h"

#include "read_only_overlay_scene.h"

#include <algorithm>

ImVec4 chooseRefinedKeypointColor(const std::string& label, size_t kp_idx) {
    const crimson::overlay::Color color =
        crimson::overlay::keypointColor(label, kp_idx);
    return ImVec4(color.red, color.green, color.blue, color.alpha);
}

ImU32 chooseRefinedKeypointColorU32(const std::string& label,
                                    size_t kp_idx,
                                    float alpha) {
    ImVec4 color = chooseRefinedKeypointColor(label, kp_idx);
    color.w = std::clamp(alpha, 0.0f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(color);
}
