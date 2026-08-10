#include "frame_slot.h"
#include "platform/nvidia/nvidia_frame_buffer_adapter.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

PictureBuffer emptySlot() {
  PictureBuffer slot{};
  slot.frame_number = -1;
  slot.local_frame_number = -1;
  slot.frame_pts = -1;
  slot.available_to_write = true;
  slot.format = FramePixelFormat::RGBA8;
  return slot;
}

void publishFrame(PictureBuffer &slot, int frame_number) {
  auto lease = frameSlotAcquireWritable(slot);
  require(lease.has_value(), "a writable slot should yield a write lease");
  FrameSlotMetadata metadata;
  metadata.frame_number = frame_number;
  metadata.local_frame_number = frame_number;
  metadata.pixel_format = FramePixelFormat::RGBA8;
  lease->publish(metadata);
}

void testSnapshotsOnlyReadableSlots() {
  std::array<PictureBuffer, 4> slots = {emptySlot(), emptySlot(), emptySlot(),
                                        emptySlot()};
  for (auto &slot : slots) {
    frameSlotInitialize(slot);
  }
  publishFrame(slots[1], 101);
  publishFrame(slots[3], 103);

  const auto candidates = crimson::platform::nvidia::snapshotFrameBuffer(
      slots.data(), slots.size());
  require(candidates.size() == 2,
          "only published readable slots should be returned");
  require(candidates[0].frame_number == 101 && candidates[0].slot == 1,
          "the first readable slot should retain frame and slot identity");
  require(candidates[1].frame_number == 103 && candidates[1].slot == 3,
          "the second readable slot should retain frame and slot identity");
  require(crimson::platform::nvidia::snapshotFrameBuffer(nullptr, 4).empty(),
          "a null ring should fail closed");

  for (auto &slot : slots) {
    frameSlotDestroy(slot);
  }
}

} // namespace

int main() {
  testSnapshotsOnlyReadableSlots();
  std::cout << "nvidia_frame_buffer_adapter_tests: PASS\n";
  return 0;
}
