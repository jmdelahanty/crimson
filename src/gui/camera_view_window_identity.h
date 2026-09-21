#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace crimson::gui {

// Presentation identity only. Decoder/telemetry stream names must remain
// clip-specific. An empty recording identity preserves ordinary-media behavior.
inline std::string cameraViewWindowLabel(std::string_view visible_title,
                                        std::string_view recording_identity,
                                        std::string_view camera_serial,
                                        size_t camera_slot = 0) {
  std::string label(visible_title);
  if (recording_identity.empty()) {
    return label;
  }
  // Encoding prevents separators and ImGui's special ### syntax in source
  // identities from changing the scope of the hidden window ID.
  const auto append_hex = [&label](std::string_view value) {
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned char byte : value) {
      label += digits[byte >> 4];
      label += digits[byte & 15];
    }
  };
  label += "###CrimsonCameraView/";
  append_hex(recording_identity);
  if (camera_serial.empty()) {
    label += "/slot/" + std::to_string(camera_slot);
  } else {
    label += "/serial/";
    append_hex(camera_serial);
  }
  return label;
}

} // namespace crimson::gui
