#include "gui/camera_view_transport_controls.h"
#include "gui/canonical_detection_inspect_adapter.h"
#include "gui/frame_inspect_detection_module.h"
#include "gui/frame_inspect_keypoint_module.h"
#include "gui/frame_inspect_window.h"
#include "gui/keypoint_overlay_inspect_adapter.h"
#include "gui/session_loading_modal.h"
#include "imgui.h"
#include "imgui_semantic_snapshot.h"
#include "loading_progress.h"
#include "session_readiness.h"

#include <cmath>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": "   \
                << #condition << '\n';                                         \
      return false;                                                            \
    }                                                                          \
  } while (false)

bool near(float lhs, float rhs) { return std::abs(lhs - rhs) < 0.01f; }

bool testSemanticSnapshot() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(640.0f, 480.0f);
  io.DisplayFramebufferScale = ImVec2(2.0f, 2.0f);
  io.DeltaTime = 1.0f / 60.0f;
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  crimson::ui::setSemanticCaptureEnabled(ImGui::GetCurrentContext(), true);
  crimson::ui::beginSemanticFrame(ImGui::GetCurrentContext());
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(10.0f, 20.0f));
  ImGui::SetNextWindowSize(ImVec2(300.0f, 200.0f));
  bool checked = true;
  ImGui::Begin("Fixture###fixture-window");
  ImGui::Button("Open...");
  ImGui::Checkbox("Enabled", &checked);
  ImGui::SliderInt("Width", &width, 1, 1024);
  ImGui::End();
  ImGui::Render();

  const auto snapshot =
      crimson::ui::finishSemanticFrame(ImGui::GetCurrentContext());
  CHECK(snapshot.frame >= 1);
  CHECK(near(snapshot.display_width, 640.0f));
  CHECK(near(snapshot.framebuffer_scale_x, 2.0f));
  const crimson::ui::SemanticWindow *fixture_window = nullptr;
  for (const auto &window : snapshot.windows) {
    if (window.name == "Fixture###fixture-window") {
      fixture_window = &window;
      break;
    }
  }
  if (fixture_window == nullptr) {
    for (const auto &window : snapshot.windows) {
      std::cerr << "captured window: " << window.name << '\n';
    }
  }
  CHECK(fixture_window != nullptr);
  CHECK(fixture_window->visible_name == "Fixture");
  CHECK(near(fixture_window->bounds.x, 10.0f));
  CHECK(near(fixture_window->bounds.y, 20.0f));

  bool found_open = false;
  bool found_enabled = false;
  bool found_width = false;
  int previous_order = -1;
  for (const auto &item : snapshot.items) {
    CHECK(item.submission_order > previous_order);
    previous_order = item.submission_order;
    if (item.window_name != "Fixture###fixture-window") {
      continue;
    }
    CHECK(item.window_relative_bounds.x >= 0.0f);
    CHECK(item.window_relative_bounds.y >= 0.0f);
    found_open |= item.visible_label == "Open...";
    found_enabled |= item.visible_label == "Enabled";
    found_width |= item.visible_label == "Width";
  }
  CHECK(found_open);
  CHECK(found_enabled);
  CHECK(found_width);

  const auto json = crimson::ui::semanticSnapshotJson(snapshot);
  CHECK(json.at("schema") == "crimson.imgui_semantic_snapshot.v1");
  CHECK(json.at("windows").size() >= 1);
  CHECK(json.at("items").size() >= 3);

  crimson::ui::setSemanticCaptureEnabled(ImGui::GetCurrentContext(), false);
  ImGui::DestroyContext();
  return true;
}

bool testSessionLoadingModalSnapshot() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(640.0f, 480.0f);
  io.DeltaTime = 1.0f / 60.0f;
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  crimson::loading::LoadingProgressTracker progress;
  progress.start(2, "Loading keypoints");
  progress.completeProduct("archive", "Archive ready", true, 1.0);
  progress.startProduct("keypoints", "Loading keypoints");
  const std::vector<crimson::session::SessionReadinessProductRule> rules = {
      {"archive", crimson::session::ProductAvailabilityRequirement::Required}};
  const auto readiness =
      crimson::session::evaluateSessionReadiness(progress.snapshot(), rules);
  const auto presentation = crimson::session::makeSessionLoadingPresentation(
      progress.snapshot(), readiness);

  ImGui::NewFrame();
  ImGui::Begin("Host");
  crimson::ui::drawSessionLoadingModal(presentation);
  ImGui::End();
  ImGui::Render();

  crimson::ui::setSemanticCaptureEnabled(ImGui::GetCurrentContext(), true);
  crimson::ui::beginSemanticFrame(ImGui::GetCurrentContext());
  ImGui::NewFrame();
  ImGui::Begin("Host");
  crimson::ui::drawSessionLoadingModal(presentation);
  ImGui::End();
  ImGui::Render();

  const auto snapshot =
      crimson::ui::finishSemanticFrame(ImGui::GetCurrentContext());
  bool found_modal = false;
  for (const auto &window : snapshot.windows) {
    found_modal |= window.visible_name == "Loading analysis";
  }
  if (!found_modal) {
    for (const auto &window : snapshot.windows) {
      std::cerr << "captured loading window: " << window.name << '\n';
    }
  }
  CHECK(found_modal);

  crimson::ui::setSemanticCaptureEnabled(ImGui::GetCurrentContext(), false);
  ImGui::DestroyContext();
  return true;
}

bool testCameraTransportUses64BitFrameState() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(1000.0f, 240.0f);
  io.DeltaTime = 1.0f / 60.0f;
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  constexpr int64_t kFrameCount = 5'000'000'001LL;
  constexpr int64_t kCurrentFrame = 3'500'000'000LL;
  ImGui::NewFrame();
  ImGui::SetNextWindowSize(ImVec2(960.0f, 180.0f));
  ImGui::Begin("Transport fixture");
  const auto result =
      drawCameraViewTransportControls(CameraViewTransportControlsContext{
          kCurrentFrame,
          kFrameCount,
          kFrameCount - 1,
          700.0,
          false,
          kCurrentFrame,
          true,
      });
  ImGui::End();
  ImGui::Render();

  CHECK(result.slider_frame_number == kCurrentFrame);
  CHECK(result.action == CameraViewTransportAction::None);
  CHECK(!result.intent.has_value());
  ImGui::DestroyContext();
  return true;
}

bool testFrameInspectWindowComposition() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(800.0f, 600.0f);
  io.DeltaTime = 1.0f / 60.0f;
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  using crimson::workspace::FrameInspectView;
  FrameInspectView selected = FrameInspectView::Keypoints;
  crimson::workspace::FrameInspectViewSyncState sync;
  int header_draws = 0;
  int detect_draws = 0;
  int keypoint_draws = 0;
  int hidden_draws = 0;
  int footer_draws = 0;
  crimson::gui::FrameInspectWindowComposition composition;
  composition.draw_header = [&]() {
    ++header_draws;
    ImGui::TextUnformatted("Inspection header");
  };
  composition.modules = {
      {FrameInspectView::Detect, "Detect", true,
       [&]() {
         ++detect_draws;
         ImGui::TextUnformatted("Detection module");
       }},
      {FrameInspectView::Keypoints, "Keypoints", true,
       [&]() {
         ++keypoint_draws;
         ImGui::TextUnformatted("Keypoint module");
       }},
      {FrameInspectView::EyeMasks, "Subject Masks", false,
       [&]() {
         ++hidden_draws;
         ImGui::TextUnformatted("Hidden module");
       }},
  };
  composition.draw_footer = [&]() {
    ++footer_draws;
    ImGui::TextUnformatted("Inspection footer");
  };

  ImGui::NewFrame();
  crimson::gui::drawFrameInspectWindow({}, selected, sync, composition);
  ImGui::Render();

  header_draws = 0;
  detect_draws = 0;
  keypoint_draws = 0;
  hidden_draws = 0;
  footer_draws = 0;
  ImGui::NewFrame();
  const auto result =
      crimson::gui::drawFrameInspectWindow({}, selected, sync, composition);
  ImGui::Render();

  CHECK(result.window_visible);
  CHECK(result.rendered_module);
  CHECK(result.active_module == FrameInspectView::Keypoints);
  CHECK(selected == FrameInspectView::Keypoints);
  CHECK(header_draws == 1);
  CHECK(detect_draws == 0);
  CHECK(keypoint_draws == 1);
  CHECK(hidden_draws == 0);
  CHECK(footer_draws == 1);
  CHECK(!sync.shouldApply(FrameInspectView::Keypoints));

  ImGui::DestroyContext();
  return true;
}

bool testCanonicalDetectionInspectAdapter() {
  crimson::zarr::CanonicalDetectionDescriptor descriptor;
  descriptor.surface_kind =
      crimson::zarr::DetectionSurfaceKind::RefinedSnapshotV1;
  descriptor.run_name = "refined_fixture";
  descriptor.camera_frame_count = 20;
  descriptor.offset_read_calls = 1;

  crimson::zarr::CanonicalDetectionFrame frame;
  frame.camera_frame = 7;
  crimson::zarr::CanonicalDetection raw;
  raw.row_index = 10;
  raw.instance_key = 101;
  raw.source_kind_code = 1;
  raw.score_valid = true;
  raw.score = 0.95f;
  raw.class_id = 2;
  crimson::zarr::CanonicalDetection edited = raw;
  edited.row_index = 11;
  edited.instance_key = 102;
  edited.score_valid = false;
  edited.manual_edit = true;
  edited.score = 0.0f;
  edited.class_id = 3;
  crimson::zarr::CanonicalDetection manual = raw;
  manual.row_index = 12;
  manual.instance_key = 103;
  manual.source_kind_code = 3;
  manual.score_valid = false;
  manual.score = 0.0f;
  manual.class_id = 4;
  frame.detections = {raw, edited, manual};

  const auto presentation =
      crimson::gui::makeCanonicalDetectionInspectPresentation(
          &descriptor, &frame, frame.camera_frame);
  CHECK(presentation.available);
  CHECK(presentation.frame_ready);
  CHECK(presentation.surface_label == "Refined snapshot");
  CHECK(presentation.run_name == "refined_fixture");
  CHECK(presentation.observations.size() == 3);
  CHECK(presentation.observations[0].selectable);
  CHECK(presentation.observations[0].confidence_valid);
  CHECK(presentation.observations[0].source_label == "Raw");
  CHECK(!presentation.observations[1].confidence_valid);
  CHECK(presentation.observations[1].source_label == "Edited raw");
  CHECK(presentation.observations[2].source_label == "Manual");

  const auto loading = crimson::gui::makeCanonicalDetectionInspectPresentation(
      &descriptor, &frame, frame.camera_frame + 1);
  CHECK(loading.available);
  CHECK(!loading.frame_ready);
  CHECK(loading.observations.empty());
  return true;
}

bool testDetectionInspectModuleSnapshot() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(800.0f, 600.0f);
  io.DeltaTime = 1.0f / 60.0f;
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  crimson::gui::DetectionInspectPresentation presentation;
  presentation.available = true;
  presentation.surface_label = "Refined snapshot";
  presentation.run_name = "refined_fixture";
  presentation.frame_ready = true;
  presentation.camera_frame = 7;
  presentation.timeline_visible = true;
  presentation.timeline_state =
      crimson::gui::DetectionInspectTimelineState::Ready;
  crimson::gui::DetectionInspectObservation observation;
  observation.instance_key = 101;
  observation.selectable = true;
  observation.confidence = 0.95f;
  observation.confidence_valid = true;
  observation.class_id = 2;
  observation.class_id_valid = true;
  observation.source_label = "Raw";
  presentation.observations = {observation};
  crimson::gui::DetectionInspectModuleState state;
  state.selected_instance_key = 101;

  ImGui::NewFrame();
  ImGui::Begin("Detection module fixture");
  crimson::gui::drawFrameInspectDetectionModule(presentation, state);
  ImGui::End();
  ImGui::Render();

  crimson::ui::setSemanticCaptureEnabled(ImGui::GetCurrentContext(), true);
  crimson::ui::beginSemanticFrame(ImGui::GetCurrentContext());
  ImGui::NewFrame();
  ImGui::Begin("Detection module fixture");
  const auto result =
      crimson::gui::drawFrameInspectDetectionModule(presentation, state);
  ImGui::End();
  ImGui::Render();
  const auto snapshot =
      crimson::ui::finishSemanticFrame(ImGui::GetCurrentContext());

  bool found_observation = false;
  bool found_timeline = false;
  for (const auto &item : snapshot.items) {
    found_observation |= item.visible_label == "#1";
    found_timeline |=
        item.visible_label.find("Detection Timeline") != std::string::npos;
  }
  CHECK(found_observation);
  CHECK(found_timeline);
  CHECK(!result.request_open_timeline);
  CHECK(state.selected_instance_key == 101);

  crimson::ui::setSemanticCaptureEnabled(ImGui::GetCurrentContext(), false);
  ImGui::DestroyContext();
  return true;
}

bool testKeypointOverlayInspectAdapter() {
  crimson::zarr::KeypointOverlayDescriptor descriptor;
  descriptor.run_name = "refined_keypoint_fixture";
  descriptor.refined = true;
  descriptor.camera_frame_count = 20;
  descriptor.keypoint_labels = {"snout_tip", "eye_left", "tail_tip"};

  crimson::zarr::KeypointOverlayResolution frame;
  frame.camera_frame = 7;
  crimson::zarr::KeypointOverlayDetection usable;
  usable.instance_key = 201;
  usable.refined_keypoints = true;
  usable.keypoint_usable = true;
  usable.confidence_valid = true;
  usable.pose_confidence = 0.97;
  usable.keypoint_confidences = {0.98, 0.96, 0.94};
  usable.keypoint_valid = {1, 1, 0};
  usable.keypoint_edit_flags = {0, 1, 0};
  crimson::zarr::KeypointOverlayDetection rejected = usable;
  rejected.instance_key = 202;
  rejected.keypoint_usable = false;
  rejected.confidence_valid = false;
  rejected.pose_confidence = 0.0;
  frame.detections = {usable, rejected};

  const auto presentation =
      crimson::gui::makeKeypointOverlayInspectPresentation(&descriptor, &frame,
                                                           frame.camera_frame);
  CHECK(presentation.available);
  CHECK(presentation.frame_ready);
  CHECK(presentation.surface_label == "Refined snapshot");
  CHECK(presentation.run_name == "refined_keypoint_fixture");
  CHECK(presentation.observations.size() == 2);
  CHECK(presentation.observations[0].selectable);
  CHECK(presentation.observations[0].pose_confidence_valid);
  CHECK(presentation.observations[0].valid_landmark_count == 2);
  CHECK(presentation.observations[0].landmark_count == 3);
  CHECK(presentation.observations[0].state_label == "Usable");
  CHECK(presentation.observations[0].landmarks.size() == 3);
  CHECK(presentation.observations[0].landmarks[1].edited);
  CHECK(!presentation.observations[1].pose_confidence_valid);
  CHECK(presentation.observations[1].state_label == "Rejected");

  const auto loading = crimson::gui::makeKeypointOverlayInspectPresentation(
      &descriptor, &frame, frame.camera_frame + 1);
  CHECK(loading.available);
  CHECK(!loading.frame_ready);
  CHECK(loading.observations.empty());
  return true;
}

bool testKeypointInspectModuleSnapshot() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(800.0f, 600.0f);
  io.DeltaTime = 1.0f / 60.0f;
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  crimson::gui::KeypointInspectPresentation presentation;
  presentation.available = true;
  presentation.surface_label = "Refined snapshot";
  presentation.run_name = "refined_keypoint_fixture";
  presentation.frame_ready = true;
  presentation.camera_frame = 7;
  presentation.timeline_visible = true;
  presentation.timeline_state =
      crimson::gui::KeypointInspectTimelineState::Ready;
  crimson::gui::KeypointInspectObservation observation;
  observation.instance_key = 201;
  observation.selectable = true;
  observation.pose_confidence = 0.97f;
  observation.pose_confidence_valid = true;
  observation.valid_landmark_count = 1;
  observation.landmark_count = 1;
  observation.state_label = "Usable";
  crimson::gui::KeypointInspectLandmark landmark;
  landmark.label = "snout_tip";
  landmark.confidence = 0.98f;
  landmark.confidence_valid = true;
  landmark.valid = true;
  landmark.valid_known = true;
  landmark.edited_known = true;
  observation.landmarks = {landmark};
  presentation.observations = {observation};
  crimson::gui::KeypointInspectModuleState state;
  state.selected_instance_key = 201;

  ImGui::NewFrame();
  ImGui::Begin("Keypoint module fixture");
  crimson::gui::drawFrameInspectKeypointModule(presentation, state);
  ImGui::End();
  ImGui::Render();

  crimson::ui::setSemanticCaptureEnabled(ImGui::GetCurrentContext(), true);
  crimson::ui::beginSemanticFrame(ImGui::GetCurrentContext());
  ImGui::NewFrame();
  ImGui::Begin("Keypoint module fixture");
  const auto result =
      crimson::gui::drawFrameInspectKeypointModule(presentation, state);
  ImGui::End();
  ImGui::Render();
  const auto snapshot =
      crimson::ui::finishSemanticFrame(ImGui::GetCurrentContext());

  bool found_observation = false;
  bool found_timeline = false;
  for (const auto &item : snapshot.items) {
    found_observation |= item.visible_label == "#1";
    found_timeline |= item.visible_label.find("Keypoint Quality Timeline") !=
                      std::string::npos;
  }
  CHECK(found_observation);
  CHECK(found_timeline);
  CHECK(!result.request_open_timeline);
  CHECK(state.selected_instance_key == 201);

  crimson::ui::setSemanticCaptureEnabled(ImGui::GetCurrentContext(), false);
  ImGui::DestroyContext();
  return true;
}

} // namespace

int main() {
  if (!testSemanticSnapshot() || !testSessionLoadingModalSnapshot() ||
      !testCameraTransportUses64BitFrameState() ||
      !testFrameInspectWindowComposition() ||
      !testCanonicalDetectionInspectAdapter() ||
      !testDetectionInspectModuleSnapshot() ||
      !testKeypointOverlayInspectAdapter() ||
      !testKeypointInspectModuleSnapshot()) {
    return 1;
  }
  std::cout << "imgui_semantic_snapshot_tests: PASS\n";
  return 0;
}
