#include "gui/camera_view_transport_controls.h"
#include "gui/canonical_detection_inspect_adapter.h"
#include "gui/eye_geometry_overlay_controls.h"
#include "gui/eye_geometry_overlay_inspect_adapter.h"
#include "gui/frame_inspect_detection_module.h"
#include "gui/frame_inspect_eye_angle_module.h"
#include "gui/frame_inspect_keypoint_module.h"
#include "gui/frame_inspect_subject_mask_module.h"
#include "gui/frame_inspect_subject_shape_module.h"
#include "gui/frame_inspect_window.h"
#include "gui/keypoint_overlay_inspect_adapter.h"
#include "gui/read_only_eye_geometry_controls_adapter.h"
#include "gui/read_only_subject_shape_controls_adapter.h"
#include "gui/session_loading_modal.h"
#include "gui/subject_mask_overlay_inspect_adapter.h"
#include "gui/subject_shape_overlay_controls.h"
#include "gui/subject_shape_overlay_inspect_adapter.h"
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

bool testSubjectMaskOverlayInspectAdapter() {
  crimson::zarr::SubjectMaskOverlayDescriptor descriptor;
  descriptor.source_group = "refined_subject_masks_runs";
  descriptor.run_name = "refined_subject_mask_fixture";
  descriptor.source_crop_run = "crop_fixture";
  descriptor.storage = crimson::zarr::SubjectMaskStorage::Dense;
  descriptor.component_labels = {"subject_body", "eye_left"};
  descriptor.mask_width = 512;
  descriptor.mask_height = 512;
  descriptor.strict_v1 = true;
  descriptor.contour_only = true;
  descriptor.presentation_cache_run = "sampled_contour_fixture";

  crimson::zarr::SubjectMaskOverlayResolution frame;
  frame.status = crimson::zarr::SubjectMaskOverlayStatus::Mapped;
  frame.camera_frame = 12;
  crimson::zarr::SubjectMaskOverlayDetection first;
  first.instance_key = 701;
  first.source_crop_row_id = 31;
  first.roi_width = 512.0;
  first.roi_height = 512.0;
  crimson::zarr::SubjectMaskOverlayComponent body;
  body.label = "subject_body";
  body.channel_index = 0;
  body.present = true;
  body.mask = std::make_shared<const std::vector<uint8_t>>(
      std::vector<uint8_t>{1, 0, 1, 1});
  body.contour = {{1.0, 2.0}, {3.0, 4.0}};
  first.components = {body};
  crimson::zarr::SubjectMaskOverlayDetection second;
  second.instance_key = 702;
  second.source_crop_row_id = 32;
  second.roi_width = 256.0;
  second.roi_height = 256.0;
  crimson::zarr::SubjectMaskOverlayComponent eye;
  eye.label = "eye_left";
  eye.channel_index = 1;
  eye.present = true;
  second.components = {eye};
  frame.detections = {first, second};

  const auto presentation =
      crimson::gui::makeSubjectMaskOverlayInspectPresentation(
          &descriptor, &frame, frame.camera_frame);
  CHECK(presentation.available);
  CHECK(presentation.frame_ready);
  CHECK(presentation.surface_label == "Sampled contour cache");
  CHECK(presentation.run_name == "refined_subject_mask_fixture");
  CHECK(presentation.observations.size() == 2);
  CHECK(presentation.observations[0].selectable);
  CHECK(presentation.observations[0].instance_key == 701);
  CHECK(presentation.observations[0].source_crop_row_id == 31);
  CHECK(presentation.observations[0].roi_valid);
  CHECK(presentation.observations[0].components.size() == 1);
  CHECK(presentation.observations[0].components[0].pixel_payload_available);
  CHECK(presentation.observations[0].components[0].pixel_payload_value_count ==
        4);
  CHECK(presentation.observations[0].components[0].contour_point_count == 2);

  crimson::zarr::SubjectMaskOverlayResolution empty;
  empty.status = crimson::zarr::SubjectMaskOverlayStatus::Missing;
  empty.camera_frame = 13;
  const auto empty_presentation =
      crimson::gui::makeSubjectMaskOverlayInspectPresentation(
          &descriptor, &empty, empty.camera_frame);
  CHECK(empty_presentation.frame_ready);
  CHECK(empty_presentation.observations.empty());
  CHECK(empty_presentation.warning.empty());

  const auto loading = crimson::gui::makeSubjectMaskOverlayInspectPresentation(
      &descriptor, &frame, frame.camera_frame + 1);
  CHECK(loading.available);
  CHECK(!loading.frame_ready);
  CHECK(loading.observations.empty());
  return true;
}

bool testSubjectMaskInspectModuleSnapshot() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(800.0f, 600.0f);
  io.DeltaTime = 1.0f / 60.0f;
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  crimson::gui::SubjectMaskInspectPresentation presentation;
  presentation.available = true;
  presentation.surface_label = "Subject-mask v1";
  presentation.run_name = "refined_subject_mask_fixture";
  presentation.frame_ready = true;
  presentation.camera_frame = 12;
  crimson::gui::SubjectMaskInspectObservation observation;
  observation.instance_key = 701;
  observation.selectable = true;
  observation.valid = true;
  observation.source_crop_row_id = 31;
  observation.source_crop_row_id_valid = true;
  observation.roi_width = 512.0;
  observation.roi_height = 512.0;
  observation.roi_valid = true;
  crimson::gui::SubjectMaskInspectComponent component;
  component.label = "subject_body";
  component.channel_index = 0;
  component.channel_index_valid = true;
  component.present = true;
  component.contour_available = true;
  component.contour_point_count = 128;
  observation.components = {component};
  presentation.observations = {observation};
  crimson::gui::SubjectMaskInspectModuleState state;
  state.selected_instance_key = 701;

  ImGui::NewFrame();
  ImGui::Begin("Subject-mask module fixture");
  crimson::gui::drawFrameInspectSubjectMaskModule(presentation, state);
  ImGui::End();
  ImGui::Render();

  crimson::ui::setSemanticCaptureEnabled(ImGui::GetCurrentContext(), true);
  crimson::ui::beginSemanticFrame(ImGui::GetCurrentContext());
  ImGui::NewFrame();
  ImGui::Begin("Subject-mask module fixture");
  crimson::gui::drawFrameInspectSubjectMaskModule(presentation, state);
  ImGui::End();
  ImGui::Render();
  const auto snapshot =
      crimson::ui::finishSemanticFrame(ImGui::GetCurrentContext());

  bool found_observation = false;
  for (const auto &item : snapshot.items) {
    found_observation |= item.visible_label == "#1";
  }
  CHECK(found_observation);
  CHECK(state.selected_instance_key == 701);

  crimson::ui::setSemanticCaptureEnabled(ImGui::GetCurrentContext(), false);
  ImGui::DestroyContext();
  return true;
}

bool testSubjectShapeOverlayInspectAdapter() {
  crimson::zarr::SubjectShapeOverlayDescriptor descriptor;
  descriptor.source_group = "analysis/subject_shape_runs";
  descriptor.run_name = "subject_shape_fixture";
  descriptor.source_refined_subject_masks_run = "refined_masks_fixture";
  descriptor.source_crop_run = "crop_fixture";
  descriptor.schema_id = "analysis.subject_shape_runs";
  descriptor.schema_version = 1;
  descriptor.method = "centerline_and_bspline";
  descriptor.method_version = 2;
  descriptor.head_endpoint_semantics = "snout_tip";

  crimson::zarr::SubjectShapeOverlayResolution frame;
  frame.status = crimson::zarr::SubjectShapeOverlayStatus::Mapped;
  frame.camera_frame = 13;
  crimson::zarr::SubjectShapeOverlayDetection first;
  first.shape_row = 40;
  first.detection_index = 2;
  first.source_refined_row_id = 71;
  first.source_crop_row_id = 31;
  first.roi_width = 512.0;
  first.roi_height = 512.0;
  first.geometry.body_frame_valid = true;
  first.geometry.snout_tip_valid = true;
  first.geometry.tail_base_valid = true;
  first.geometry.centerline_valid = true;
  first.geometry.centerline = {{1.0, 2.0}, {3.0, 4.0}};
  first.geometry.bspline_valid = true;
  first.geometry.bspline_sample = {
      {1.0, 2.0}, {2.0, 3.0}, {3.0, 4.0}};
  first.geometry.tail_sample_valid = true;
  first.geometry.tail_samples = {{3.0, 4.0}, {4.0, 5.0}};
  crimson::zarr::SubjectShapeOverlayDetection second;
  second.shape_row = 41;
  second.detection_index = 3;
  second.source_refined_row_id = 72;
  second.source_crop_row_id = 32;
  second.roi_width = 256.0;
  second.roi_height = 256.0;
  frame.detections = {first, second};

  const auto presentation =
      crimson::gui::makeSubjectShapeOverlayInspectPresentation(
          &descriptor, &frame, frame.camera_frame);
  CHECK(presentation.available);
  CHECK(presentation.frame_ready);
  CHECK(presentation.run_name == "subject_shape_fixture");
  CHECK(presentation.observations.size() == 2);
  CHECK(presentation.observations[0].row_selection_key == 41);
  CHECK(presentation.observations[0].source_row == 40);
  CHECK(presentation.observations[0].detection_index == 2);
  CHECK(presentation.observations[0].source_refined_row_id == 71);
  CHECK(presentation.observations[0].source_crop_row_id == 31);
  CHECK(presentation.observations[0].roi_valid);
  CHECK(presentation.observations[0].features.size() == 11);
  CHECK(presentation.observations[0].features[5].key == "centerline");
  CHECK(presentation.observations[0].features[5].point_count == 2);
  CHECK(presentation.observations[0].features[7].key == "bspline");
  CHECK(presentation.observations[0].features[7].point_count == 3);
  CHECK(!presentation.observations[1].features[0].valid);

  crimson::zarr::SubjectShapeOverlayResolution empty;
  empty.status = crimson::zarr::SubjectShapeOverlayStatus::Missing;
  empty.camera_frame = 14;
  const auto empty_presentation =
      crimson::gui::makeSubjectShapeOverlayInspectPresentation(
          &descriptor, &empty, empty.camera_frame);
  CHECK(empty_presentation.frame_ready);
  CHECK(empty_presentation.observations.empty());
  CHECK(empty_presentation.warning.empty());

  const auto loading =
      crimson::gui::makeSubjectShapeOverlayInspectPresentation(
          &descriptor, &frame, frame.camera_frame + 1);
  CHECK(loading.available);
  CHECK(!loading.frame_ready);
  CHECK(loading.observations.empty());
  return true;
}

bool testSubjectShapeInspectModuleSnapshot() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(800.0f, 600.0f);
  io.DeltaTime = 1.0f / 60.0f;
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  crimson::gui::SubjectShapeInspectPresentation presentation;
  presentation.available = true;
  presentation.surface_label = "Subject-shape overlay";
  presentation.run_name = "subject_shape_fixture";
  presentation.frame_ready = true;
  presentation.camera_frame = 13;
  crimson::gui::SubjectShapeInspectObservation observation;
  observation.row_selection_key = 41;
  observation.selectable = true;
  observation.source_row = 40;
  observation.source_row_valid = true;
  observation.source_crop_row_id = 31;
  observation.source_crop_row_id_valid = true;
  observation.roi_width = 512.0;
  observation.roi_height = 512.0;
  observation.roi_valid = true;
  observation.features = {
      {"body_frame", "Body frame", true, true, 3},
      {"snout_tip", "Snout tip", true, true, 1},
      {"tail_base", "Tail base", true, true, 1},
      {"centerline", "Centerline", true, true, 64},
      {"bspline", "B-spline", true, true, 64},
  };
  presentation.observations = {observation};
  crimson::gui::SubjectShapeInspectModuleState state;
  state.selected_row_key = 41;

  ImGui::NewFrame();
  ImGui::Begin("Subject-shape module fixture");
  crimson::gui::drawFrameInspectSubjectShapeModule(presentation, state);
  ImGui::End();
  ImGui::Render();

  crimson::ui::setSemanticCaptureEnabled(ImGui::GetCurrentContext(), true);
  crimson::ui::beginSemanticFrame(ImGui::GetCurrentContext());
  ImGui::NewFrame();
  ImGui::Begin("Subject-shape module fixture");
  crimson::gui::drawFrameInspectSubjectShapeModule(presentation, state);
  ImGui::End();
  ImGui::Render();
  const auto snapshot =
      crimson::ui::finishSemanticFrame(ImGui::GetCurrentContext());

  bool found_observation = false;
  for (const auto &item : snapshot.items) {
    found_observation |= item.visible_label == "#1";
  }
  CHECK(found_observation);
  CHECK(state.selected_row_key == 41);

  crimson::ui::setSemanticCaptureEnabled(ImGui::GetCurrentContext(), false);
  ImGui::DestroyContext();
  return true;
}

bool testReadOnlySubjectShapeControlAdapter() {
  crimson::overlay::ReadOnlyOverlayControlState source;
  source.show_subject_shape = false;
  source.show_subject_shape_body_axes = true;
  source.show_subject_shape_snout_tip = false;
  source.show_subject_shape_caudal_anchor = false;
  source.show_subject_shape_tail_base = false;
  source.show_subject_shape_tail_tip = false;
  source.show_subject_shape_centerline = false;
  source.show_subject_shape_bspline = false;
  source.show_subject_shape_bspline_debug_points = true;
  source.show_subject_shape_bspline_control_points = true;
  source.show_subject_shape_tail_samples = true;
  source.show_subject_shape_tail_normals = true;

  auto shared =
      crimson::gui::makeReadOnlySubjectShapeOverlayControlState(source);
  CHECK(!shared.show_overlay);
  CHECK(shared.show_body_frame_axes);
  CHECK(!shared.show_snout_tip);
  CHECK(!shared.show_caudal_anchor);
  CHECK(!shared.show_tail_base);
  CHECK(!shared.show_tail_tip);
  CHECK(!shared.show_centerline);
  CHECK(!shared.show_bspline_sample);
  CHECK(shared.show_bspline_debug_points);
  CHECK(shared.show_bspline_control_points);
  CHECK(shared.show_tail_samples);
  CHECK(shared.show_tail_normals);
  CHECK(!shared.show_body_contour);
  CHECK(!shared.show_swim_bladder_contour);
  CHECK(!shared.show_eye_contours);

  shared.show_overlay = true;
  shared.show_body_frame_axes = false;
  shared.show_snout_tip = true;
  shared.show_caudal_anchor = true;
  shared.show_tail_base = true;
  shared.show_tail_tip = true;
  shared.show_centerline = true;
  shared.show_bspline_sample = true;
  shared.show_bspline_debug_points = false;
  shared.show_bspline_control_points = false;
  shared.show_tail_samples = false;
  shared.show_tail_normals = false;
  crimson::gui::applyReadOnlySubjectShapeOverlayControlState(shared, &source);
  CHECK(source.show_subject_shape);
  CHECK(!source.show_subject_shape_body_axes);
  CHECK(source.show_subject_shape_snout_tip);
  CHECK(source.show_subject_shape_caudal_anchor);
  CHECK(source.show_subject_shape_tail_base);
  CHECK(source.show_subject_shape_tail_tip);
  CHECK(source.show_subject_shape_centerline);
  CHECK(source.show_subject_shape_bspline);
  CHECK(!source.show_subject_shape_bspline_debug_points);
  CHECK(!source.show_subject_shape_bspline_control_points);
  CHECK(!source.show_subject_shape_tail_samples);
  CHECK(!source.show_subject_shape_tail_normals);
  crimson::gui::applyReadOnlySubjectShapeOverlayControlState(shared, nullptr);
  return true;
}

bool testSubjectShapeOverlayControlsSnapshot() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(900.0f, 700.0f);
  io.DeltaTime = 1.0f / 60.0f;
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  crimson::gui::SubjectShapeOverlayControlState strict_state;
  crimson::gui::SubjectShapeOverlayControlState legacy_state;
  ImGui::NewFrame();
  ImGui::Begin("Strict shape controls");
  crimson::gui::drawSubjectShapeOverlayControls(
      strict_state, {true, false, false, false});
  ImGui::End();
  ImGui::Begin("Legacy shape controls");
  crimson::gui::drawSubjectShapeOverlayControls(
      legacy_state, {true, true, true, true});
  ImGui::End();
  ImGui::Render();

  crimson::ui::setSemanticCaptureEnabled(ImGui::GetCurrentContext(), true);
  crimson::ui::beginSemanticFrame(ImGui::GetCurrentContext());
  ImGui::NewFrame();
  ImGui::Begin("Strict shape controls");
  const auto strict_result = crimson::gui::drawSubjectShapeOverlayControls(
      strict_state, {true, false, false, false});
  ImGui::End();
  ImGui::Begin("Legacy shape controls");
  const auto legacy_result = crimson::gui::drawSubjectShapeOverlayControls(
      legacy_state, {true, true, true, true});
  ImGui::End();
  ImGui::Render();
  const auto snapshot =
      crimson::ui::finishSemanticFrame(ImGui::GetCurrentContext());

  bool strict_has_common = false;
  bool strict_has_contour = false;
  bool legacy_has_common = false;
  bool legacy_has_body_contour = false;
  bool legacy_has_swim_bladder_contour = false;
  bool legacy_has_eye_contours = false;
  for (const auto &item : snapshot.items) {
    if (item.window_name == "Strict shape controls") {
      strict_has_common |= item.visible_label == "Centerline";
      strict_has_contour |= item.visible_label == "Body contour";
    } else if (item.window_name == "Legacy shape controls") {
      legacy_has_common |= item.visible_label == "Centerline";
      legacy_has_body_contour |= item.visible_label == "Body contour";
      legacy_has_swim_bladder_contour |=
          item.visible_label == "Swim-bladder contour";
      legacy_has_eye_contours |= item.visible_label == "Eye contours";
    }
  }
  CHECK(strict_has_common);
  CHECK(!strict_has_contour);
  CHECK(legacy_has_common);
  CHECK(legacy_has_body_contour);
  CHECK(legacy_has_swim_bladder_contour);
  CHECK(legacy_has_eye_contours);
  CHECK(!strict_result.changed);
  CHECK(!legacy_result.changed);

  crimson::ui::setSemanticCaptureEnabled(ImGui::GetCurrentContext(), false);
  ImGui::DestroyContext();
  return true;
}

bool testReadOnlyEyeGeometryControlAdapter() {
  crimson::overlay::ReadOnlyOverlayControlState source;
  source.show_eye_geometry = false;
  source.show_eye_direction_beams = false;
  source.show_eye_gaze_rays = true;
  source.show_eye_angle_arcs = false;
  source.show_eye_angle_labels = true;

  auto shared =
      crimson::gui::makeReadOnlyEyeGeometryOverlayControlState(source);
  CHECK(!shared.show_overlay);
  CHECK(!shared.show_direction_beams);
  CHECK(shared.show_gaze_rays);
  CHECK(!shared.show_angle_arcs);
  CHECK(shared.show_angle_labels);

  shared.show_overlay = true;
  shared.show_direction_beams = true;
  shared.show_gaze_rays = false;
  shared.show_angle_arcs = true;
  shared.show_angle_labels = false;
  crimson::gui::applyReadOnlyEyeGeometryOverlayControlState(shared, &source);
  CHECK(source.show_eye_geometry);
  CHECK(source.show_eye_direction_beams);
  CHECK(!source.show_eye_gaze_rays);
  CHECK(source.show_eye_angle_arcs);
  CHECK(!source.show_eye_angle_labels);
  crimson::gui::applyReadOnlyEyeGeometryOverlayControlState(shared, nullptr);
  return true;
}

bool testEyeGeometryOverlayControlsSnapshot() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(900.0f, 700.0f);
  io.DeltaTime = 1.0f / 60.0f;
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  crimson::gui::EyeGeometryOverlayControlState strict_state;
  crimson::gui::EyeGeometryOverlayControlState legacy_state;
  ImGui::NewFrame();
  ImGui::Begin("Strict eye controls");
  crimson::gui::drawEyeGeometryOverlayControls(strict_state, {true, true});
  ImGui::End();
  ImGui::Begin("Legacy eye controls");
  crimson::gui::drawEyeGeometryOverlayControls(legacy_state, {true, false});
  ImGui::End();
  ImGui::Render();

  crimson::ui::setSemanticCaptureEnabled(ImGui::GetCurrentContext(), true);
  crimson::ui::beginSemanticFrame(ImGui::GetCurrentContext());
  ImGui::NewFrame();
  ImGui::Begin("Strict eye controls");
  const auto strict_result =
      crimson::gui::drawEyeGeometryOverlayControls(strict_state, {true, true});
  ImGui::End();
  ImGui::Begin("Legacy eye controls");
  const auto legacy_result =
      crimson::gui::drawEyeGeometryOverlayControls(legacy_state, {true, false});
  ImGui::End();
  ImGui::Render();
  const auto snapshot =
      crimson::ui::finishSemanticFrame(ImGui::GetCurrentContext());

  bool strict_has_master = false;
  bool strict_has_details = false;
  bool legacy_has_master = false;
  bool legacy_has_details = false;
  for (const auto &item : snapshot.items) {
    if (item.window_name == "Strict eye controls") {
      strict_has_master |= item.visible_label == "Show eye geometry";
      strict_has_details |= item.visible_label == "Eye visual cones";
    } else if (item.window_name == "Legacy eye controls") {
      legacy_has_master |= item.visible_label == "Show eye geometry";
      legacy_has_details |= item.visible_label == "Eye visual cones";
    }
  }
  CHECK(strict_has_master);
  CHECK(strict_has_details);
  CHECK(!legacy_has_master);
  CHECK(legacy_has_details);
  CHECK(!strict_result.changed);
  CHECK(!legacy_result.changed);

  crimson::ui::setSemanticCaptureEnabled(ImGui::GetCurrentContext(), false);
  ImGui::DestroyContext();
  return true;
}

bool testEyeGeometryOverlayInspectAdapter() {
  crimson::zarr::EyeGeometryOverlayDescriptor descriptor;
  descriptor.source_group = "analysis/eye_angle_runs";
  descriptor.run_name = "eye_geometry_fixture";
  descriptor.source_refined_subject_masks_run = "refined_masks_fixture";
  descriptor.source_crop_run = "crop_fixture";
  descriptor.schema_id = "analysis.eye_angle_runs";
  descriptor.schema_version = 5;
  descriptor.method = "ellipse_and_centroid_eye_angles";
  descriptor.method_version = "1";

  crimson::zarr::EyeGeometryOverlayResolution frame;
  frame.status = crimson::zarr::EyeGeometryOverlayStatus::Mapped;
  frame.camera_frame = 14;
  crimson::zarr::EyeGeometryOverlayDetection first;
  first.eye_row = 10;
  first.camera_frame = 14;
  first.detection_index = 3;
  first.source_crop_row_id = 21;
  first.frame_valid = true;
  first.eyes[0].valid = true;
  first.eyes[0].eye_frame_angle_valid = true;
  first.eyes[0].eye_frame_angle_degrees = 12.5;
  first.eyes[0].signed_angle_valid = true;
  first.eyes[0].signed_angle_degrees = -8.0;
  first.eyes[0].gaze_valid = true;
  first.eyes[0].gaze = {0.25, 0.75};
  first.eyes[1].valid = true;
  first.eyes[1].eye_frame_angle_valid = true;
  first.eyes[1].eye_frame_angle_degrees = -11.0;
  first.eyes[1].signed_angle_valid = true;
  first.eyes[1].signed_angle_degrees = 7.5;
  first.eyes[1].gaze_valid = true;
  first.eyes[1].gaze = {-0.25, 0.75};
  first.vergence_valid = true;
  first.vergence_degrees = 23.5;
  crimson::zarr::EyeGeometryOverlayDetection second;
  second.eye_row = 11;
  second.camera_frame = 14;
  second.detection_index = 4;
  second.source_crop_row_id = 22;
  second.frame_valid = false;
  frame.detections = {first, second};

  const auto presentation =
      crimson::gui::makeEyeGeometryOverlayInspectPresentation(
          &descriptor, &frame, frame.camera_frame);
  CHECK(presentation.available);
  CHECK(presentation.frame_ready);
  CHECK(presentation.run_name == "eye_geometry_fixture");
  CHECK(presentation.default_representation_key == "eye_frame");
  CHECK(presentation.representations.size() == 3);
  CHECK(presentation.observations.size() == 2);
  CHECK(presentation.observations[0].row_selection_key == 11);
  CHECK(presentation.observations[0].source_row == 10);
  CHECK(presentation.observations[0].detection_index == 3);
  CHECK(presentation.observations[0].source_crop_row_id == 21);
  CHECK(presentation.observations[0].left_valid);
  CHECK(presentation.observations[0].right_valid);
  CHECK(presentation.observations[0].fields.size() == 7);
  CHECK(presentation.observations[0].fields[0].valid);
  CHECK(near(static_cast<float>(presentation.observations[0].fields[0].value_x),
             12.5f));
  CHECK(!presentation.observations[1].frame_valid);

  crimson::zarr::EyeGeometryOverlayResolution empty;
  empty.status = crimson::zarr::EyeGeometryOverlayStatus::Missing;
  empty.camera_frame = 15;
  const auto empty_presentation =
      crimson::gui::makeEyeGeometryOverlayInspectPresentation(
          &descriptor, &empty, empty.camera_frame);
  CHECK(empty_presentation.frame_ready);
  CHECK(empty_presentation.observations.empty());
  CHECK(empty_presentation.warning.empty());

  const auto loading = crimson::gui::makeEyeGeometryOverlayInspectPresentation(
      &descriptor, &frame, frame.camera_frame + 1);
  CHECK(loading.available);
  CHECK(!loading.frame_ready);
  CHECK(loading.observations.empty());
  return true;
}

bool testEyeAngleInspectModuleSnapshot() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(800.0f, 600.0f);
  io.DeltaTime = 1.0f / 60.0f;
  unsigned char *pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  crimson::gui::EyeAngleInspectPresentation presentation;
  presentation.available = true;
  presentation.surface_label = "Eye-geometry overlay";
  presentation.run_name = "eye_geometry_fixture";
  presentation.default_representation_key = "eye_frame";
  presentation.representations = {
      {"eye_frame", "Eye frame", "body-relative eye angle", "eye frame"}};
  presentation.frame_ready = true;
  presentation.camera_frame = 14;
  crimson::gui::EyeAngleInspectObservation observation;
  observation.row_selection_key = 11;
  observation.selectable = true;
  observation.source_row = 10;
  observation.source_row_valid = true;
  observation.frame_valid = true;
  observation.frame_valid_known = true;
  observation.left_valid = true;
  observation.left_valid_known = true;
  observation.right_valid = true;
  observation.right_valid_known = true;
  crimson::gui::EyeAngleInspectField field;
  field.representation_key = "eye_frame";
  field.label = "Vergence";
  field.units = "deg";
  field.value_x = 23.5;
  field.valid = true;
  observation.fields = {field};
  presentation.observations = {observation};
  crimson::gui::EyeAngleInspectModuleState state;
  state.selected_row_key = 11;

  ImGui::NewFrame();
  ImGui::Begin("Eye-angle module fixture");
  crimson::gui::drawFrameInspectEyeAngleModule(presentation, state);
  ImGui::End();
  ImGui::Render();

  crimson::ui::setSemanticCaptureEnabled(ImGui::GetCurrentContext(), true);
  crimson::ui::beginSemanticFrame(ImGui::GetCurrentContext());
  ImGui::NewFrame();
  ImGui::Begin("Eye-angle module fixture");
  crimson::gui::drawFrameInspectEyeAngleModule(presentation, state);
  ImGui::End();
  ImGui::Render();
  const auto snapshot =
      crimson::ui::finishSemanticFrame(ImGui::GetCurrentContext());

  bool found_observation = false;
  for (const auto &item : snapshot.items) {
    found_observation |= item.visible_label == "#1";
  }
  CHECK(found_observation);
  CHECK(state.selected_row_key == 11);
  CHECK(state.selected_representation_key == "eye_frame");

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
      !testKeypointInspectModuleSnapshot() ||
      !testSubjectMaskOverlayInspectAdapter() ||
      !testSubjectMaskInspectModuleSnapshot() ||
      !testSubjectShapeOverlayInspectAdapter() ||
      !testSubjectShapeInspectModuleSnapshot() ||
      !testReadOnlySubjectShapeControlAdapter() ||
      !testSubjectShapeOverlayControlsSnapshot() ||
      !testReadOnlyEyeGeometryControlAdapter() ||
      !testEyeGeometryOverlayControlsSnapshot() ||
      !testEyeGeometryOverlayInspectAdapter() ||
      !testEyeAngleInspectModuleSnapshot()) {
    return 1;
  }
  std::cout << "imgui_semantic_snapshot_tests: PASS\n";
  return 0;
}
