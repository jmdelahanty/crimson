#include "platform/nvidia/nvidia_detection_presentation_adapter.h"

#include <iostream>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__           \
                << ": " #condition << '\n';                                    \
      return false;                                                            \
    }                                                                          \
  } while (false)

bool testExactLegacyConversion() {
  crimson::zarr::DetectionRepositoryDescriptor descriptor;
  descriptor.frames_per_second = 10.0;
  crimson::zarr::DetectionFrame frame;
  frame.status = crimson::zarr::DetectionFrameStatus::Ready;
  frame.frame_id = 30;
  frame.observations.resize(2);
  frame.observations[0].ordinal = 0;
  frame.observations[0].box_xyxy = {1.0f, 2.0f, 6.0f, 10.0f};
  frame.observations[0].score = 0.75f;
  frame.observations[0].class_id = 4;
  frame.observations[1].ordinal = 300;
  frame.observations[1].box_xyxy = {4.0f, 5.0f, 7.0f, 9.0f};

  const auto boxes =
      crimson::platform::nvidia::makeLegacyBoundingBoxes(descriptor, frame);
  CHECK(boxes.size() == 2);
  CHECK(boxes[0].payload_frame_id == 30);
  CHECK(boxes[0].payload_timestamp_ns_epoch == 3000000000LL);
  CHECK(boxes[0].x_min == 1.0f);
  CHECK(boxes[0].width == 5.0f);
  CHECK(boxes[0].height == 8.0f);
  CHECK(boxes[0].confidence == 0.75f);
  CHECK(boxes[0].class_id == 4);
  CHECK(boxes[1].box_index_in_payload == 255);
  return true;
}

bool testUnavailableFrameIsEmpty() {
  crimson::zarr::DetectionRepositoryDescriptor descriptor;
  crimson::zarr::DetectionFrame frame;
  frame.observations.resize(1);
  CHECK(crimson::platform::nvidia::makeLegacyBoundingBoxes(descriptor, frame)
            .empty());
  return true;
}

} // namespace

int main() {
  if (!testExactLegacyConversion() || !testUnavailableFrameIsEmpty()) {
    return 1;
  }
  std::cout << "nvidia_detection_presentation_adapter_tests: PASS\n";
  return 0;
}
