#include "platform/nvidia/nvidia_detection_presentation_adapter.h"

#include <algorithm>
#include <limits>

namespace crimson::platform::nvidia {

std::vector<LoggedBoundingBox> makeLegacyBoundingBoxes(
    const crimson::zarr::DetectionRepositoryDescriptor &descriptor,
    const crimson::zarr::DetectionFrame &frame) {
  std::vector<LoggedBoundingBox> result;
  if (!frame.ready()) {
    return result;
  }
  result.reserve(frame.observations.size());
  const double fps =
      descriptor.frames_per_second > 0.0 ? descriptor.frames_per_second : 1.0;
  const int64_t timestamp_ns =
      static_cast<int64_t>(static_cast<double>(frame.frame_id) / fps * 1e9);
  for (const auto &observation : frame.observations) {
    LoggedBoundingBox box{};
    box.payload_timestamp_ns_epoch = timestamp_ns;
    box.received_timestamp_ns_epoch = timestamp_ns;
    box.payload_frame_id = frame.frame_id;
    box.payload_camera_id = 0;
    box.box_index_in_payload = static_cast<uint8_t>(std::min<size_t>(
        observation.ordinal, std::numeric_limits<uint8_t>::max()));
    box.x_min = observation.box_xyxy[0];
    box.y_min = observation.box_xyxy[1];
    box.width = observation.box_xyxy[2] - observation.box_xyxy[0];
    box.height = observation.box_xyxy[3] - observation.box_xyxy[1];
    box.class_id = static_cast<uint16_t>(observation.class_id);
    box.confidence = observation.score;
    result.push_back(box);
  }
  return result;
}

} // namespace crimson::platform::nvidia
