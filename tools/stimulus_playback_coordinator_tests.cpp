#include "stimulus_playback_coordinator.h"

#include <iostream>
#include <memory>

namespace {

#define CHECK(condition)                                                      \
  do {                                                                        \
    if (!(condition)) {                                                       \
      std::cerr << "CHECK failed: " #condition << " at " << __FILE__ << ':' \
                << __LINE__ << '\n';                                         \
      return false;                                                           \
    }                                                                         \
  } while (false)

using crimson::playback::StimulusDecodeAction;
using crimson::playback::StimulusDecoderSnapshot;
using crimson::playback::StimulusPlaybackCoordinator;
using crimson::zarr::StimulusAlignmentData;
using crimson::zarr::StimulusMappingStatus;

std::unique_ptr<crimson::zarr::StimulusRepository> MakeFixtureRepository() {
  StimulusAlignmentData alignment;
  alignment.run_name = "playback_policy_fixture";
  alignment.camera_to_stimulus_frame_corrected = {-1, 0, 2, 3, 3, 7, -1, 10};
  alignment.camera_stimulus_frame_interpolated = {0, 0, 1, 0, 0, 0, 0, 0};
  alignment.alignment_available = true;
  alignment.direct_corrected_available = true;
  return crimson::zarr::MakeStimulusRepository(std::move(alignment));
}

bool RunTest() {
  auto repository = MakeFixtureRepository();
  CHECK(repository != nullptr);
  StimulusPlaybackCoordinator coordinator(*repository);

  StimulusDecoderSnapshot decoder;
  decoder.max_forward_decode_frames = 4;

  auto missing = coordinator.requestCameraFrame(0, false, decoder);
  CHECK(missing.resolution.status == StimulusMappingStatus::Missing);
  CHECK(missing.action == StimulusDecodeAction::Clear);
  CHECK(!missing.target_frame);

  auto first = coordinator.requestCameraFrame(1, false, decoder);
  CHECK(first.action == StimulusDecodeAction::Seek);
  CHECK(first.target_frame == 0);

  decoder.last_decoded_frame = 0;
  auto follow = coordinator.requestCameraFrame(2, false, decoder);
  CHECK(follow.action == StimulusDecodeAction::Follow);
  CHECK(follow.target_frame == 2);
  CHECK(follow.resolution.interpolated);

  decoder.last_decoded_frame = 3;
  decoder.target_buffered = true;
  auto hold = coordinator.requestCameraFrame(3, false, decoder);
  CHECK(hold.action == StimulusDecodeAction::Hold);
  CHECK(hold.target_frame == 3);

  decoder.target_buffered = false;
  decoder.last_decoded_frame = 4;
  auto evicted_repeat = coordinator.requestCameraFrame(4, false, decoder);
  CHECK(evicted_repeat.action == StimulusDecodeAction::Seek);
  CHECK(evicted_repeat.target_frame == 3);

  decoder.last_decoded_frame = 3;
  auto large_jump = coordinator.requestCameraFrame(5, false, decoder);
  CHECK(large_jump.action == StimulusDecodeAction::Follow);
  CHECK(large_jump.target_frame == 7);

  decoder.max_forward_decode_frames = 3;
  decoder.last_decoded_frame = 2;
  auto bounded_jump = coordinator.requestCameraFrame(5, false, decoder);
  CHECK(bounded_jump.action == StimulusDecodeAction::Seek);

  decoder.last_decoded_frame = 7;
  auto backwards = coordinator.requestCameraFrame(2, false, decoder);
  CHECK(backwards.action == StimulusDecodeAction::Seek);

  decoder.target_buffered = true;
  auto buffered_discontinuity =
      coordinator.requestCameraFrame(2, true, decoder);
  CHECK(buffered_discontinuity.action == StimulusDecodeAction::Hold);

  decoder.target_buffered = false;
  auto gap = coordinator.requestCameraFrame(6, false, decoder);
  CHECK(gap.action == StimulusDecodeAction::Clear);
  CHECK(!coordinator.lastRequestedStimulusFrame());

  decoder.last_decoded_frame = 9;
  auto after_gap = coordinator.requestCameraFrame(7, false, decoder);
  CHECK(after_gap.action == StimulusDecodeAction::Seek);
  CHECK(after_gap.target_frame == 10);

  auto out_of_range = coordinator.requestCameraFrame(8, false, decoder);
  CHECK(out_of_range.resolution.status ==
        StimulusMappingStatus::OutOfRange);
  CHECK(out_of_range.action == StimulusDecodeAction::Clear);

  coordinator.reset();
  CHECK(!coordinator.lastRequestedStimulusFrame());
  std::cout << "stimulus_playback_coordinator_tests: PASS\n";
  return true;
}

}  // namespace

int main() { return RunTest() ? 0 : 1; }
