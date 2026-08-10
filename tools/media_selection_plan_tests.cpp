#include "media_selection_plan.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

void testMultipleVideos() {
  crimson::media::CameraMediaOpenPlan plan;
  std::string error;
  const bool ready = crimson::media::BuildCameraMediaOpenPlan(
      {{"Cam2010095_recording.mp4", "/recording/Cam2010095_recording.mp4"},
       {"folder\\Cam2010096_recording.MP4",
        "/recording/Cam2010096_recording.MP4"}},
      plan, error);
  require(ready && error.empty(), "valid camera videos should be accepted");
  require(plan.kind == crimson::media::CameraMediaKind::VideoFiles,
          "MP4 selections should produce a video plan");
  require(plan.camera_names.size() == 2 &&
              plan.camera_names[0] == "Cam2010095_recording" &&
              plan.camera_names[1] == "Cam2010096_recording",
          "camera names should preserve selection order");
  require(plan.video_paths.size() == 2,
          "every selected video path should be retained");
}

void testRectangularImageSequence() {
  crimson::media::CameraMediaOpenPlan plan;
  std::string error;
  const bool ready = crimson::media::BuildCameraMediaOpenPlan(
      {{"CamA_0001.png", "/images/CamA_0001.png"},
       {"CamA_0002.png", "/images/CamA_0002.png"},
       {"CamB_0001.png", "/images/CamB_0001.png"},
       {"CamB_0002.png", "/images/CamB_0002.png"}},
      plan, error);
  require(ready && error.empty(), "a complete image grid should be accepted");
  require(plan.kind == crimson::media::CameraMediaKind::ImageSequence,
          "image selections should produce an image-sequence plan");
  require(plan.camera_names == std::vector<std::string>({"CamA", "CamB"}),
          "image camera order should be stable");
  require(plan.image_frame_names ==
              std::vector<std::string>({"0001.png", "0002.png"}),
          "image frame names should be deduplicated in stable order");
}

void testInvalidSelectionsFailClosed() {
  crimson::media::CameraMediaOpenPlan plan;
  std::string error;
  require(!crimson::media::BuildCameraMediaOpenPlan({}, plan, error) &&
              !error.empty(),
          "an empty selection should fail");
  require(!crimson::media::BuildCameraMediaOpenPlan(
              {{"camera.avi", "/recording/camera.avi"}}, plan, error),
          "an unsupported media type should fail");
  require(!crimson::media::BuildCameraMediaOpenPlan(
              {{"camera.mp4", "/recording/camera.mp4"},
               {"camera_0001.png", "/recording/camera_0001.png"}},
              plan, error),
          "mixed videos and images should fail");
  require(!crimson::media::BuildCameraMediaOpenPlan(
              {{"0001.png", "/images/0001.png"}}, plan, error),
          "an image without a camera prefix should fail");
  require(!crimson::media::BuildCameraMediaOpenPlan(
              {{"CamA_0001.png", "/images/CamA_0001.png"},
               {"CamB_0002.png", "/images/CamB_0002.png"}},
              plan, error),
          "an incomplete image grid should fail");
  require(!crimson::media::BuildCameraMediaOpenPlan(
              {{"camera.mp4", "/one/camera.mp4"},
               {"camera.MP4", "/two/camera.MP4"}},
              plan, error),
          "duplicate video camera names should fail");
}

} // namespace

int main() {
  testMultipleVideos();
  testRectangularImageSequence();
  testInvalidSelectionsFailClosed();
  std::cout << "media_selection_plan_tests: PASS\n";
  return 0;
}
