#include "gui/refined_keypoint_style.h"

#include <algorithm>
#include <cctype>

namespace {

std::string toLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

}  // namespace

ImVec4 chooseRefinedKeypointColor(const std::string& label, size_t kp_idx) {
    const std::string lowered_label = toLowerCopy(label);
    if (lowered_label.find("swim") != std::string::npos ||
        lowered_label.find("bladder") != std::string::npos) {
        return ImVec4(1.0f, 0.85f, 0.15f, 1.0f);
    }
    if (lowered_label.find("left") != std::string::npos) {
        return ImVec4(0.3f, 0.95f, 0.4f, 1.0f);
    }
    if (lowered_label.find("right") != std::string::npos) {
        return ImVec4(0.75f, 0.4f, 0.95f, 1.0f);
    }
    static const ImVec4 kFallbackColors[] = {
        ImVec4(0.95f, 0.6f, 0.2f, 1.0f),
        ImVec4(0.35f, 0.85f, 0.55f, 1.0f),
        ImVec4(0.6f, 0.5f, 0.95f, 1.0f),
        ImVec4(0.95f, 0.4f, 0.4f, 1.0f),
        ImVec4(0.4f, 0.75f, 0.95f, 1.0f),
    };
    return kFallbackColors[kp_idx %
                            (sizeof(kFallbackColors) /
                             sizeof(kFallbackColors[0]))];
}

ImU32 chooseRefinedKeypointColorU32(const std::string& label,
                                    size_t kp_idx,
                                    float alpha) {
    ImVec4 color = chooseRefinedKeypointColor(label, kp_idx);
    color.w = std::clamp(alpha, 0.0f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(color);
}
