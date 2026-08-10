#include "IconsForkAwesome.h"
#include "analysis_series_timeline_buffer.h"
#include "apple_acquisition_crop_playback_session.h"
#include "apple_analysis_product_adopter.h"
#include "apple_analysis_repository_loader.h"
#include "apple_metal_presentation_texture.h"
#include "apple_overlay_metal_renderer.h"
#include "apple_playback_presentation_adapter.h"
#include "apple_stimulus_playback_session.h"
#include "apple_video_frame_provider.h"
#include "apple_video_metal_renderer.h"
#include "apple_video_playback_buffer.h"
#include "apple_video_viewer_ui.h"
#include "canonical_detection_buffer.h"
#include "chaser_distance_polar_buffer.h"
#include "chaser_distance_polar_scene.h"
#include "crop_presentation_coordinator.h"
#include "data_access_diagnostics.h"
#include "data_access_scheduler.h"
#include "debug_flags.h"
#include "diagnostic_report.h"
#include "eye_angle_timeline_buffer.h"
#include "eye_geometry_overlay_buffer.h"
#include "frame_presentation.h"
#include "gui/quality_timeline_session.h"
#include "gui/session_loading_modal.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_metal.h"
#include "imgui_semantic_snapshot.h"
#include "implot.h"
#include "keypoint_overlay_buffer.h"
#include "playback_clock.h"
#include "recording_open_workflow.h"
#include "session_readiness.h"
#include "stimulus_camera_overlay_scene.h"
#include "stimulus_context_timeline.h"
#include "stimulus_presentation_coordinator.h"
#include "subject_mask_overlay_buffer.h"
#include "subject_mask_presentation_coordinator.h"
#include "subject_shape_overlay_buffer.h"
#include "swim_bout_timeline_buffer.h"
#include "ui_reference_capture.h"
#include "ui_reference_contract.h"
#include "ui_path_config.h"
#include "zarr/affiliated_video_repository.h"
#include "zarr/analysis_crop_geometry_repository.h"
#include "zarr/archive_context.h"
#include "zarr/canonical_detection_overlay_scene_adapter.h"
#include "zarr/eye_geometry_overlay_scene_adapter.h"
#include "zarr/keypoint_overlay_repository.h"
#include "zarr/keypoint_overlay_scene_adapter.h"
#include "zarr/subject_mask_overlay_repository.h"
#include "zarr/subject_mask_overlay_scene_adapter.h"
#include "zarr/subject_shape_overlay_scene_adapter.h"
#include "zarr/tensorstore_acquisition_crop_repository.h"
#include "zarr/tensorstore_analysis_crop_geometry_repository.h"
#include "zarr/tensorstore_analysis_series_timeline_repository.h"
#include "zarr/tensorstore_chaser_distance_polar_repository.h"
#include "zarr/tensorstore_detection_quality_timeline_repository.h"
#include "zarr/tensorstore_eye_angle_timeline_repository.h"
#include "zarr/tensorstore_eye_geometry_overlay_repository.h"
#include "zarr/tensorstore_keypoint_overlay_repository.h"
#include "zarr/tensorstore_stimulus_context_timeline_repository.h"
#include "zarr/tensorstore_stimulus_repository.h"
#include "zarr/tensorstore_subject_mask_overlay_repository.h"
#include "zarr/tensorstore_subject_shape_overlay_repository.h"
#include "zarr/tensorstore_swim_bout_timeline_repository.h"

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#import <AppKit/AppKit.h>
#import <CoreImage/CoreImage.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#if !defined(__arm64__)
#error "The Crimson macOS backend must be compiled for Apple Silicon arm64."
#endif

#ifndef CRIMSON_GIT_COMMIT
#define CRIMSON_GIT_COMMIT "unknown"
#endif

namespace {

using AppleUiReferenceState = crimson::ui_reference::State;
using AppleUiReferenceConfig = crimson::ui_reference::LaunchOptions;

constexpr crimson::ui_reference::StateMask kAppleUiReferenceStates =
    crimson::ui_reference::stateBit(AppleUiReferenceState::Empty) |
    crimson::ui_reference::stateBit(AppleUiReferenceState::Workspace) |
    crimson::ui_reference::stateBit(AppleUiReferenceState::Keypoints) |
    crimson::ui_reference::stateBit(AppleUiReferenceState::Overlays) |
    crimson::ui_reference::stateBit(AppleUiReferenceState::Polar) |
    crimson::ui_reference::stateBit(AppleUiReferenceState::StimulusOverlay) |
    crimson::ui_reference::stateBit(AppleUiReferenceState::CropPreview) |
    crimson::ui_reference::stateBit(AppleUiReferenceState::AnalysisEye) |
    crimson::ui_reference::stateBit(AppleUiReferenceState::StimulusDebug);

constexpr crimson::ui_reference::LaunchParsePolicy kUiReferenceParsePolicy{
    kAppleUiReferenceStates, true, 4096};

const char *appleUiReferenceStateName(AppleUiReferenceState state) {
  return crimson::ui_reference::stateName(state);
}

struct LaunchOptions {
  bool smoke = false;
  bool validate_metal = false;
  bool video_smoke = false;
  bool stimulus_smoke = false;
  bool crop_smoke = false;
  bool multistream_smoke = false;
  bool subject_masks_enabled = true;
  bool show_subject_masks = false;
  bool subject_shapes_enabled = true;
  bool eye_geometry_enabled = true;
  bool motion_timeline_enabled = true;
  bool swim_bout_timeline_enabled = true;
  bool eye_angle_timeline_enabled = true;
  bool tail_kinematics_timeline_enabled = true;
  bool stimulus_context_timeline_enabled = true;
  bool show_analysis_timeline = false;
  bool show_eye_angle_timeline = false;
  bool show_stimulus_timeline = false;
  bool require_motion_timeline = false;
  bool require_swim_bout_timeline = false;
  bool require_eye_angle_timeline = false;
  bool require_tail_kinematics_timeline = false;
  bool require_stimulus_context_timeline = false;
  int smoke_frames = 12;
  int video_smoke_start = 0;
  int video_smoke_end = 0;
  std::optional<int> start_paused_frame;
  std::string video_path;
  std::string recording_clip_index_path;
  std::string zarr_path;
  std::string stimulus_video_path;
  std::string detection_run;
  std::string refined_detection_run;
  bool allow_selector_ineligible_refined_run = false;
  std::string stimulus_run;
  std::string crop_run;
  std::string swim_bout_run;
  AppleKeypointV2LoadRequest keypoint_v2;
  std::string subject_mask_run;
  std::string subject_mask_manifest_payload_digest;
  bool allow_selector_ineligible_subject_mask_run = false;
  bool require_subject_mask_v1 = false;
  std::string subject_mask_presentation_cache_path;
  std::string subject_mask_presentation_cache_run;
  std::string subject_mask_presentation_cache_manifest_payload_digest;
  bool subject_mask_contour_only = false;
  size_t video_buffer_capacity = 6;
  size_t stimulus_buffer_capacity = 6;
  AppleUiReferenceConfig ui_reference = [] {
    AppleUiReferenceConfig options;
    options.timeout_seconds = 90.0;
    return options;
  }();
  crimson::crop::CropSourcePreference crop_preference =
      crimson::crop::CropSourcePreference::PreferAcquisitionVideo;
};

enum class MultistreamSmokeStage : uint8_t {
  Disabled,
  PlayToPause,
  WaitForPausedExact,
  WaitForStepExact,
  WaitForBackwardSeekExact,
  WaitForForwardSeekExact,
  ResumeToEnd,
  WaitForEndExact,
  Complete,
};

struct MultistreamSmokeState {
  MultistreamSmokeStage stage = MultistreamSmokeStage::Disabled;
  int64_t pause_frame = -1;
  int64_t step_frame = -1;
  int64_t backward_frame = -1;
  int64_t forward_frame = -1;
  int64_t end_frame = -1;
  uint64_t exact_settlements = 0;
  std::string error;
};

constexpr double kMultistreamMemoryGrowthLimitMiB = 512.0;
constexpr size_t kAcquisitionCropBufferCapacity = 32;

void glfwErrorCallback(int error, const char *description) {
  std::fprintf(stderr, "Glfw Error %d: %s\n", error, description);
}

std::optional<int> parsePositiveInt(const char *value) {
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  int parsed = 0;
  const char *end = value + std::char_traits<char>::length(value);
  const auto result = std::from_chars(value, end, parsed);
  if (result.ec != std::errc() || result.ptr != end || parsed < 1) {
    return std::nullopt;
  }
  return parsed;
}

std::optional<int> parseNonNegativeInt(const std::string &value) {
  if (value.empty()) {
    return std::nullopt;
  }
  int parsed = 0;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc() || result.ptr != value.data() + value.size() ||
      parsed < 0) {
    return std::nullopt;
  }
  return parsed;
}

bool parseFrameRange(const std::string &value, int *start, int *end) {
  const size_t separator = value.find(':');
  if (separator == std::string::npos) {
    return false;
  }
  const auto parsed_start = parseNonNegativeInt(value.substr(0, separator));
  const auto parsed_end = parseNonNegativeInt(value.substr(separator + 1));
  if (!parsed_start || !parsed_end || *parsed_end < *parsed_start) {
    return false;
  }
  *start = *parsed_start;
  *end = *parsed_end;
  return true;
}

bool viewportFitsDrawable(const AppleMetalVideoViewport &viewport,
                          int framebuffer_width, int framebuffer_height) {
  return std::isfinite(viewport.x) && std::isfinite(viewport.y) &&
         std::isfinite(viewport.width) && std::isfinite(viewport.height) &&
         viewport.x >= -0.001 && viewport.y >= -0.001 && viewport.width > 0.0 &&
         viewport.height > 0.0 &&
         viewport.x + viewport.width <= framebuffer_width + 0.001 &&
         viewport.y + viewport.height <= framebuffer_height + 0.001;
}

bool viewportHasArea(const AppleMetalVideoViewport &viewport) {
  return std::isfinite(viewport.x) && std::isfinite(viewport.y) &&
         std::isfinite(viewport.width) && std::isfinite(viewport.height) &&
         viewport.width > 0.0 && viewport.height > 0.0;
}

bool writeBgraPng(const std::filesystem::path &output_path,
                  const uint8_t *pixels, size_t width, size_t height,
                  size_t source_bytes_per_row, std::string *error) {
  if (pixels == nullptr || width == 0 || height == 0 ||
      source_bytes_per_row < width * 4) {
    if (error != nullptr) {
      *error = "invalid BGRA image buffer";
    }
    return false;
  }
  std::error_code ec;
  if (output_path.has_parent_path()) {
    std::filesystem::create_directories(output_path.parent_path(), ec);
    if (ec) {
      if (error != nullptr) {
        *error = "failed to create PNG parent directory: " + ec.message();
      }
      return false;
    }
  }

  std::vector<uint8_t> rgba(width * height * 4);
  for (size_t y = 0; y < height; ++y) {
    const uint8_t *source = pixels + y * source_bytes_per_row;
    uint8_t *destination = rgba.data() + y * width * 4;
    for (size_t x = 0; x < width; ++x) {
      destination[x * 4 + 0] = source[x * 4 + 2];
      destination[x * 4 + 1] = source[x * 4 + 1];
      destination[x * 4 + 2] = source[x * 4 + 0];
      destination[x * 4 + 3] = source[x * 4 + 3];
    }
  }

  CGDataProviderRef provider =
      CGDataProviderCreateWithData(nullptr, rgba.data(), rgba.size(), nullptr);
  CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
  CGImageRef image =
      provider == nullptr || color_space == nullptr
          ? nullptr
          : CGImageCreate(width, height, 8, 32, width * 4, color_space,
                          kCGImageAlphaLast | kCGBitmapByteOrder32Big, provider,
                          nullptr, false, kCGRenderingIntentDefault);
  bool written = false;
  if (image != nullptr) {
    NSBitmapImageRep *representation =
        [[NSBitmapImageRep alloc] initWithCGImage:image];
    NSData *png =
        [representation representationUsingType:NSBitmapImageFileTypePNG
                                     properties:@{}];
    NSString *path =
        [NSString stringWithUTF8String:output_path.string().c_str()];
    written = png != nil && path != nil &&
              [png writeToFile:path options:NSDataWritingAtomic error:nil];
  }
  if (image != nullptr) {
    CGImageRelease(image);
  }
  if (color_space != nullptr) {
    CGColorSpaceRelease(color_space);
  }
  if (provider != nullptr) {
    CGDataProviderRelease(provider);
  }
  if (!written && error != nullptr) {
    *error = "AppKit failed to encode the Metal capture as PNG";
  }
  return written;
}

nlohmann::json viewportJson(const AppleMetalVideoViewport &viewport) {
  return {{"x", viewport.x},
          {"y", viewport.y},
          {"width", viewport.width},
          {"height", viewport.height}};
}

nlohmann::json polarSceneReferenceJson(
    const crimson::polar::ChaserDistancePolarScene &scene,
    double display_origin_x, double display_origin_y, double scale_x,
    double scale_y,
    const crimson::polar::ChaserDistancePolarDescriptor *descriptor) {
  nlohmann::json result = {
      {"ready", scene.ready()},
      {"status",
       crimson::polar::chaserDistancePolarSceneStatusName(scene.status)},
      {"availability", crimson::polar::chaserDistancePolarAvailabilityName(
                           scene.frame_availability)},
      {"requested_frame", scene.requested_camera_frame},
      {"source_frame", scene.source_camera_frame
                           ? nlohmann::json(*scene.source_camera_frame)
                           : nlohmann::json(nullptr)},
      {"point_count", scene.point_count},
      {"primitive_count", scene.primitives.size()},
      {"text_count", scene.text.size()},
      {"semantic_signature",
       crimson::polar::chaserDistancePolarSceneSemanticSignature(scene)},
  };
  if (descriptor != nullptr) {
    result["descriptor"] = {
        {"availability", crimson::polar::chaserDistancePolarAvailabilityName(
                             descriptor->availability)},
        {"source_group", descriptor->provenance.source_group},
        {"run_name", descriptor->provenance.run_name},
        {"component_name", descriptor->provenance.component_name},
        {"run_selection",
         crimson::polar::chaserDistancePolarSelectionProvenanceName(
             descriptor->provenance.run_selection)},
        {"component_selection",
         crimson::polar::chaserDistancePolarSelectionProvenanceName(
             descriptor->provenance.component_selection)},
        {"row_count", descriptor->row_count},
        {"chaser_count", descriptor->chaser_count},
        {"distance_unit", crimson::polar::chaserDistancePolarDistanceUnitName(
                              descriptor->distance_unit)},
        {"coordinate_frame", descriptor->coordinate_frame},
        {"angle_convention", descriptor->angle_convention},
        {"normalized_coordinate_frame",
         static_cast<int>(descriptor->normalized_coordinate_frame)},
        {"normalized_angle_convention",
         static_cast<int>(descriptor->normalized_angle_convention)},
        {"dataset_global_max_distance_mm",
         descriptor->dataset_global_max_distance_mm},
        {"display_max_distance_mm",
         descriptor->radial_scale.display_max_distance_mm},
    };
  }
  if (!scene.ready() || !std::isfinite(display_origin_x) ||
      !std::isfinite(display_origin_y) || !std::isfinite(scale_x) ||
      !std::isfinite(scale_y) || scale_x <= 0.0 || scale_y <= 0.0) {
    return result;
  }

  auto pointJson = [&](crimson::polar::ChaserDistancePolarScenePoint point) {
    return nlohmann::json{{"x", point.x - scene.box.x},
                          {"y", point.y - scene.box.y}};
  };
  auto colorJson = [](const crimson::polar::ChaserDistancePolarRgba &color) {
    return nlohmann::json::array(
        {color.red, color.green, color.blue, color.alpha});
  };
  result["viewport"] = {{"width", scene.viewport.width_px},
                        {"height", scene.viewport.height_px}};
  result["box"] = {{"x", scene.box.x},
                   {"y", scene.box.y},
                   {"width", scene.box.width},
                   {"height", scene.box.height}};
  result["screen_box"] = {
      {"x", display_origin_x + scene.box.x * scale_x},
      {"y", display_origin_y + scene.box.y * scale_y},
      {"width", scene.box.width * scale_x},
      {"height", scene.box.height * scale_y},
  };
  result["graph"] = {
      {"x", scene.graph.x - scene.box.x},
      {"y", scene.graph.y - scene.box.y},
      {"width", scene.graph.width},
      {"height", scene.graph.height},
  };
  result["center"] = pointJson(scene.center);
  result["radius_px"] = scene.radius_px;
  result["display_max_distance_mm"] = scene.display_max_distance_mm;

  result["points"] = nlohmann::json::array();
  for (const auto &primitive : scene.primitives) {
    if (primitive.type !=
        crimson::polar::ChaserDistancePolarScenePrimitiveType::Marker) {
      continue;
    }
    result["points"].push_back({
        {"chaser_index", primitive.chaser_index},
        {"center", pointJson(primitive.first)},
        {"screen_center",
         {{"x", display_origin_x + primitive.first.x * scale_x},
          {"y", display_origin_y + primitive.first.y * scale_y}}},
        {"radius_px", primitive.radius_px},
        {"fill", colorJson(primitive.fill)},
        {"stroke", colorJson(primitive.stroke)},
    });
  }
  result["text"] = nlohmann::json::array();
  for (const auto &annotation : scene.text) {
    result["text"].push_back({
        {"layer",
         crimson::polar::chaserDistancePolarSceneLayerName(annotation.layer)},
        {"anchor", pointJson(annotation.anchor)},
        {"centered", annotation.centered},
        {"color", colorJson(annotation.color)},
        {"content", annotation.content},
    });
  }
  return result;
}

nlohmann::json stimulusCameraOverlaySceneReferenceJson(
    const crimson::stimulus::StimulusCameraOverlayScene &scene,
    double display_origin_x, double display_origin_y, double scale_x,
    double scale_y,
    const crimson::timeline::StimulusContextTimelineDescriptor *descriptor) {
  nlohmann::json result = {
      {"ready", scene.ready()},
      {"status",
       crimson::stimulus::stimulusCameraOverlaySceneStatusName(scene.status)},
      {"availability",
       crimson::stimulus::stimulusCameraOverlayFrameAvailabilityName(
           scene.frame_availability)},
      {"requested_frame", scene.requested_camera_frame},
      {"source_frame", scene.source_camera_frame
                           ? nlohmann::json(*scene.source_camera_frame)
                           : nlohmann::json(nullptr)},
      {"event_source_frame",
       scene.event_source_camera_frame
           ? nlohmann::json(*scene.event_source_camera_frame)
           : nlohmann::json(nullptr)},
      {"step_index", scene.step_index ? nlohmann::json(*scene.step_index)
                                      : nlohmann::json(nullptr)},
      {"grating_direction_camera_deg",
       scene.grating_direction_camera_deg
           ? nlohmann::json(*scene.grating_direction_camera_deg)
           : nlohmann::json(nullptr)},
      {"primitive_count", scene.primitives.size()},
      {"text_count", scene.text.size()},
      {"event_box",
       {{"x", scene.event_box.x},
        {"y", scene.event_box.y},
        {"width", scene.event_box.width},
        {"height", scene.event_box.height}}},
      {"step_box",
       {{"x", scene.step_box.x},
        {"y", scene.step_box.y},
        {"width", scene.step_box.width},
        {"height", scene.step_box.height}}},
      {"viewport",
       {{"width", scene.viewport.width_px},
        {"height", scene.viewport.height_px}}},
      {"display_origin", {{"x", display_origin_x}, {"y", display_origin_y}}},
      {"display_scale", {{"x", scale_x}, {"y", scale_y}}},
      {"semantic_signature",
       crimson::stimulus::stimulusCameraOverlaySceneSemanticSignature(scene)},
      {"screen_event_box",
       {{"x", display_origin_x + scene.event_box.x * scale_x},
        {"y", display_origin_y + scene.event_box.y * scale_y},
        {"width", scene.event_box.width * scale_x},
        {"height", scene.event_box.height * scale_y}}},
      {"screen_step_box",
       {{"x", display_origin_x + scene.step_box.x * scale_x},
        {"y", display_origin_y + scene.step_box.y * scale_y},
        {"width", scene.step_box.width * scale_x},
        {"height", scene.step_box.height * scale_y}}},
      {"descriptor",
       descriptor != nullptr
           ? nlohmann::json{{"run_name", descriptor->run_name},
                            {"frame_count", descriptor->frame_count},
                            {"event_count", descriptor->event_count},
                            {"step_count", descriptor->step_count}}
           : nlohmann::json(nullptr)}};
  auto colorJson = [](const auto &color) {
    return nlohmann::json::array(
        {color.red, color.green, color.blue, color.alpha});
  };
  result["primitives"] = nlohmann::json::array();
  for (const auto &primitive : scene.primitives) {
    result["primitives"].push_back({
        {"type", static_cast<int>(primitive.type)},
        {"layer", crimson::stimulus::stimulusCameraOverlaySceneLayerName(
                      primitive.layer)},
        {"first", {{"x", primitive.first.x}, {"y", primitive.first.y}}},
        {"second", {{"x", primitive.second.x}, {"y", primitive.second.y}}},
        {"third", {{"x", primitive.third.x}, {"y", primitive.third.y}}},
        {"corner_radius_px", primitive.corner_radius_px},
        {"stroke_width_px", primitive.stroke_width_px},
        {"fill", colorJson(primitive.fill)},
        {"stroke", colorJson(primitive.stroke)},
        {"has_fill", primitive.has_fill},
        {"has_stroke", primitive.has_stroke},
    });
  }
  result["text"] = nlohmann::json::array();
  for (const auto &annotation : scene.text) {
    result["text"].push_back({
        {"layer", crimson::stimulus::stimulusCameraOverlaySceneLayerName(
                      annotation.layer)},
        {"anchor", {{"x", annotation.anchor.x}, {"y", annotation.anchor.y}}},
        {"color", colorJson(annotation.color)},
        {"content", annotation.content},
    });
  }
  return result;
}

std::filesystem::path decodeDumpRoot() {
  if (const char *configured = std::getenv("CRIMSON_BUFFER_DUMP_DIR");
      configured != nullptr && *configured != '\0') {
    return configured;
  }
  return GetDefaultCrimsonBufferDumpRoot();
}

bool dumpAppleDecodeBuffer(const AppleVideoPlaybackBuffer &playback,
                           const std::filesystem::path &root,
                           const std::string &reason, std::string *status) {
  const auto frames = playback.bufferedFrameNumbers();
  if (frames.empty()) {
    if (status != nullptr) {
      *status = "No decoded camera frames are buffered.";
    }
    return false;
  }
  const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  const std::filesystem::path output =
      root / (reason + "_" + std::to_string(stamp));
  std::error_code ec;
  std::filesystem::create_directories(output, ec);
  if (ec) {
    if (status != nullptr) {
      *status = "Could not create " + output.string() + ": " + ec.message();
    }
    return false;
  }

  CIContext *context = [CIContext contextWithOptions:nil];
  CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
  nlohmann::json manifest = {
      {"schema", "crimson.apple_decode_buffer_dump.v1"},
      {"reason", reason},
      {"source_video", playback.info().path},
      {"frames", nlohmann::json::array()},
  };
  size_t written = 0;
  for (const int64_t frame_number : frames) {
    const auto frame = playback.frameForTarget(frame_number, true);
    if (!frame || !frame->surface) {
      continue;
    }
    CVPixelBufferRef pixel_buffer =
        reinterpret_cast<CVPixelBufferRef>(frame->surface->nativeHandle());
    if (pixel_buffer == nullptr) {
      continue;
    }
    CIImage *image = [CIImage imageWithCVPixelBuffer:pixel_buffer];
    const std::string filename =
        "camera_" + std::to_string(frame_number) + ".png";
    NSURL *url = [NSURL
        fileURLWithPath:[NSString stringWithUTF8String:(output / filename)
                                                           .string()
                                                           .c_str()]];
    NSError *write_error = nil;
    const bool frame_written =
        [context writePNGRepresentationOfImage:image
                                         toURL:url
                                        format:kCIFormatRGBA8
                                    colorSpace:color_space
                                       options:@{}
                                         error:&write_error];
    manifest["frames"].push_back(
        {{"frame", frame_number},
         {"file", frame_written ? filename : std::string{}},
         {"width", frame->metadata.width},
         {"height", frame->metadata.height},
         {"pts", frame->metadata.frame_pts},
         {"error",
          frame_written || write_error == nil
              ? std::string{}
              : std::string(write_error.localizedDescription.UTF8String)}});
    written += frame_written ? 1 : 0;
  }
  CGColorSpaceRelease(color_space);

  std::ofstream manifest_stream(output / "manifest.json");
  manifest_stream << manifest.dump(2) << '\n';
  if (!manifest_stream || written == 0) {
    if (status != nullptr) {
      *status = written == 0 ? "No buffered frames could be written."
                             : "Could not write decode dump manifest.";
    }
    return false;
  }
  if (status != nullptr) {
    *status = "Wrote " + std::to_string(written) + " frame images to " +
              output.string();
  }
  return true;
}

bool launchReplacementSession(const AppleSessionRelaunchRequest &request,
                              const char *executable_path, std::string *error) {
  if (!request.requested || executable_path == nullptr ||
      (request.video_path.empty() &&
       request.recording_clip_index_path.empty())) {
    return false;
  }
  NSMutableArray<NSString *> *arguments = [NSMutableArray array];
  if (!request.video_path.empty()) {
    [arguments addObject:@"--video"];
    [arguments
        addObject:[NSString stringWithUTF8String:request.video_path.c_str()]];
  } else {
    [arguments addObject:@"--recording-clip-index"];
    [arguments
        addObject:[NSString
                      stringWithUTF8String:request.recording_clip_index_path
                                               .c_str()]];
  }
  if (!request.zarr_path.empty()) {
    [arguments addObject:@"--zarr"];
    [arguments
        addObject:[NSString stringWithUTF8String:request.zarr_path.c_str()]];
  }
  if (!request.stimulus_video_path.empty()) {
    [arguments addObject:@"--stimulus-video"];
    [arguments
        addObject:[NSString stringWithUTF8String:request.stimulus_video_path
                                                     .c_str()]];
  }
  [arguments addObject:@"--video-buffer-size"];
  [arguments
      addObject:[NSString
                    stringWithFormat:@"%d", request.video_buffer_capacity]];
  [arguments addObject:@"--stimulus-buffer-size"];
  [arguments
      addObject:[NSString
                    stringWithFormat:@"%d", request.stimulus_buffer_capacity]];
  NSTask *task = [[NSTask alloc] init];
  task.executableURL =
      [NSURL fileURLWithPath:[NSString stringWithUTF8String:executable_path]];
  task.arguments = arguments;
  NSError *launch_error = nil;
  if (![task launchAndReturnError:&launch_error]) {
    if (error != nullptr) {
      *error = launch_error == nil
                   ? "Could not launch the replacement Crimson session."
                   : launch_error.localizedDescription.UTF8String;
    }
    return false;
  }
  return true;
}

std::string recordingWindowName(const std::string &path) {
  if (path.empty()) {
    return "Camera";
  }
  const size_t separator = path.find_last_of("/\\");
  const size_t start = separator == std::string::npos ? 0 : separator + 1;
  const size_t extension = path.find_last_of('.');
  const size_t end = extension == std::string::npos || extension < start
                         ? path.size()
                         : extension;
  const std::string name = path.substr(start, end - start);
  return name.empty() ? "Camera" : name;
}

std::optional<LaunchOptions> parseOptions(int argc, const char *const *argv) {
  LaunchOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    const auto ui_reference_argument =
        crimson::ui_reference::consumeLaunchArgument(
            argc, argv, i, options.ui_reference, kUiReferenceParsePolicy);
    if (ui_reference_argument.status ==
        crimson::ui_reference::ArgumentParseStatus::Error) {
      std::fprintf(stderr, "%s\n", ui_reference_argument.error.c_str());
      return std::nullopt;
    }
    if (ui_reference_argument.status ==
        crimson::ui_reference::ArgumentParseStatus::Consumed) {
      continue;
    }
    if (argument == "--smoke") {
      options.smoke = true;
      continue;
    }
    if (argument == "--validate-metal") {
      options.validate_metal = true;
      continue;
    }
    if (argument == "--no-subject-masks") {
      options.subject_masks_enabled = false;
      continue;
    }
    if (argument == "--show-subject-masks") {
      options.show_subject_masks = true;
      options.subject_masks_enabled = true;
      continue;
    }
    if (argument == "--no-subject-shapes") {
      options.subject_shapes_enabled = false;
      continue;
    }
    if (argument == "--no-eye-geometry") {
      options.eye_geometry_enabled = false;
      continue;
    }
    if (argument == "--no-eye-angle-timeline") {
      options.eye_angle_timeline_enabled = false;
      continue;
    }
    if (argument == "--no-motion-timeline") {
      options.motion_timeline_enabled = false;
      continue;
    }
    if (argument == "--no-swim-bout-timeline") {
      options.swim_bout_timeline_enabled = false;
      continue;
    }
    if (argument == "--no-tail-kinematics-timeline") {
      options.tail_kinematics_timeline_enabled = false;
      continue;
    }
    if (argument == "--no-stimulus-context-timeline") {
      options.stimulus_context_timeline_enabled = false;
      continue;
    }
    if (argument == "--show-analysis-timeline") {
      options.show_analysis_timeline = true;
      continue;
    }
    if (argument == "--show-eye-angle-timeline") {
      options.show_analysis_timeline = true;
      options.show_eye_angle_timeline = true;
      continue;
    }
    if (argument == "--show-stimulus-timeline") {
      options.show_analysis_timeline = true;
      options.show_stimulus_timeline = true;
      continue;
    }
    if (argument == "--require-motion-timeline") {
      options.require_motion_timeline = true;
      continue;
    }
    if (argument == "--require-swim-bout-timeline") {
      options.require_swim_bout_timeline = true;
      continue;
    }
    if (argument == "--require-eye-angle-timeline") {
      options.require_eye_angle_timeline = true;
      continue;
    }
    if (argument == "--require-tail-kinematics-timeline") {
      options.require_tail_kinematics_timeline = true;
      continue;
    }
    if (argument == "--require-stimulus-context-timeline") {
      options.require_stimulus_context_timeline = true;
      continue;
    }
    if (argument == "--video") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Missing value for --video\n");
        return std::nullopt;
      }
      options.video_path = argv[++i];
      continue;
    }
    if (argument == "--recording-clip-index") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Missing value for --recording-clip-index\n");
        return std::nullopt;
      }
      options.recording_clip_index_path = argv[++i];
      continue;
    }
    if (argument == "--zarr") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Missing value for --zarr\n");
        return std::nullopt;
      }
      options.zarr_path = argv[++i];
      continue;
    }
    if (argument == "--stimulus-video") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Missing value for --stimulus-video\n");
        return std::nullopt;
      }
      options.stimulus_video_path = argv[++i];
      continue;
    }
    if (argument == "--video-buffer-size") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Missing value for --video-buffer-size\n");
        return std::nullopt;
      }
      const auto capacity = parsePositiveInt(argv[++i]);
      if (!capacity || *capacity < 2 || *capacity > 256) {
        std::fprintf(stderr,
                     "Invalid --video-buffer-size; expected 2 through 256\n");
        return std::nullopt;
      }
      options.video_buffer_capacity = static_cast<size_t>(*capacity);
      continue;
    }
    if (argument == "--stimulus-buffer-size") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Missing value for --stimulus-buffer-size\n");
        return std::nullopt;
      }
      const auto capacity = parsePositiveInt(argv[++i]);
      if (!capacity || *capacity < 2 || *capacity > 64) {
        std::fprintf(stderr,
                     "Invalid --stimulus-buffer-size; expected 2 through 64\n");
        return std::nullopt;
      }
      options.stimulus_buffer_capacity = static_cast<size_t>(*capacity);
      continue;
    }
    if (argument == "--start-paused") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Missing value for --start-paused\n");
        return std::nullopt;
      }
      options.start_paused_frame = parseNonNegativeInt(argv[++i]);
      if (!options.start_paused_frame.has_value()) {
        std::fprintf(stderr,
                     "Invalid --start-paused value; expected a frame >= 0\n");
        return std::nullopt;
      }
      continue;
    }
    if (argument == "--stimulus-run") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Missing value for --stimulus-run\n");
        return std::nullopt;
      }
      options.stimulus_run = argv[++i];
      continue;
    }
    if (argument == "--detection-run") {
      if (i + 1 >= argc || argv[i + 1][0] == '\0') {
        std::fprintf(stderr, "Missing value for --detection-run\n");
        return std::nullopt;
      }
      options.detection_run = argv[++i];
      continue;
    }
    if (argument == "--refined-detection-run") {
      if (i + 1 >= argc || argv[i + 1][0] == '\0') {
        std::fprintf(stderr, "Missing value for --refined-detection-run\n");
        return std::nullopt;
      }
      if (!options.refined_detection_run.empty()) {
        std::fprintf(stderr,
                     "Only one refined-detection run may be requested\n");
        return std::nullopt;
      }
      options.refined_detection_run = argv[++i];
      continue;
    }
    if (argument == "--benchmark-refined-detection-run") {
      if (i + 1 >= argc || argv[i + 1][0] == '\0') {
        std::fprintf(stderr,
                     "Missing value for --benchmark-refined-detection-run\n");
        return std::nullopt;
      }
      if (!options.refined_detection_run.empty()) {
        std::fprintf(stderr,
                     "Only one refined-detection run may be requested\n");
        return std::nullopt;
      }
      options.refined_detection_run = argv[++i];
      options.allow_selector_ineligible_refined_run = true;
      continue;
    }
    if (argument == "--benchmark-keypoint-v2-raw") {
      if (i + 3 >= argc) {
        std::fprintf(stderr, "--benchmark-keypoint-v2-raw requires ARCHIVE RUN "
                             "MANIFEST_DIGEST\n");
        return std::nullopt;
      }
      options.keypoint_v2.raw = {argv[i + 1], argv[i + 2], argv[i + 3]};
      options.keypoint_v2.allow_selector_ineligible = true;
      i += 3;
      continue;
    }
    if (argument == "--benchmark-subject-mask-v1") {
      if (i + 2 >= argc) {
        std::fprintf(stderr, "--benchmark-subject-mask-v1 requires RUN "
                             "MANIFEST_PAYLOAD_DIGEST\n");
        return std::nullopt;
      }
      options.subject_mask_run = argv[i + 1];
      options.subject_mask_manifest_payload_digest = argv[i + 2];
      options.allow_selector_ineligible_subject_mask_run = true;
      options.require_subject_mask_v1 = true;
      options.subject_masks_enabled = true;
      i += 2;
      continue;
    }
    if (argument == "--benchmark-subject-mask-presentation-cache-v1") {
      if (i + 3 >= argc) {
        std::fprintf(stderr,
                     "--benchmark-subject-mask-presentation-cache-v1 requires "
                     "ARCHIVE RUN MANIFEST_PAYLOAD_DIGEST\n");
        return std::nullopt;
      }
      options.subject_mask_presentation_cache_path = argv[i + 1];
      options.subject_mask_presentation_cache_run = argv[i + 2];
      options.subject_mask_presentation_cache_manifest_payload_digest =
          argv[i + 3];
      options.subject_mask_contour_only = true;
      options.subject_masks_enabled = true;
      i += 3;
      continue;
    }
    if (argument == "--benchmark-keypoint-v2-quality") {
      if (i + 3 >= argc) {
        std::fprintf(stderr,
                     "--benchmark-keypoint-v2-quality requires ARCHIVE RUN "
                     "MANIFEST_DIGEST\n");
        return std::nullopt;
      }
      options.keypoint_v2.quality = {argv[i + 1], argv[i + 2], argv[i + 3]};
      options.keypoint_v2.allow_selector_ineligible = true;
      i += 3;
      continue;
    }
    if (argument == "--benchmark-keypoint-v2-refined") {
      if (i + 3 >= argc) {
        std::fprintf(stderr,
                     "--benchmark-keypoint-v2-refined requires ARCHIVE RUN "
                     "MANIFEST_DIGEST\n");
        return std::nullopt;
      }
      options.keypoint_v2.refined = {argv[i + 1], argv[i + 2], argv[i + 3]};
      options.keypoint_v2.allow_selector_ineligible = true;
      i += 3;
      continue;
    }
    if (argument == "--benchmark-keypoint-v2-body-frame") {
      if (i + 3 >= argc) {
        std::fprintf(stderr,
                     "--benchmark-keypoint-v2-body-frame requires ARCHIVE RUN "
                     "MANIFEST_DIGEST\n");
        return std::nullopt;
      }
      options.keypoint_v2.body_frame = {argv[i + 1], argv[i + 2], argv[i + 3]};
      options.keypoint_v2.allow_selector_ineligible = true;
      i += 3;
      continue;
    }
    if (argument == "--swim-bout-run") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Missing value for --swim-bout-run\n");
        return std::nullopt;
      }
      options.swim_bout_run = argv[++i];
      continue;
    }
    if (argument == "--crop-run") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Missing value for --crop-run\n");
        return std::nullopt;
      }
      options.crop_run = argv[++i];
      continue;
    }
    if (argument == "--video-smoke") {
      if (i + 1 >= argc ||
          !parseFrameRange(argv[++i], &options.video_smoke_start,
                           &options.video_smoke_end)) {
        std::fprintf(stderr,
                     "Invalid --video-smoke value; expected START:END\n");
        return std::nullopt;
      }
      options.smoke = true;
      options.video_smoke = true;
      continue;
    }
    if (argument == "--stimulus-smoke") {
      if (i + 1 >= argc ||
          !parseFrameRange(argv[++i], &options.video_smoke_start,
                           &options.video_smoke_end)) {
        std::fprintf(stderr,
                     "Invalid --stimulus-smoke value; expected START:END\n");
        return std::nullopt;
      }
      options.smoke = true;
      options.video_smoke = true;
      options.stimulus_smoke = true;
      continue;
    }
    if (argument == "--crop-smoke") {
      if (i + 1 >= argc ||
          !parseFrameRange(argv[++i], &options.video_smoke_start,
                           &options.video_smoke_end)) {
        std::fprintf(stderr,
                     "Invalid --crop-smoke value; expected START:END\n");
        return std::nullopt;
      }
      options.smoke = true;
      options.video_smoke = true;
      options.crop_smoke = true;
      continue;
    }
    if (argument == "--multistream-smoke") {
      if (i + 1 >= argc ||
          !parseFrameRange(argv[++i], &options.video_smoke_start,
                           &options.video_smoke_end) ||
          options.video_smoke_end - options.video_smoke_start < 20) {
        std::fprintf(
            stderr,
            "Invalid --multistream-smoke value; expected START:END with a "
            "span of at least 20 frames\n");
        return std::nullopt;
      }
      options.smoke = true;
      options.video_smoke = true;
      options.stimulus_smoke = true;
      options.crop_smoke = true;
      options.multistream_smoke = true;
      continue;
    }
    if (argument == "--crop-source") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Missing value for --crop-source\n");
        return std::nullopt;
      }
      const std::string source = argv[++i];
      if (source == "acquisition") {
        options.crop_preference =
            crimson::crop::CropSourcePreference::PreferAcquisitionVideo;
      } else if (source == "geometry") {
        options.crop_preference =
            crimson::crop::CropSourcePreference::PreferLiveGeometry;
      } else {
        std::fprintf(stderr,
                     "Invalid --crop-source value; expected acquisition or "
                     "geometry\n");
        return std::nullopt;
      }
      continue;
    }
    if (argument == "--smoke-frames") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Missing value for --smoke-frames\n");
        return std::nullopt;
      }
      const auto frames = parsePositiveInt(argv[++i]);
      if (!frames) {
        std::fprintf(
            stderr, "Invalid --smoke-frames value; expected an integer >= 1\n");
        return std::nullopt;
      }
      options.smoke = true;
      options.smoke_frames = *frames;
      continue;
    }
    if (argument.rfind("-psn_", 0) == 0) {
      continue;
    }
    std::fprintf(stderr, "Unknown argument: %s\n", argument.c_str());
    return std::nullopt;
  }
  if (const auto ui_reference_error =
          crimson::ui_reference::validateLaunchOptions(
              options.ui_reference, kUiReferenceParsePolicy)) {
    std::fprintf(stderr, "%s\n", ui_reference_error->c_str());
    return std::nullopt;
  }
  if (options.ui_reference.enabled) {
    if (options.smoke || options.validate_metal) {
      std::fprintf(stderr,
                   "UI reference capture cannot be combined with smoke or "
                   "Metal validation modes\n");
      return std::nullopt;
    }
    const bool empty_state =
        options.ui_reference.state == AppleUiReferenceState::Empty;
    const bool has_recording_media = !options.video_path.empty() ||
                                     !options.recording_clip_index_path.empty();
    if (empty_state && (has_recording_media || !options.zarr_path.empty())) {
      std::fprintf(stderr,
                   "The empty UI reference state cannot load video or Zarr\n");
      return std::nullopt;
    }
    if (!empty_state && (!has_recording_media || options.zarr_path.empty())) {
      std::fprintf(stderr,
                   "Loaded UI reference states require recording media and "
                   "--zarr\n");
      return std::nullopt;
    }
    if (!empty_state) {
      if (options.start_paused_frame &&
          *options.start_paused_frame != options.ui_reference.target_frame) {
        std::fprintf(stderr,
                     "--start-paused must match --ui-reference-frame\n");
        return std::nullopt;
      }
      options.start_paused_frame = options.ui_reference.target_frame;
    }
    if (options.ui_reference.state == AppleUiReferenceState::CropPreview) {
      options.crop_preference =
          crimson::crop::CropSourcePreference::PreferLiveGeometry;
    }
    if (options.ui_reference.state == AppleUiReferenceState::AnalysisEye) {
      options.show_analysis_timeline = true;
      options.show_eye_angle_timeline = true;
      options.require_eye_angle_timeline = true;
    }
    if (options.ui_reference.state == AppleUiReferenceState::StimulusOverlay) {
      options.require_stimulus_context_timeline = true;
    }
  }
  if (options.smoke && options.validate_metal) {
    std::fprintf(stderr,
                 "--smoke and --validate-metal cannot be used together\n");
    return std::nullopt;
  }
  if (!options.video_path.empty() &&
      !options.recording_clip_index_path.empty()) {
    std::fprintf(stderr,
                 "--video and --recording-clip-index are mutually exclusive\n");
    return std::nullopt;
  }
  const bool has_recording_media =
      !options.video_path.empty() || !options.recording_clip_index_path.empty();
  if (options.video_smoke && !has_recording_media) {
    std::fprintf(stderr,
                 "--video-smoke requires --video or --recording-clip-index\n");
    return std::nullopt;
  }
  if (!options.zarr_path.empty() && !has_recording_media) {
    std::fprintf(stderr, "--zarr requires --video or --recording-clip-index\n");
    return std::nullopt;
  }
  if (options.start_paused_frame.has_value() && !has_recording_media) {
    std::fprintf(stderr,
                 "--start-paused requires --video or --recording-clip-index\n");
    return std::nullopt;
  }
  if (options.start_paused_frame.has_value() && options.video_smoke) {
    std::fprintf(stderr,
                 "--start-paused cannot be combined with a video smoke\n");
    return std::nullopt;
  }
  if (!options.stimulus_run.empty() && options.zarr_path.empty()) {
    std::fprintf(stderr, "--stimulus-run requires --zarr PATH\n");
    return std::nullopt;
  }
  if (!options.detection_run.empty() && options.zarr_path.empty()) {
    std::fprintf(stderr, "--detection-run requires --zarr PATH\n");
    return std::nullopt;
  }
  if (!options.refined_detection_run.empty() && options.zarr_path.empty()) {
    std::fprintf(stderr, "A refined-detection run requires --zarr PATH\n");
    return std::nullopt;
  }
  const bool any_keypoint_v2_artifact = !options.keypoint_v2.raw.empty() ||
                                        !options.keypoint_v2.quality.empty() ||
                                        !options.keypoint_v2.refined.empty() ||
                                        !options.keypoint_v2.body_frame.empty();
  if (any_keypoint_v2_artifact && (!options.keypoint_v2.raw.complete() ||
                                   !options.keypoint_v2.quality.complete() ||
                                   !options.keypoint_v2.body_frame.complete() ||
                                   (!options.keypoint_v2.refined.empty() &&
                                    !options.keypoint_v2.refined.complete()))) {
    std::fprintf(stderr,
                 "Keypoint v2 requires complete raw, quality, and body-frame "
                 "artifacts; refined must be complete when requested\n");
    return std::nullopt;
  }
  if (any_keypoint_v2_artifact && options.zarr_path.empty()) {
    std::fprintf(stderr, "Keypoint v2 benchmark options require --zarr PATH\n");
    return std::nullopt;
  }
  if (!options.subject_mask_presentation_cache_path.empty() &&
      (!options.require_subject_mask_v1 || options.subject_mask_run.empty() ||
       options.subject_mask_manifest_payload_digest.empty() ||
       !options.allow_selector_ineligible_subject_mask_run)) {
    std::fprintf(stderr,
                 "A subject-mask presentation cache requires an explicit "
                 "--benchmark-subject-mask-v1 source run and digest\n");
    return std::nullopt;
  }
  if (!options.stimulus_video_path.empty() && options.zarr_path.empty()) {
    std::fprintf(stderr, "--stimulus-video requires --zarr PATH\n");
    return std::nullopt;
  }
  if (!options.crop_run.empty() && options.zarr_path.empty()) {
    std::fprintf(stderr, "--crop-run requires --zarr PATH\n");
    return std::nullopt;
  }
  if (options.stimulus_smoke && options.zarr_path.empty()) {
    std::fprintf(stderr, "--stimulus-smoke requires --zarr PATH\n");
    return std::nullopt;
  }
  if (options.crop_smoke && options.zarr_path.empty()) {
    std::fprintf(stderr, "--crop-smoke requires --zarr PATH\n");
    return std::nullopt;
  }
  if ((options.show_analysis_timeline || options.require_motion_timeline ||
       options.require_swim_bout_timeline ||
       options.require_eye_angle_timeline ||
       options.require_stimulus_context_timeline) &&
      options.zarr_path.empty()) {
    std::fprintf(stderr, "Analysis timeline options require --zarr PATH\n");
    return std::nullopt;
  }
  if (options.require_tail_kinematics_timeline && options.zarr_path.empty()) {
    std::fprintf(stderr,
                 "Tail-kinematics timeline options require --zarr PATH\n");
    return std::nullopt;
  }
  if (options.require_stimulus_context_timeline &&
      !options.stimulus_context_timeline_enabled) {
    std::fprintf(stderr, "--require-stimulus-context-timeline conflicts with "
                         "--no-stimulus-context-timeline\n");
    return std::nullopt;
  }
  if (options.require_motion_timeline && !options.motion_timeline_enabled) {
    std::fprintf(stderr, "--require-motion-timeline conflicts with "
                         "--no-motion-timeline\n");
    return std::nullopt;
  }
  if (options.require_swim_bout_timeline &&
      !options.swim_bout_timeline_enabled) {
    std::fprintf(stderr, "--require-swim-bout-timeline conflicts with "
                         "--no-swim-bout-timeline\n");
    return std::nullopt;
  }
  if (!options.swim_bout_run.empty() && options.zarr_path.empty()) {
    std::fprintf(stderr, "--swim-bout-run requires --zarr PATH\n");
    return std::nullopt;
  }
  if (options.require_eye_angle_timeline &&
      !options.eye_angle_timeline_enabled) {
    std::fprintf(stderr, "--require-eye-angle-timeline conflicts with "
                         "--no-eye-angle-timeline\n");
    return std::nullopt;
  }
  if (options.require_tail_kinematics_timeline &&
      !options.tail_kinematics_timeline_enabled) {
    std::fprintf(stderr, "--require-tail-kinematics-timeline conflicts with "
                         "--no-tail-kinematics-timeline\n");
    return std::nullopt;
  }
  return options;
}

std::optional<std::string> bundledFontPath(const char *resource) {
  NSBundle *bundle = [NSBundle mainBundle];
  NSString *resource_name = [NSString stringWithUTF8String:resource];
  NSURL *url = [bundle URLForResource:resource_name
                        withExtension:@"ttf"
                         subdirectory:@"fonts"];
  if (url == nil || !url.isFileURL) {
    return std::nullopt;
  }
  const char *path = url.fileSystemRepresentation;
  if (path == nullptr || *path == '\0') {
    return std::nullopt;
  }
  return std::string(path);
}

bool loadBundledFonts(ImGuiIO &io, std::string *roboto_path = nullptr) {
  const auto text_font = bundledFontPath("Roboto-Regular");
  const auto icon_font = bundledFontPath("forkawesome-webfont");
  if (!text_font || !icon_font ||
      io.Fonts->AddFontFromFileTTF(text_font->c_str(), 15.0f) == nullptr) {
    return false;
  }
  ImFontConfig icon_config;
  icon_config.MergeMode = true;
  icon_config.PixelSnapH = true;
  static const ImWchar icon_ranges[] = {ICON_MIN_FK, ICON_MAX_16_FK, 0};
  if (io.Fonts->AddFontFromFileTTF(icon_font->c_str(), 15.0f, &icon_config,
                                   icon_ranges) == nullptr) {
    return false;
  }
  if (roboto_path != nullptr) {
    *roboto_path = *text_font;
  }
  return true;
}

std::optional<std::string> workspaceIniPath() {
  NSArray<NSURL *> *application_support = [[NSFileManager defaultManager]
      URLsForDirectory:NSApplicationSupportDirectory
             inDomains:NSUserDomainMask];
  if (application_support.count == 0) {
    return std::nullopt;
  }
  NSURL *directory =
      [application_support.firstObject URLByAppendingPathComponent:@"Crimson"
                                                       isDirectory:YES];
  NSError *error = nil;
  if (![[NSFileManager defaultManager] createDirectoryAtURL:directory
                                withIntermediateDirectories:YES
                                                 attributes:nil
                                                      error:&error]) {
    std::fprintf(stderr, "[MacShell] Workspace persistence unavailable: %s\n",
                 error.localizedDescription.UTF8String);
    return std::nullopt;
  }
  NSURL *file = [directory URLByAppendingPathComponent:@"imgui.ini"];
  const char *path = file.fileSystemRepresentation;
  return path != nullptr && *path != '\0' ? std::optional<std::string>(path)
                                          : std::nullopt;
}

void configureStyle() {
  ImGui::StyleColorsClassic();
  ImPlot::GetStyle().Colors[ImPlotCol_Crosshairs] =
      ImVec4(0.58f, 0.28f, 0.78f, 1.0f);
}

void drawShellSurface(const std::array<float, 120> &frame_times_ms,
                      int frame_time_count, int framebuffer_width,
                      int framebuffer_height) {
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;

  ImGui::Begin("Crimson", nullptr, flags);
  ImGui::TextUnformatted("Crimson");
  ImGui::Separator();
  ImGui::Text("Renderer: Metal");
  ImGui::Text("Window system: GLFW / Cocoa");
  ImGui::Text("Architecture: arm64");
  ImGui::Text("Framebuffer: %d x %d", framebuffer_width, framebuffer_height);
  ImGui::Text("Revision: %s", CRIMSON_GIT_COMMIT);

  const int sample_count =
      std::min(frame_time_count, static_cast<int>(frame_times_ms.size()));
  if (sample_count > 1 && ImPlot::BeginPlot("Frame time", ImVec2(-1.0f, 220.0f),
                                            ImPlotFlags_NoLegend)) {
    ImPlot::SetupAxes("Frame", "ms", ImPlotAxisFlags_AutoFit,
                      ImPlotAxisFlags_AutoFit);
    ImPlot::PlotLine("frame_ms", frame_times_ms.data(), sample_count);
    ImPlot::EndPlot();
  }
  ImGui::End();
}

bool renderAppleLoadingFrame(GLFWwindow *window, CAMetalLayer *layer,
                             id<MTLCommandQueue> command_queue,
                             const std::string &phase, size_t completed,
                             size_t total) {
  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(window, &width, &height);
  if (width <= 0 || height <= 0) {
    return true;
  }
  layer.drawableSize = CGSizeMake(width, height);
  id<CAMetalDrawable> drawable = [layer nextDrawable];
  if (drawable == nil) {
    return true;
  }

  MTLRenderPassDescriptor *render_pass = [MTLRenderPassDescriptor new];
  render_pass.colorAttachments[0].texture = drawable.texture;
  render_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  render_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  render_pass.colorAttachments[0].clearColor =
      MTLClearColorMake(0.055, 0.060, 0.065, 1.0);
  id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
  id<MTLRenderCommandEncoder> encoder =
      [command_buffer renderCommandEncoderWithDescriptor:render_pass];
  if (command_buffer == nil || encoder == nil) {
    return false;
  }

  ImGui_ImplMetal_NewFrame(render_pass);
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  ImGui::Begin("Crimson loading", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
  const float content_width =
      std::max(1.0f, std::min(420.0f, viewport->WorkSize.x - 48.0f));
  const float content_height = 92.0f;
  ImGui::SetCursorPos(
      ImVec2(std::max(24.0f, (viewport->WorkSize.x - content_width) * 0.5f),
             std::max(24.0f, (viewport->WorkSize.y - content_height) * 0.5f)));
  ImGui::BeginGroup();
  ImGui::TextUnformatted("Crimson");
  ImGui::Spacing();
  ImGui::TextUnformatted(phase.empty() ? "Preparing session" : phase.c_str());
  const float progress = total == 0 ? 0.0f
                                    : std::clamp(static_cast<float>(completed) /
                                                     static_cast<float>(total),
                                                 0.0f, 1.0f);
  ImGui::ProgressBar(progress, ImVec2(content_width, 5.0f), "");
  if (total > 0) {
    ImGui::TextDisabled("%zu of %zu stages", std::min(completed, total), total);
  }
  ImGui::EndGroup();
  ImGui::End();
  ImGui::Render();
  ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), command_buffer, encoder);
  [encoder endEncoding];
  [command_buffer presentDrawable:drawable];
  [command_buffer commit];
  return true;
}

int runHeadlessMetalValidation() {
  constexpr NSUInteger width = 384;
  constexpr NSUInteger height = 240;

  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  id<MTLCommandQueue> command_queue = [device newCommandQueue];
  if (device == nil || command_queue == nil) {
    std::fprintf(
        stderr, "[MacMetalHeadless] No usable Metal device or command queue\n");
    return 20;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImPlot::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DisplaySize =
      ImVec2(static_cast<float>(width), static_cast<float>(height));
  io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
  io.DeltaTime = 1.0f / 60.0f;
  configureStyle();

  const bool bundled_font_loaded = loadBundledFonts(io);
  if (!bundled_font_loaded) {
    std::fprintf(stderr, "[MacMetalHeadless] Failed to load bundled font\n");
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    return 21;
  }
  if (!ImGui_ImplMetal_Init(device)) {
    std::fprintf(stderr,
                 "[MacMetalHeadless] Failed to initialize ImGui Metal\n");
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    return 22;
  }

  MTLTextureDescriptor *texture_descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                   width:width
                                  height:height
                               mipmapped:NO];
  texture_descriptor.usage =
      MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  texture_descriptor.storageMode = MTLStorageModeShared;
  id<MTLTexture> texture = [device newTextureWithDescriptor:texture_descriptor];
  AppleMetalPresentationTexture presentation_texture;
  presentation_texture.reset(texture);

  MTLRenderPassDescriptor *render_pass = [MTLRenderPassDescriptor new];
  render_pass.colorAttachments[0].texture = texture;
  render_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  render_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  render_pass.colorAttachments[0].clearColor =
      MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

  id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
  id<MTLRenderCommandEncoder> encoder =
      [command_buffer renderCommandEncoderWithDescriptor:render_pass];
  if (texture == nil || command_buffer == nil || encoder == nil ||
      presentation_texture.descriptor().backend != PresentationBackend::Metal ||
      presentation_texture.descriptor().pixel_format !=
          FramePixelFormat::BGRA8 ||
      presentation_texture.descriptor().width != static_cast<int>(width) ||
      presentation_texture.descriptor().height != static_cast<int>(height) ||
      presentation_texture.nativeHandle() == 0) {
    std::fprintf(
        stderr,
        "[MacMetalHeadless] Failed to allocate offscreen render state\n");
    ImGui_ImplMetal_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    return 23;
  }

  ImGui_ImplMetal_NewFrame(render_pass);
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::Begin("Crimson headless validation", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoSavedSettings);
  ImGui::TextUnformatted("Crimson Metal validation");
  ImGui::Text("Revision: %s", CRIMSON_GIT_COMMIT);
  const float samples[] = {1.0f, 2.0f, 1.5f, 3.0f, 2.25f};
  if (ImPlot::BeginPlot("Offscreen plot", ImVec2(-1.0f, 140.0f),
                        ImPlotFlags_NoLegend)) {
    ImPlot::PlotLine("sample", samples, 5);
    ImPlot::EndPlot();
  }
  ImGui::End();
  ImGui::Render();
  ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), command_buffer, encoder);
  [encoder endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];

  const bool command_completed =
      command_buffer.status == MTLCommandBufferStatusCompleted;
  if (!command_completed) {
    std::fprintf(stderr, "[MacMetalHeadless] Metal command failed: %s\n",
                 command_buffer.error.localizedDescription.UTF8String);
  }

  std::vector<unsigned char> pixels(width * height * 4);
  [texture getBytes:pixels.data()
        bytesPerRow:width * 4
         fromRegion:MTLRegionMake2D(0, 0, width, height)
        mipmapLevel:0];
  size_t colored_pixels = 0;
  for (size_t i = 0; i < pixels.size(); i += 4) {
    if (pixels[i] != 0 || pixels[i + 1] != 0 || pixels[i + 2] != 0) {
      ++colored_pixels;
    }
  }

  ImGui_ImplMetal_Shutdown();
  ImPlot::DestroyContext();
  ImGui::DestroyContext();

  if (!command_completed || colored_pixels == 0) {
    std::fprintf(stderr,
                 "[MacMetalHeadless] FAIL completed=%s colored_pixels=%zu\n",
                 command_completed ? "true" : "false", colored_pixels);
    return 24;
  }
  std::printf("[MacMetalHeadless] PASS texture=%lux%lu colored_pixels=%zu "
              "font=bundled renderer=Metal\n",
              static_cast<unsigned long>(width),
              static_cast<unsigned long>(height), colored_pixels);
  return 0;
}

const char *canonicalDetectionResidencyDecisionName(
    CanonicalDetectionResidencyState state) {
  switch (state) {
  case CanonicalDetectionResidencyState::Disabled:
    return "disabled";
  case CanonicalDetectionResidencyState::Ineligible:
    return "rejected_budget";
  case CanonicalDetectionResidencyState::Loading:
    return "building";
  case CanonicalDetectionResidencyState::Ready:
    return "resident";
  case CanonicalDetectionResidencyState::Cancelled:
    return "cancelled";
  case CanonicalDetectionResidencyState::Failed:
    return "fallback_paged";
  }
  return "unknown";
}

void reportCanonicalDetectionResidency(
    const char *event,
    const crimson::zarr::CanonicalDetectionDescriptor &descriptor,
    const CanonicalDetectionResidencyMetrics &metrics) {
  std::printf(
      "[AppleCanonicalDetectionResidency] event=%s state=%s decision=%s "
      "surface=%s run=%s attempts=%llu candidate_bytes=%llu "
      "budget_bytes=%llu chunk_bytes=%llu planned=%llu completed=%llu "
      "source_bytes=%llu retained_bytes=%llu stale=%llu failed=%llu "
      "publications=%llu elapsed_ms=%.1f max_chunk_ms=%.1f error=%s\n",
      event, canonicalDetectionResidencyStateName(metrics.state),
      canonicalDetectionResidencyDecisionName(metrics.state),
      descriptor.surface_kind ==
              crimson::zarr::DetectionSurfaceKind::RefinedSnapshotV1
          ? "refined_v1"
          : "canonical_raw_v1",
      descriptor.run_name.c_str(),
      static_cast<unsigned long long>(metrics.attempts),
      static_cast<unsigned long long>(metrics.decoded_hot_bytes),
      static_cast<unsigned long long>(metrics.maximum_resident_bytes),
      static_cast<unsigned long long>(metrics.maximum_chunk_decoded_bytes),
      static_cast<unsigned long long>(metrics.planned_chunks),
      static_cast<unsigned long long>(metrics.completed_chunks),
      static_cast<unsigned long long>(metrics.decoded_source_bytes),
      static_cast<unsigned long long>(metrics.retained_bytes),
      static_cast<unsigned long long>(metrics.stale_chunks),
      static_cast<unsigned long long>(metrics.failed_chunks),
      static_cast<unsigned long long>(metrics.publications), metrics.elapsed_ms,
      metrics.maximum_chunk_ms, metrics.last_error.c_str());
}

} // namespace

int main(int argc, char **argv) {
  const auto options = parseOptions(argc, argv);
  if (!options) {
    return 2;
  }
  if (options->validate_metal) {
    return runHeadlessMetalValidation();
  }

  glfwSetErrorCallback(glfwErrorCallback);
  if (!glfwInit()) {
    std::fprintf(stderr, "[MacShell] Failed to initialize GLFW\n");
    return 3;
  }

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER,
                 options->ui_reference.enabled ? GLFW_FALSE : GLFW_TRUE);
  const int initial_window_width = options->ui_reference.enabled
                                       ? options->ui_reference.logical_width
                                       : 1280;
  const int initial_window_height = options->ui_reference.enabled
                                        ? options->ui_reference.logical_height
                                        : 800;
  GLFWwindow *window = glfwCreateWindow(
      initial_window_width, initial_window_height, "Crimson", nullptr, nullptr);
  if (window == nullptr) {
    std::fprintf(stderr, "[MacShell] Failed to create the Cocoa window\n");
    glfwTerminate();
    return 4;
  }

  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  id<MTLCommandQueue> command_queue = [device newCommandQueue];
  if (device == nil || command_queue == nil) {
    std::fprintf(stderr,
                 "[MacShell] No usable Metal device or command queue\n");
    glfwDestroyWindow(window);
    glfwTerminate();
    return 5;
  }

  NSWindow *cocoa_window = glfwGetCocoaWindow(window);
  CAMetalLayer *layer = [CAMetalLayer layer];
  layer.device = device;
  layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  layer.framebufferOnly = options->ui_reference.enabled ? NO : YES;
  cocoa_window.contentView.layer = layer;
  cocoa_window.contentView.wantsLayer = YES;
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  [NSApp activateIgnoringOtherApps:YES];
  [cocoa_window makeKeyAndOrderFront:nil];

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImPlot::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  const auto workspace_ini_path =
      options->smoke || options->video_smoke || options->ui_reference.enabled
          ? std::nullopt
          : workspaceIniPath();
  io.IniFilename = workspace_ini_path ? workspace_ini_path->c_str() : nullptr;
  if (options->ui_reference.enabled) {
    io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard;
  }
  configureStyle();

  std::string font_path;
  const bool bundled_font_loaded = loadBundledFonts(io, &font_path);
  if (!bundled_font_loaded) {
    std::fprintf(
        stderr, "[MacShell] Failed to load bundled fonts/Roboto-Regular.ttf\n");
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 6;
  }

  if (!ImGui_ImplGlfw_InitForOther(window, true) ||
      !ImGui_ImplMetal_Init(device)) {
    std::fprintf(stderr, "[MacShell] Failed to initialize ImGui backends\n");
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 7;
  }
  crimson::ui::setSemanticCaptureEnabled(ImGui::GetCurrentContext(),
                                         options->ui_reference.enabled);
  auto workspace_layout_profile =
      crimson::macos::workspace::LayoutProfile::Standard;
  if (options->ui_reference.enabled &&
      options->ui_reference.state == AppleUiReferenceState::CropPreview) {
    workspace_layout_profile =
        crimson::macos::workspace::LayoutProfile::CropReference;
  } else if (options->ui_reference.enabled &&
             options->ui_reference.state ==
                 AppleUiReferenceState::StimulusDebug) {
    workspace_layout_profile =
        crimson::macos::workspace::LayoutProfile::StimulusReference;
  }
  setAppleWorkspaceLayoutProfile(workspace_layout_profile);

  std::printf("[MacShell] renderer=Metal window=GLFW/Cocoa architecture=arm64 "
              "revision=%s font=%s workspace=%s\n",
              CRIMSON_GIT_COMMIT, font_path.c_str(),
              workspace_ini_path ? workspace_ini_path->c_str() : "transient");
  crimson::ui_reference::CaptureCoordinator ui_reference_capture(
      {60, options->ui_reference.timeout_seconds});
  bool ui_reference_setup_failed = false;
  if (options->ui_reference.enabled) {
    std::string reference_error;
    if (!crimson::ui_reference::prepareUiReferenceOutput(
            options->ui_reference.ready_file, &reference_error)) {
      std::fprintf(stderr, "[AppleUiReference] START failed: %s\n",
                   reference_error.c_str());
      ui_reference_capture.fail(reference_error);
      ui_reference_setup_failed = true;
      glfwSetWindowShouldClose(window, GLFW_TRUE);
    } else {
      std::printf(
          "[AppleUiReference] START state=%s target_frame=%d size=%dx%d "
          "ready_file=%s timeout_s=%.1f\n",
          appleUiReferenceStateName(options->ui_reference.state),
          options->ui_reference.target_frame,
          options->ui_reference.logical_width,
          options->ui_reference.logical_height,
          options->ui_reference.ready_file.string().c_str(),
          options->ui_reference.timeout_seconds);
    }
  }

  AppleVideoPlaybackBuffer video_playback;
  AppleVideoMetalRenderer video_renderer;
  AppleOverlayMetalRenderer overlay_renderer;
  crimson::playback::PlaybackTransportController video_clock;
  AppleVideoViewerStats viewer_stats;
  crimson::playback::FramePresentationTracker camera_presentation_tracker;
  crimson::workspace::WorkspaceState workspace_state;
  AppleFileBrowserState file_browser_state;
  file_browser_state.video_buffer_capacity =
      static_cast<int>(options->video_buffer_capacity);
  file_browser_state.stimulus_buffer_capacity =
      static_cast<int>(options->stimulus_buffer_capacity);
  file_browser_state.selected_zarr_path = options->zarr_path;
  {
    std::error_code cwd_error;
    const auto cwd = std::filesystem::current_path(cwd_error);
    file_browser_state.path_config =
        LoadUiPathConfig(cwd_error ? std::filesystem::path{} : cwd, argv[0]);
    file_browser_state.persisted_path_config = file_browser_state.path_config;
    file_browser_state.start_folder =
        file_browser_state.path_config.default_start_path;
  }
  AppleCameraViewState camera_view_state;
  AppleFrameInspectPresentationState frame_inspect_presentation;
  AppleDetectionInspectState detection_inspect_state;
  AppleDetectionQualityTimelineControls detection_quality_timeline_controls;
  AppleKeypointInspectState keypoint_inspect_state;
  AppleKeypointQualityTimelineControls keypoint_quality_timeline_controls;
  AppleSubjectMaskInspectState subject_mask_inspect_state;
  AppleSubjectShapeInspectState subject_shape_inspect_state;
  AppleEyeAngleInspectState eye_angle_inspect_state;
  AppleStimulusDebugState stimulus_debug_state;
  crimson::session::SessionLifecycle session_lifecycle;
  crimson::loading::LoadingProgressTracker session_open_progress;
  crimson::session::RecordingOpenWorkflowController recording_open_workflow(
      session_lifecycle, session_open_progress);
  const crimson::session::SessionDescriptor initial_session{
      options->video_path, options->zarr_path, options->stimulus_video_path,
      options->recording_clip_index_path};
  std::string decode_debug_status;
  const std::filesystem::path decode_dump_root = decodeDumpRoot();
  bool show_error_popup = false;
  std::string error_popup_message;
  std::mt19937_64 diagnostic_random{0x4352494d534f4eULL};
  auto &overlay_controls = workspace_state.overlayControls();
  if (options->show_subject_masks) {
    overlay_controls.show_subject_masks = true;
  }
  if (options->ui_reference.enabled) {
    const bool masks_visible =
        options->ui_reference.state == AppleUiReferenceState::Overlays ||
        options->ui_reference.state == AppleUiReferenceState::AnalysisEye;
    overlay_controls.show_subject_masks = masks_visible;
    if (options->ui_reference.state == AppleUiReferenceState::Keypoints) {
      overlay_controls.show_keypoints = true;
      overlay_controls.show_headings = true;
      overlay_controls.show_subject_masks = false;
      overlay_controls.show_eye_geometry = false;
      overlay_controls.show_subject_shape = false;
    } else if (options->ui_reference.state == AppleUiReferenceState::Polar) {
      overlay_controls.show_keypoints = false;
      overlay_controls.show_headings = false;
      overlay_controls.show_subject_masks = false;
      overlay_controls.show_eye_geometry = false;
      overlay_controls.show_subject_shape = false;
      frame_inspect_presentation.roi_inset.visible = false;
      frame_inspect_presentation.show_motion_trail = false;
      frame_inspect_presentation.show_stimulus_inset = false;
      frame_inspect_presentation.polar_inset = {};
    } else if (options->ui_reference.state ==
               AppleUiReferenceState::StimulusOverlay) {
      overlay_controls.show_keypoints = false;
      overlay_controls.show_headings = false;
      overlay_controls.show_subject_masks = false;
      overlay_controls.show_eye_geometry = false;
      overlay_controls.show_subject_shape = false;
      frame_inspect_presentation.roi_inset.visible = false;
      frame_inspect_presentation.show_motion_trail = false;
      frame_inspect_presentation.show_stimulus_inset = false;
      frame_inspect_presentation.polar_inset.show_inset = false;
    }
  }
  crimson::overlay::ReadOnlyOverlayAvailability overlay_availability;
  AppleAnalysisTimelineControls analysis_timeline_controls;
  analysis_timeline_controls.selections = &workspace_state.selections();
  analysis_timeline_controls.open = true;
  analysis_timeline_controls.initial_tab =
      options->show_stimulus_timeline    ? AppleAnalysisTimelineTab::Stimulus
      : options->show_eye_angle_timeline ? AppleAnalysisTimelineTab::EyeAngles
                                         : AppleAnalysisTimelineTab::Motion;
  std::optional<AppleDecodedVideoFrame> current_video_frame;
  std::optional<AppleDecodedVideoFrame> pending_video_frame;
  const bool video_enabled = !options->video_path.empty() ||
                             !options->recording_clip_index_path.empty();
  const std::string camera_window_name = recordingWindowName(
      options->video_path.empty() ? options->recording_clip_index_path
                                  : options->video_path);
  const bool analysis_requested = video_enabled && !options->zarr_path.empty();
  std::vector<crimson::session::SessionReadinessProductRule>
      analysis_readiness_products;
  if (analysis_requested) {
    analysis_readiness_products.push_back(
        {"archive",
         crimson::session::ProductAvailabilityRequirement::Required});
    if (!options->detection_run.empty() ||
        !options->refined_detection_run.empty()) {
      analysis_readiness_products.push_back(
          {"canonical_detection",
           crimson::session::ProductAvailabilityRequirement::Required});
    }
    if (options->keypoint_v2.enabled()) {
      analysis_readiness_products.push_back(
          {"keypoints",
           crimson::session::ProductAvailabilityRequirement::Required});
    }
  }
  crimson::session::SessionOpenReadinessBinding
      initial_session_readiness_binding;
  initial_session_readiness_binding.readiness_products =
      analysis_readiness_products;
  initial_session_readiness_binding.resolved = initial_session;
  if (analysis_requested) {
    initial_session_readiness_binding.transaction_product = "analysis";
    initial_session_readiness_binding.product_ready_phase = "Analysis ready";
    initial_session_readiness_binding.product_failed_phase =
        "Analysis unavailable";
  }
  uint64_t initial_recording_open_generation = 0;
  if (!initial_session.empty()) {
    std::vector<crimson::session::SessionReadinessProductRule> session_products;
    if (video_enabled) {
      session_products.push_back(
          {"media",
           crimson::session::ProductAvailabilityRequirement::Required});
    }
    if (analysis_requested) {
      session_products.push_back(
          {"analysis",
           crimson::session::ProductAvailabilityRequirement::Required});
    }
    recording_open_workflow.begin(
        {initial_session, "Opening session", std::move(session_products)},
        initial_session_readiness_binding);
    initial_recording_open_generation = recording_open_workflow.generation();
    if (video_enabled) {
      recording_open_workflow.startProduct("media", "Opening camera media");
    }
  }
  auto fail_initial_media_open = [&](const std::string &error) {
    if (!recording_open_workflow.active()) {
      return;
    }
    recording_open_workflow.failProduct("media", "Camera media unavailable",
                                        error, "Session unavailable");
  };
  if (options->crop_smoke ||
      (options->ui_reference.enabled &&
       options->ui_reference.state == AppleUiReferenceState::CropPreview)) {
    workspace_state.setWindowRequested(
        crimson::workspace::Window::AdvancedCropPreview, true);
  }
  if (options->stimulus_smoke ||
      (options->ui_reference.enabled &&
       options->ui_reference.state == AppleUiReferenceState::StimulusDebug)) {
    workspace_state.setWindowRequested(crimson::workspace::Window::Stimulus,
                                       true);
  }
  bool stimulus_enabled = false;
  AppleStimulusPlaybackSession stimulus_playback;
  crimson::playback::StimulusPresentationCoordinator stimulus_presentation;
  std::optional<AppleAlignedStimulusFrame> current_stimulus_frame;
  std::optional<AppleAlignedStimulusFrame> candidate_stimulus_frame;
  crimson::zarr::StimulusFrameResolution current_stimulus_resolution;
  crimson::zarr::StimulusFrameResolution candidate_stimulus_resolution;
  bool stimulus_candidate_ready = false;
  std::string stimulus_error;
  bool stimulus_failed = false;
  bool stimulus_smoke_end_satisfied = false;
  bool pending_camera_discontinuity = true;
  bool viewer_presentation_discontinuity = true;
  int64_t last_stimulus_camera_request = -1;
  AppleAcquisitionCropPlaybackSession crop_playback;
  std::unique_ptr<crimson::zarr::AnalysisCropGeometryRepository>
      analysis_crop_geometry;
  auto analysis_data_scheduler =
      std::make_shared<crimson::data::DataAccessScheduler>(64, 4, 1, 1);
  AppleAnalysisRepositoryLoader analysis_loader;
  std::string analysis_loading_start_error;
  bool analysis_loading_failure_reported = false;
  std::unique_ptr<AppleAnalysisProductAdopter> analysis_product_adopter;
  const std::string analysis_archive_identity = options->zarr_path;
  CanonicalDetectionBuffer canonical_detection_buffer(
      analysis_data_scheduler, analysis_archive_identity);
  std::shared_ptr<crimson::zarr::ArchiveContext> analysis_archive;
  const CanonicalDetectionResidencyPolicy canonical_detection_residency_policy =
      canonicalDetectionProductionResidencyPolicy();
  crimson::zarr::CanonicalDetectionDescriptor canonical_detection_descriptor;
  crimson::zarr::CanonicalDetectionRepositoryOpenMetrics
      canonical_detection_open_metrics;
  bool canonical_detection_available = false;
  bool canonical_detection_failed = false;
  bool canonical_detection_residency_attempted = false;
  std::optional<CanonicalDetectionResidencyState>
      canonical_detection_residency_reported_state;
  int64_t last_canonical_detection_camera_request = -1;
  std::string canonical_detection_error;
  std::shared_ptr<const crimson::zarr::CanonicalDetectionFrame>
      presented_canonical_detection_frame;
  KeypointOverlayBuffer keypoint_overlay_buffer(analysis_data_scheduler,
                                                analysis_archive_identity);
  crimson::zarr::KeypointOverlayDescriptor keypoint_descriptor;
  std::shared_ptr<const crimson::zarr::KeypointOverlayResolution>
      presented_keypoint_resolution;
  bool keypoint_overlay_available = false;
  bool keypoint_overlay_failed = false;
  int64_t last_keypoint_camera_request = -1;
  std::string keypoint_error;
  crimson::gui::QualityTimelineSession quality_timeline_session(
      analysis_data_scheduler);
  crimson::polar::ChaserDistancePolarBuffer chaser_distance_polar_buffer;
  crimson::polar::ChaserDistancePolarDescriptor
      chaser_distance_polar_descriptor;
  bool chaser_distance_polar_available = false;
  bool chaser_distance_polar_failed = false;
  std::string chaser_distance_polar_error;
  SubjectMaskOverlayBuffer subject_mask_overlay_buffer(
      analysis_data_scheduler, analysis_archive_identity);
  crimson::overlay::SubjectMaskPresentationCoordinator
      subject_mask_frame_presentation;
  crimson::zarr::SubjectMaskOverlayDescriptor subject_mask_descriptor;
  bool subject_mask_overlay_available = false;
  bool subject_mask_overlay_failed = false;
  bool subject_mask_smoke_start_pending = false;
  bool subject_mask_first_ready_logged = false;
  std::optional<std::chrono::steady_clock::time_point>
      subject_mask_initial_request_started;
  std::optional<int64_t> subject_mask_initial_request_frame;
  std::string subject_mask_error;
  SubjectShapeOverlayBuffer subject_shape_overlay_buffer;
  crimson::zarr::SubjectShapeOverlayDescriptor subject_shape_descriptor;
  bool subject_shape_overlay_available = false;
  bool subject_shape_overlay_failed = false;
  std::string subject_shape_error;
  EyeGeometryOverlayBuffer eye_geometry_overlay_buffer;
  crimson::zarr::EyeGeometryOverlayDescriptor eye_geometry_descriptor;
  bool eye_geometry_overlay_available = false;
  bool eye_geometry_overlay_failed = false;
  std::string eye_geometry_error;
  AnalysisSeriesTimelineBuffer motion_timeline_buffer(
      analysis_data_scheduler, analysis_archive_identity);
  crimson::timeline::AnalysisSeriesTimelineDescriptor
      motion_timeline_descriptor;
  bool motion_timeline_available = false;
  bool motion_timeline_failed = false;
  std::string motion_timeline_error;
  uint64_t motion_timeline_presentations = 0;
  SwimBoutTimelineBuffer swim_bout_timeline_buffer;
  crimson::timeline::SwimBoutTimelineDescriptor swim_bout_timeline_descriptor;
  bool swim_bout_timeline_available = false;
  bool swim_bout_timeline_failed = false;
  std::string swim_bout_timeline_error;
  uint64_t swim_bout_timeline_presentations = 0;
  EyeAngleTimelineBuffer eye_angle_timeline_buffer;
  crimson::timeline::EyeAngleTimelineDescriptor eye_angle_timeline_descriptor;
  bool eye_angle_timeline_available = false;
  bool eye_angle_timeline_failed = false;
  std::string eye_angle_timeline_error;
  uint64_t eye_angle_timeline_presentations = 0;
  AnalysisSeriesTimelineBuffer tail_kinematics_timeline_buffer(
      analysis_data_scheduler, analysis_archive_identity);
  crimson::timeline::AnalysisSeriesTimelineDescriptor
      tail_kinematics_timeline_descriptor;
  bool tail_kinematics_timeline_available = false;
  bool tail_kinematics_timeline_failed = false;
  std::string tail_kinematics_timeline_error;
  uint64_t tail_kinematics_timeline_presentations = 0;
  crimson::timeline::StimulusContextTimelineDescriptor
      stimulus_context_timeline_descriptor;
  std::shared_ptr<const crimson::timeline::StimulusContextTimelineSnapshot>
      stimulus_context_timeline_snapshot;
  bool stimulus_context_timeline_available = false;
  bool stimulus_context_timeline_failed = false;
  std::string stimulus_context_timeline_error;
  uint64_t stimulus_context_timeline_presentations = 0;
  std::optional<AppleVideoAssetInfo> analysis_crop_view_info;
  std::optional<AppleVideoAssetInfo> crop_view_info;
  crimson::crop::CropPresentationCoordinator crop_presentation;
  std::optional<AppleAlignedAcquisitionCropFrame> current_crop_frame;
  crimson::crop::CropSourceSelection current_crop_selection;
  AppleCropViewerControls crop_controls;
  workspace_state.setCropSourcePreference(options->crop_preference);
  crop_controls.preference = workspace_state.cropSourcePreference();
  auto active_crop_preference = crop_controls.preference;
  bool crop_enabled = false;
  bool composite_enabled = false;
  bool crop_failed = false;
  bool crop_smoke_end_satisfied = false;
  uint64_t read_only_overlay_presentations = 0;
  uint64_t canonical_detection_presentations = 0;
  uint64_t canonical_detection_detections = 0;
  uint64_t chaser_distance_polar_presentations = 0;
  uint64_t chaser_distance_polar_points = 0;
  int64_t last_chaser_distance_polar_camera_request = -1;
  uint64_t keypoint_overlay_presentations = 0;
  uint64_t keypoint_overlay_detections = 0;
  uint64_t subject_mask_overlay_presentations = 0;
  uint64_t subject_mask_overlay_detections = 0;
  uint64_t subject_mask_overlay_components = 0;
  uint64_t subject_shape_overlay_presentations = 0;
  uint64_t subject_shape_overlay_detections = 0;
  int64_t last_subject_shape_camera_request = -1;
  uint64_t eye_geometry_overlay_presentations = 0;
  uint64_t eye_geometry_overlay_detections = 0;
  uint64_t eye_geometry_overlay_labels = 0;
  int64_t last_eye_geometry_camera_request = -1;
  bool pending_crop_discontinuity = true;
  int64_t last_crop_camera_request = -1;
  std::string crop_error;
  MultistreamSmokeState multistream_smoke;
  double smoke_start_memory_mib = 0.0;
  auto logSubjectMaskFirstReady = [&]() {
    if (subject_mask_first_ready_logged ||
        !subject_mask_initial_request_started) {
      return;
    }
    const auto repository_metrics =
        subject_mask_overlay_buffer.repositoryMetrics();
    const auto buffer_metrics = subject_mask_overlay_buffer.metrics();
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() -
                                  *subject_mask_initial_request_started)
                                  .count();
    std::printf(
        "[AppleDataAccess] source=subject_masks phase=first_ready "
        "elapsed_ms=%.1f lazy_mapping=%d frame_index_ms=%.1f "
        "frame_index_bytes=%llu mapping_page_reads=%llu "
        "mapping_page_hits=%llu cached_mapping_bytes=%llu "
        "peak_mapping_bytes=%llu fallback_index_builds=%llu "
        "source_bytes=%llu retained_chunk_bytes=%llu "
        "cached_chunk_bytes=%llu peak_chunk_bytes=%llu "
        "cached_frame_bytes=%llu peak_frame_bytes=%llu demand_chunks=%llu "
        "prefetched_chunks=%llu\n",
        elapsed_ms, repository_metrics.lazy_mapping ? 1 : 0,
        repository_metrics.frame_index_initialize_ms,
        static_cast<unsigned long long>(
            repository_metrics.frame_index_retained_bytes),
        static_cast<unsigned long long>(repository_metrics.mapping_page_reads),
        static_cast<unsigned long long>(
            repository_metrics.mapping_page_cache_hits),
        static_cast<unsigned long long>(
            repository_metrics.cached_mapping_bytes),
        static_cast<unsigned long long>(
            repository_metrics.peak_cached_mapping_bytes),
        static_cast<unsigned long long>(
            repository_metrics.fallback_frame_index_builds),
        static_cast<unsigned long long>(
            repository_metrics.chunk_source_bytes_read),
        static_cast<unsigned long long>(
            repository_metrics.chunk_retained_bytes_produced),
        static_cast<unsigned long long>(
            repository_metrics.cached_payload_bytes),
        static_cast<unsigned long long>(
            repository_metrics.peak_cached_payload_bytes),
        static_cast<unsigned long long>(buffer_metrics.cached_payload_bytes),
        static_cast<unsigned long long>(
            buffer_metrics.peak_cached_payload_bytes),
        static_cast<unsigned long long>(repository_metrics.demand_chunk_loads),
        static_cast<unsigned long long>(
            repository_metrics.prefetched_chunk_loads));
    std::fflush(stdout);
    subject_mask_first_ready_logged = true;
  };
  auto video_smoke_started = std::chrono::steady_clock::now();
  if (video_enabled) {
    std::string video_error;
    const bool renderers_ready =
        video_renderer.initialize(
            reinterpret_cast<uintptr_t>((__bridge void *)device),
            static_cast<uint64_t>(layer.pixelFormat), &video_error) &&
        overlay_renderer.initialize(
            reinterpret_cast<uintptr_t>((__bridge void *)device),
            static_cast<uint64_t>(layer.pixelFormat), &video_error);
    const bool playback_opened =
        renderers_ready &&
        (options->recording_clip_index_path.empty()
             ? video_playback.open(options->video_path, "camera-main",
                                   options->video_buffer_capacity, &video_error)
             : video_playback.openClipIndex(
                   options->recording_clip_index_path, "camera-main",
                   options->video_buffer_capacity, &video_error));
    if (!renderers_ready || !playback_opened) {
      std::fprintf(stderr, "[AppleVideo] Initialization failed: %s\n",
                   video_error.c_str());
      fail_initial_media_open(video_error.empty()
                                  ? "Camera media initialization failed"
                                  : video_error);
      ImGui_ImplMetal_Shutdown();
      ImGui_ImplGlfw_Shutdown();
      ImPlot::DestroyContext();
      ImGui::DestroyContext();
      glfwDestroyWindow(window);
      glfwTerminate();
      return 9;
    }
    video_clock.configure(video_playback.info().nominal_frame_rate,
                          video_playback.info().frame_count);
    const int64_t initial_frame = options->video_smoke
                                      ? options->video_smoke_start
                                      : options->start_paused_frame.value_or(0);
    if (initial_frame >= video_playback.info().frame_count) {
      std::fprintf(stderr,
                   "[AppleVideo] Initial frame %lld is outside the video "
                   "sample range\n",
                   static_cast<long long>(initial_frame));
      fail_initial_media_open("Initial camera frame is outside the media");
      video_playback.close();
      ImGui_ImplMetal_Shutdown();
      ImGui_ImplGlfw_Shutdown();
      ImPlot::DestroyContext();
      ImGui::DestroyContext();
      glfwDestroyWindow(window);
      glfwTerminate();
      return 9;
    }
    if (initial_frame > 0 &&
        !video_playback.requestSeek(initial_frame, &video_error)) {
      std::fprintf(stderr, "[AppleVideo] Initial seek failed: %s\n",
                   video_error.c_str());
      fail_initial_media_open(video_error.empty() ? "Initial seek failed"
                                                  : video_error);
      video_playback.close();
      ImGui_ImplMetal_Shutdown();
      ImGui_ImplGlfw_Shutdown();
      ImPlot::DestroyContext();
      ImGui::DestroyContext();
      glfwDestroyWindow(window);
      glfwTerminate();
      return 9;
    }
    const int64_t prebuffer_frame = std::min<int64_t>(
        video_playback.info().frame_count - 1,
        initial_frame + static_cast<int64_t>(video_playback.capacity()) - 1);
    const auto prebuffer_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool prebuffer_ready = false;
    while (!glfwWindowShouldClose(window) &&
           std::chrono::steady_clock::now() < prebuffer_deadline) {
      prebuffer_ready = video_playback.waitForFrame(
          prebuffer_frame, std::chrono::milliseconds(16));
      if (prebuffer_ready) {
        break;
      }
      glfwPollEvents();
      if (!renderAppleLoadingFrame(window, layer, command_queue,
                                   "Preparing first video frames", 0, 1)) {
        break;
      }
      glfwWaitEventsTimeout(1.0 / 60.0);
    }
    if (!prebuffer_ready && !glfwWindowShouldClose(window)) {
      const auto metrics = video_playback.metrics();
      std::fprintf(
          stderr,
          "[AppleVideo] Timed out prebuffering through frame %lld: %s\n",
          static_cast<long long>(prebuffer_frame), metrics.last_error.c_str());
      fail_initial_media_open(metrics.last_error.empty()
                                  ? "Camera prebuffer timed out"
                                  : metrics.last_error);
      video_playback.close();
      ImGui_ImplMetal_Shutdown();
      ImGui_ImplGlfw_Shutdown();
      ImPlot::DestroyContext();
      ImGui::DestroyContext();
      glfwDestroyWindow(window);
      glfwTerminate();
      return 9;
    }
    if (recording_open_workflow.active()) {
      recording_open_workflow.completeProduct("media", "Camera media ready",
                                              true);
    }
    if (analysis_requested) {
      recording_open_workflow.startProduct("analysis", "Loading analysis");
      AppleAnalysisRepositoryLoadRequest load_request;
      load_request.archive_path = options->zarr_path;
      load_request.detection_run = options->detection_run;
      load_request.refined_detection_run = options->refined_detection_run;
      load_request.allow_selector_ineligible_refined_run =
          options->allow_selector_ineligible_refined_run;
      load_request.stimulus_run = options->stimulus_run;
      load_request.stimulus_video_override = options->stimulus_video_path;
      load_request.crop_run = options->crop_run;
      load_request.swim_bout_run = options->swim_bout_run;
      load_request.keypoint_v2 = options->keypoint_v2;
      load_request.subject_mask_run = options->subject_mask_run;
      load_request.subject_mask_manifest_payload_digest =
          options->subject_mask_manifest_payload_digest;
      load_request.allow_selector_ineligible_subject_mask_run =
          options->allow_selector_ineligible_subject_mask_run;
      load_request.require_subject_mask_v1 = options->require_subject_mask_v1;
      load_request.subject_mask_presentation_cache_path =
          options->subject_mask_presentation_cache_path;
      load_request.subject_mask_presentation_cache_run =
          options->subject_mask_presentation_cache_run;
      load_request.subject_mask_presentation_cache_manifest_payload_digest =
          options->subject_mask_presentation_cache_manifest_payload_digest;
      load_request.subject_mask_contour_only =
          options->subject_mask_contour_only;
      load_request.camera_frame_count =
          static_cast<size_t>(video_playback.info().frame_count);
      load_request.subject_masks_enabled = options->subject_masks_enabled;
      load_request.subject_shapes_enabled = options->subject_shapes_enabled;
      load_request.eye_geometry_enabled = options->eye_geometry_enabled;
      load_request.motion_timeline_enabled = options->motion_timeline_enabled;
      load_request.swim_bout_timeline_enabled =
          options->swim_bout_timeline_enabled;
      load_request.eye_angle_timeline_enabled =
          options->eye_angle_timeline_enabled;
      load_request.tail_kinematics_timeline_enabled =
          options->tail_kinematics_timeline_enabled;
      load_request.stimulus_context_timeline_enabled =
          options->stimulus_context_timeline_enabled;
      load_request.scheduler = analysis_data_scheduler;

      std::string archive_error;
      std::optional<AppleAnalysisRepositoryBundle> loaded_analysis;
      if (!analysis_loader.start(std::move(load_request), &archive_error)) {
        std::fprintf(stderr, "[AppleAnalysisLoad] Failed to start: %s\n",
                     archive_error.c_str());
        analysis_loading_start_error = archive_error.empty()
                                           ? "Failed to start analysis loading"
                                           : archive_error;
      } else {
        while (!loaded_analysis && analysis_loader.loading() &&
               !glfwWindowShouldClose(window)) {
          loaded_analysis = analysis_loader.takeReady();
          if (loaded_analysis) {
            break;
          }
          glfwPollEvents();
          const auto progress = analysis_loader.progress();
          if (!renderAppleLoadingFrame(
                  window, layer, command_queue, progress.phase,
                  progress.completed_products, progress.total_products)) {
            archive_error = "Failed to render the analysis loading view";
            analysis_loader.cancel();
            break;
          }
          glfwWaitEventsTimeout(1.0 / 60.0);
        }
        if (glfwWindowShouldClose(window) || !archive_error.empty()) {
          analysis_loader.cancel();
          analysis_loader.close();
        } else if (!loaded_analysis) {
          loaded_analysis = analysis_loader.takeReady();
        }
      }
      if (!loaded_analysis || !loaded_analysis->archive ||
          loaded_analysis->cancelled) {
        if (archive_error.empty() && loaded_analysis) {
          archive_error = loaded_analysis->archive_error;
        }
        if (archive_error.empty()) {
          archive_error = loaded_analysis && loaded_analysis->cancelled
                              ? "Analysis loading was cancelled"
                              : "Analysis archive is unavailable";
        }
        std::fprintf(stderr, "[AppleZarr] Unavailable: %s\n",
                     archive_error.c_str());
      } else {
        AppleAnalysisProductAdoptionOptions adoption_options;
        adoption_options.initial_frame = initial_frame;
        adoption_options.video_smoke = options->video_smoke;
        adoption_options.ui_reference_enabled = options->ui_reference.enabled;
        adoption_options.prefer_alternate_eye_angle_representation =
            options->ui_reference.enabled &&
            options->ui_reference.state == AppleUiReferenceState::AnalysisEye;
        adoption_options.show_analysis_timeline =
            options->show_analysis_timeline;
        adoption_options.subject_masks_enabled = options->subject_masks_enabled;
        adoption_options.subject_shapes_enabled =
            options->subject_shapes_enabled;
        adoption_options.eye_geometry_enabled = options->eye_geometry_enabled;
        adoption_options.motion_timeline_enabled =
            options->motion_timeline_enabled;
        adoption_options.swim_bout_timeline_enabled =
            options->swim_bout_timeline_enabled;
        adoption_options.eye_angle_timeline_enabled =
            options->eye_angle_timeline_enabled;
        adoption_options.tail_kinematics_timeline_enabled =
            options->tail_kinematics_timeline_enabled;
        adoption_options.stimulus_context_timeline_enabled =
            options->stimulus_context_timeline_enabled;
        adoption_options.require_motion_timeline =
            options->require_motion_timeline;
        adoption_options.require_swim_bout_timeline =
            options->require_swim_bout_timeline;
        adoption_options.require_eye_angle_timeline =
            options->require_eye_angle_timeline;
        adoption_options.require_tail_kinematics_timeline =
            options->require_tail_kinematics_timeline;
        adoption_options.require_stimulus_context_timeline =
            options->require_stimulus_context_timeline;
        adoption_options.explicit_detection_requested =
            !options->detection_run.empty() ||
            !options->refined_detection_run.empty();
        adoption_options.keypoint_v2_requested = options->keypoint_v2.enabled();
        adoption_options.stimulus_buffer_capacity =
            options->stimulus_buffer_capacity;
        adoption_options.acquisition_crop_buffer_capacity =
            kAcquisitionCropBufferCapacity;
        adoption_options.detection_residency_budget_bytes =
            canonical_detection_residency_policy.maximum_resident_bytes;
        adoption_options.detection_residency_chunk_bytes =
            canonical_detection_residency_policy.maximum_chunk_decoded_bytes;

        analysis_product_adopter = std::make_unique<
            AppleAnalysisProductAdopter>(
            std::move(adoption_options),
            AppleAnalysisProductAdoptionContext{
                {analysis_archive, analysis_loading_start_error},
                {video_playback, workspace_state, analysis_timeline_controls},
                {chaser_distance_polar_buffer, chaser_distance_polar_descriptor,
                 chaser_distance_polar_available, chaser_distance_polar_failed,
                 chaser_distance_polar_error,
                 last_chaser_distance_polar_camera_request},
                {stimulus_playback, stimulus_enabled, stimulus_error},
                {canonical_detection_buffer, canonical_detection_descriptor,
                 canonical_detection_open_metrics,
                 canonical_detection_available, canonical_detection_failed,
                 canonical_detection_error,
                 last_canonical_detection_camera_request},
                {keypoint_overlay_buffer, keypoint_descriptor,
                 keypoint_overlay_available, keypoint_overlay_failed,
                 keypoint_error, last_keypoint_camera_request},
                {subject_mask_overlay_buffer, subject_mask_frame_presentation,
                 subject_mask_descriptor, subject_mask_overlay_available,
                 subject_mask_overlay_failed, subject_mask_smoke_start_pending,
                 subject_mask_initial_request_started,
                 subject_mask_initial_request_frame, subject_mask_error,
                 logSubjectMaskFirstReady},
                {subject_shape_overlay_buffer, subject_shape_descriptor,
                 subject_shape_overlay_available, subject_shape_overlay_failed,
                 subject_shape_error},
                {eye_geometry_overlay_buffer, eye_geometry_descriptor,
                 eye_geometry_overlay_available, eye_geometry_overlay_failed,
                 eye_geometry_error},
                {motion_timeline_buffer, motion_timeline_descriptor,
                 motion_timeline_available, motion_timeline_failed,
                 motion_timeline_error},
                {swim_bout_timeline_buffer, swim_bout_timeline_descriptor,
                 swim_bout_timeline_available, swim_bout_timeline_failed,
                 swim_bout_timeline_error},
                {eye_angle_timeline_buffer, eye_angle_timeline_descriptor,
                 eye_angle_timeline_available, eye_angle_timeline_failed,
                 eye_angle_timeline_error},
                {tail_kinematics_timeline_buffer,
                 tail_kinematics_timeline_descriptor,
                 tail_kinematics_timeline_available,
                 tail_kinematics_timeline_failed,
                 tail_kinematics_timeline_error},
                {stimulus_context_timeline_descriptor,
                 stimulus_context_timeline_snapshot,
                 stimulus_context_timeline_available,
                 stimulus_context_timeline_failed,
                 stimulus_context_timeline_error},
                {analysis_crop_geometry, crop_playback, analysis_crop_view_info,
                 crop_view_info, crop_controls, active_crop_preference,
                 crop_enabled, composite_enabled, crop_error}});
        analysis_product_adopter->adopt(std::move(*loaded_analysis),
                                        analysis_loader.loading());
        const bool deterministic_analysis_start =
            options->video_smoke || options->ui_reference.enabled;
        if (deterministic_analysis_start) {
          while (analysis_loader.loading() && !glfwWindowShouldClose(window)) {
            if (auto ready = analysis_loader.takeReady()) {
              analysis_product_adopter->adopt(std::move(*ready),
                                              analysis_loader.loading());
              continue;
            }
            glfwPollEvents();
            const auto progress = analysis_loader.progress();
            if (!renderAppleLoadingFrame(
                    window, layer, command_queue, progress.phase,
                    progress.completed_products, progress.total_products)) {
              analysis_loader.cancel();
              break;
            }
            glfwWaitEventsTimeout(1.0 / 60.0);
          }
          while (auto ready = analysis_loader.takeReady()) {
            analysis_product_adopter->adopt(std::move(*ready),
                                            analysis_loader.loading());
          }
        }
      }

      const bool requested_acquisition =
          options->crop_preference ==
          crimson::crop::CropSourcePreference::PreferAcquisitionVideo;
      const bool requested_crop_available =
          requested_acquisition ? crop_controls.acquisition_available
                                : crop_controls.live_geometry_available;
      if (options->stimulus_smoke && !stimulus_enabled) {
        video_playback.close();
        video_renderer.reset();
        ImGui_ImplMetal_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 10;
      }
      if (options->crop_smoke && !requested_crop_available) {
        stimulus_playback.close();
        video_playback.close();
        video_renderer.reset();
        ImGui_ImplMetal_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 11;
      }
    }
    if (options->multistream_smoke) {
      auto frame_supports_exact_composite = [&](int64_t camera_frame) {
        if (!stimulus_enabled || !crop_enabled || camera_frame < 0 ||
            camera_frame > std::numeric_limits<int32_t>::max()) {
          return false;
        }
        const auto stimulus_resolution = stimulus_playback.resolveCameraFrame(
            static_cast<int32_t>(camera_frame));
        if (stimulus_resolution.status !=
            crimson::zarr::StimulusMappingStatus::Mapped) {
          return false;
        }
        if (options->crop_preference ==
            crimson::crop::CropSourcePreference::PreferAcquisitionVideo) {
          return crop_playback.repository() != nullptr &&
                 crop_playback.resolveCameraFrame(camera_frame).status ==
                     crimson::zarr::AcquisitionCropMappingStatus::Mapped;
        }
        std::optional<crimson::crop::CropFrameGeometry> geometry;
        if (analysis_crop_geometry) {
          geometry = analysis_crop_geometry
                         ->resolveCameraFrame(camera_frame,
                                              video_playback.info().width,
                                              video_playback.info().height)
                         .geometry;
        } else if (const auto *repository = crop_playback.repository()) {
          geometry = repository->liveGeometry(camera_frame,
                                              video_playback.info().width,
                                              video_playback.info().height);
        }
        return geometry && geometry->usableForLiveCrop();
      };

      auto find_frame = [&](int64_t begin, int64_t end,
                            bool require_next) -> std::optional<int64_t> {
        for (int64_t frame = begin; frame <= end; ++frame) {
          if (frame_supports_exact_composite(frame) &&
              (!require_next || frame_supports_exact_composite(frame + 1))) {
            return frame;
          }
        }
        return std::nullopt;
      };

      const int64_t start = options->video_smoke_start;
      const int64_t end = options->video_smoke_end;
      const int64_t span = end - start;
      const auto pause =
          find_frame(start + std::max<int64_t>(2, span / 5),
                     start + std::max<int64_t>(4, span / 3), true);
      const auto backward =
          pause ? find_frame(start + 1, *pause - 2, false) : std::nullopt;
      const auto forward =
          pause
              ? find_frame(std::max<int64_t>(*pause + 2, start + span * 3 / 5),
                           end - 1, false)
              : std::nullopt;
      if (!pause || !backward || !forward ||
          !frame_supports_exact_composite(end)) {
        std::fprintf(
            stderr,
            "[AppleMultistreamSmoke] No mapped discontinuity plan exists "
            "inside %lld:%lld for the selected crop source\n",
            static_cast<long long>(start), static_cast<long long>(end));
        stimulus_playback.close();
        crop_playback.close();
        video_playback.close();
        video_renderer.reset();
        ImGui_ImplMetal_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 12;
      }
      multistream_smoke.stage = MultistreamSmokeStage::PlayToPause;
      multistream_smoke.pause_frame = *pause;
      multistream_smoke.step_frame = *pause + 1;
      multistream_smoke.backward_frame = *backward;
      multistream_smoke.forward_frame = *forward;
      multistream_smoke.end_frame = end;
      std::printf(
          "[AppleMultistreamSmoke] plan pause=%lld step=%lld backward=%lld "
          "forward=%lld end=%lld\n",
          static_cast<long long>(multistream_smoke.pause_frame),
          static_cast<long long>(multistream_smoke.step_frame),
          static_cast<long long>(multistream_smoke.backward_frame),
          static_cast<long long>(multistream_smoke.forward_frame),
          static_cast<long long>(multistream_smoke.end_frame));
    }
    video_clock.seek(initial_frame);
    const bool hold_for_analysis_start = analysis_requested &&
                                         !options->video_smoke &&
                                         !options->ui_reference.enabled;
    const bool start_playing = !subject_mask_smoke_start_pending &&
                               !options->start_paused_frame.has_value() &&
                               !hold_for_analysis_start;
    if (start_playing) {
      video_clock.play();
    }
    video_playback.setPlaybackState(initial_frame, start_playing,
                                    video_clock.effectiveFramesPerSecond());
    video_smoke_started = std::chrono::steady_clock::now();
    const auto &info = video_playback.info();
    std::printf("[AppleVideo] asset=%dx%d frames=%lld fps=%.6f "
                "buffer_capacity=%zu startup_ms=%.1f path=%s\n",
                info.width, info.height,
                static_cast<long long>(info.frame_count),
                info.nominal_frame_rate, video_playback.capacity(),
                video_playback.metrics().startup_ms, info.path.c_str());
  }
  composite_enabled = stimulus_enabled || crop_enabled;
  const double video_smoke_timeout_seconds =
      options->video_smoke
          ? std::max(30.0, static_cast<double>(options->video_smoke_end -
                                               options->video_smoke_start) /
                                   video_playback.info().nominal_frame_rate *
                                   1.5 +
                               30.0)
          : 30.0;

  MTLRenderPassDescriptor *render_pass = [MTLRenderPassDescriptor new];
  AppleMetalPresentationTexture presentation_texture;
  id<MTLCommandBuffer> last_command_buffer = nil;
  std::array<float, 120> frame_times_ms{};
  int frame_time_count = 0;
  int presented_frames = 0;
  bool render_failed = ui_reference_setup_failed;
  if (options->ui_reference.enabled && !ui_reference_setup_failed &&
      !ui_reference_capture.start()) {
    std::fprintf(stderr, "[AppleUiReference] START failed: %s\n",
                 ui_reference_capture.snapshot().failure_reason.c_str());
    render_failed = true;
    glfwSetWindowShouldClose(window, GLFW_TRUE);
  }
  bool invalid_workspace_viewport_logged = false;
  auto previous_frame_time = std::chrono::steady_clock::now();
  auto last_process_metric_time = LogicalPlaybackClock::TimePoint{};
  crimson::ui::SemanticSnapshot ui_semantic_snapshot;

  auto current_analysis_progress = [&] {
    auto progress = analysis_loader.progress();
    if (!analysis_loading_start_error.empty()) {
      progress.state = crimson::loading::LoadingState::Failed;
      progress.running = false;
      progress.error = analysis_loading_start_error;
      progress.phase = "Analysis loading failed";
    }
    return progress;
  };
  auto analysis_progress = current_analysis_progress();
  auto analysis_readiness =
      recording_open_workflow
          .settle(initial_recording_open_generation, analysis_progress)
          .readiness;

  while (!glfwWindowShouldClose(window)) {
    @autoreleasepool {
      glfwPollEvents();

      while (analysis_product_adopter) {
        auto ready = analysis_loader.takeReady();
        if (!ready) {
          break;
        }
        analysis_product_adopter->adopt(std::move(*ready),
                                        analysis_loader.loading());
      }
      analysis_progress = current_analysis_progress();
      if (analysis_progress.terminal() && analysis_product_adopter) {
        while (auto ready = analysis_loader.takeReady()) {
          analysis_product_adopter->adopt(std::move(*ready),
                                          analysis_loader.loading());
        }
        analysis_progress = current_analysis_progress();
      }
      analysis_readiness =
          recording_open_workflow
              .settle(initial_recording_open_generation, analysis_progress)
              .readiness;
      const bool analysis_presentation_demand_enabled =
          analysis_readiness.ready();

      if (canonical_detection_available &&
          !canonical_detection_residency_attempted &&
          last_canonical_detection_camera_request >= 0 &&
          canonical_detection_buffer.frame(
              last_canonical_detection_camera_request)) {
        canonical_detection_residency_attempted = true;
        std::string residency_error;
        if (!canonical_detection_buffer.startUiResidency(
                canonical_detection_residency_policy, &residency_error)) {
          std::fprintf(stderr,
                       "[AppleCanonicalDetectionResidency] Start failed: %s; "
                       "continuing with paging\n",
                       residency_error.c_str());
        }
        const auto residency = canonical_detection_buffer.residencyMetrics();
        reportCanonicalDetectionResidency(
            "activation", canonical_detection_descriptor, residency);
        canonical_detection_residency_reported_state = residency.state;
      }
      if (canonical_detection_residency_attempted) {
        const auto residency = canonical_detection_buffer.residencyMetrics();
        if (!canonical_detection_residency_reported_state ||
            *canonical_detection_residency_reported_state != residency.state) {
          reportCanonicalDetectionResidency(
              "transition", canonical_detection_descriptor, residency);
          canonical_detection_residency_reported_state = residency.state;
        }
      }

      if (!subject_mask_first_ready_logged &&
          subject_mask_initial_request_started &&
          subject_mask_initial_request_frame &&
          subject_mask_overlay_available &&
          subject_mask_overlay_buffer.frame(
              *subject_mask_initial_request_frame)) {
        logSubjectMaskFirstReady();
      }

      int width = 0;
      int height = 0;
      glfwGetFramebufferSize(window, &width, &height);
      if (options->ui_reference.enabled) {
        width = options->ui_reference.logical_width;
        height = options->ui_reference.logical_height;
      }
      if (width <= 0 || height <= 0) {
        glfwWaitEventsTimeout(0.01);
        continue;
      }
      layer.drawableSize = CGSizeMake(width, height);

      const auto drawable_started = std::chrono::steady_clock::now();
      id<CAMetalDrawable> drawable = [layer nextDrawable];
      if (video_enabled) {
        viewer_stats.max_next_drawable_ms =
            std::max(viewer_stats.max_next_drawable_ms,
                     std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - drawable_started)
                         .count());
      }
      if (drawable == nil) {
        glfwWaitEventsTimeout(0.01);
        continue;
      }
      presentation_texture.reset(drawable.texture);
      if (presentation_texture.descriptor().backend !=
              PresentationBackend::Metal ||
          presentation_texture.descriptor().pixel_format !=
              FramePixelFormat::BGRA8 ||
          presentation_texture.descriptor().width !=
              static_cast<int>(drawable.texture.width) ||
          presentation_texture.descriptor().height !=
              static_cast<int>(drawable.texture.height) ||
          presentation_texture.nativeHandle() == 0) {
        std::fprintf(
            stderr, "[MacShell] Invalid Metal presentation texture contract\n");
        render_failed = true;
        break;
      }

      id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
      render_pass.colorAttachments[0].texture = drawable.texture;
      render_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      render_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      render_pass.colorAttachments[0].clearColor =
          video_enabled ? MTLClearColorMake(0.0, 0.0, 0.0, 1.0)
                        : MTLClearColorMake(0.055, 0.060, 0.065, 1.0);
      id<MTLRenderCommandEncoder> encoder =
          [command_buffer renderCommandEncoderWithDescriptor:render_pass];
      if (command_buffer == nil || encoder == nil) {
        std::fprintf(stderr,
                     "[MacShell] Failed to create a Metal command buffer\n");
        render_failed = true;
        break;
      }

      ImGui_ImplMetal_NewFrame(render_pass);
      ImGui_ImplGlfw_NewFrame();
      if (options->ui_reference.enabled) {
        io.DisplaySize =
            ImVec2(static_cast<float>(options->ui_reference.logical_width),
                   static_cast<float>(options->ui_reference.logical_height));
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
      }
      crimson::ui::beginSemanticFrame(ImGui::GetCurrentContext());
      ImGui::NewFrame();
      const bool base_ui_interactive = !options->smoke &&
                                       !options->video_smoke &&
                                       !options->ui_reference.enabled;
      const bool session_ui_interactive =
          base_ui_interactive && analysis_readiness.session_interaction_enabled;
      const bool ui_interactive =
          base_ui_interactive && analysis_readiness.playback_controls_enabled;

      const auto now = std::chrono::steady_clock::now();
      const bool playback_transport_enabled =
          !analysis_requested || analysis_readiness.playback_controls_enabled;
      const bool playback_paused_for_readiness =
          video_enabled &&
          video_clock.setControlsEnabled(playback_transport_enabled, now);
      if (playback_paused_for_readiness) {
        video_playback.setPlaybackState(video_clock.requestedFrame(now), false,
                                        video_clock.effectiveFramesPerSecond());
      }
      if (analysis_requested && analysis_readiness.show_error_ui &&
          !analysis_loading_failure_reported) {
        error_popup_message = analysis_readiness.reason.empty()
                                  ? "Analysis loading failed"
                                  : analysis_readiness.reason;
        show_error_popup = true;
        analysis_loading_failure_reported = true;
      }
      const float frame_ms =
          std::chrono::duration<float, std::milli>(now - previous_frame_time)
              .count();
      previous_frame_time = now;
      if (frame_time_count < static_cast<int>(frame_times_ms.size())) {
        frame_times_ms[frame_time_count] = frame_ms;
      } else {
        std::rotate(frame_times_ms.begin(), frame_times_ms.begin() + 1,
                    frame_times_ms.end());
        frame_times_ms.back() = frame_ms;
      }
      ++frame_time_count;
      const int average_sample_count =
          std::min(frame_time_count, static_cast<int>(frame_times_ms.size()));
      double average_frame_ms = 0.0;
      for (int sample = 0; sample < average_sample_count; ++sample) {
        average_frame_ms += frame_times_ms[static_cast<size_t>(sample)];
      }
      if (average_sample_count > 0) {
        average_frame_ms /= average_sample_count;
      }
      const auto file_browser_result = drawAppleFileBrowserWindow(
          &file_browser_state, options->video_path,
          options->recording_clip_index_path, options->zarr_path,
          options->stimulus_video_path, average_frame_ms,
          video_enabled ? &video_clock : nullptr, session_ui_interactive);
      if (!file_browser_result.error.empty()) {
        error_popup_message = file_browser_result.error;
        show_error_popup = true;
      }
      if (file_browser_result.zarr_open_request.has_value()) {
        std::string discovery_error;
        try {
          auto selected_archive = crimson::zarr::ArchiveContext::Open(
              *file_browser_result.zarr_open_request, &discovery_error);
          if (!selected_archive) {
            error_popup_message =
                discovery_error.empty()
                    ? "The selected Zarr archive could not be opened."
                    : discovery_error;
            show_error_popup = true;
          } else {
            std::string video_discovery_error;
            const auto affiliated_video =
                crimson::zarr::DiscoverAffiliatedVideo(selected_archive,
                                                       &video_discovery_error);
            std::string clip_discovery_error;
            std::optional<crimson::zarr::AffiliatedRecordingClipIndexDescriptor>
                affiliated_clips;
            if (!affiliated_video) {
              affiliated_clips =
                  crimson::zarr::DiscoverAffiliatedRecordingClipIndex(
                      selected_archive, &clip_discovery_error);
            }
            if (!affiliated_video && !affiliated_clips) {
              error_popup_message =
                  !clip_discovery_error.empty() ? clip_discovery_error
                  : !video_discovery_error.empty()
                      ? video_discovery_error
                      : "The selected Zarr archive does not identify "
                        "affiliated recording media.";
              show_error_popup = true;
            } else {
              AppleSessionRelaunchRequest replacement;
              replacement.requested = true;
              replacement.zarr_path = *file_browser_result.zarr_open_request;
              replacement.video_buffer_capacity =
                  file_browser_state.video_buffer_capacity;
              replacement.stimulus_buffer_capacity =
                  file_browser_state.stimulus_buffer_capacity;
              bool media_ready = false;
              if (affiliated_video) {
                replacement.video_path =
                    affiliated_video->resolved_path.string();
                AppleVideoFrameProvider preflight;
                media_ready = preflight.open(replacement.video_path,
                                             "camera-main", &discovery_error);
                preflight.close();
              } else {
                replacement.recording_clip_index_path =
                    affiliated_clips->index_path.string();
                AppleVideoPlaybackBuffer preflight;
                media_ready = preflight.openClipIndex(
                    replacement.recording_clip_index_path, "camera-main",
                    file_browser_state.video_buffer_capacity, &discovery_error);
                preflight.close();
              }
              if (!media_ready) {
                error_popup_message =
                    discovery_error.empty()
                        ? "The affiliated recording media could not be opened."
                        : discovery_error;
                show_error_popup = true;
              } else {
                std::string replacement_error;
                if (!session_lifecycle.requestReplacement(replacement,
                                                          &replacement_error)) {
                  error_popup_message = replacement_error;
                  show_error_popup = true;
                  continue;
                }
                if (affiliated_video) {
                  std::printf(
                      "[MacSession] Discovered affiliated video source=%s "
                      "resolution=%s stored=%s resolved=%s\n",
                      crimson::zarr::AffiliatedVideoSourceName(
                          affiliated_video->source),
                      crimson::zarr::AffiliatedVideoResolutionName(
                          affiliated_video->resolution),
                      affiliated_video->stored_path.string().c_str(),
                      replacement.video_path.c_str());
                } else {
                  std::printf(
                      "[MacSession] Discovered recording clip index "
                      "recording=%s camera=%s frames=%lld fps=%.6f path=%s\n",
                      affiliated_clips->recording_id.c_str(),
                      affiliated_clips->camera_serial.c_str(),
                      static_cast<long long>(affiliated_clips->frame_count),
                      affiliated_clips->frames_per_second,
                      replacement.recording_clip_index_path.c_str());
                }
                glfwSetWindowShouldClose(window, GLFW_TRUE);
              }
            }
          }
        } catch (const std::exception &exception) {
          error_popup_message = "Could not open the selected Zarr archive: " +
                                std::string(exception.what());
          show_error_popup = true;
        }
      }
      if (file_browser_result.relaunch.requested) {
        std::string replacement_error;
        if (!session_lifecycle.requestReplacement(file_browser_result.relaunch,
                                                  &replacement_error)) {
          error_popup_message = replacement_error;
          show_error_popup = true;
        } else {
          glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
      }
      if (video_enabled && file_browser_result.accurate_seek_frame) {
        const int64_t target =
            std::clamp<int64_t>(*file_browser_result.accurate_seek_frame, 0,
                                video_playback.info().frame_count - 1);
        video_clock.pause(now);
        video_clock.seek(target, now);
        std::string seek_error;
        if (!video_playback.requestSeek(target, &seek_error)) {
          error_popup_message = seek_error;
          show_error_popup = true;
        } else {
          pending_camera_discontinuity = true;
          viewer_presentation_discontinuity = true;
        }
      }
      if (ui_interactive && !io.WantTextInput &&
          ImGui::IsKeyPressed(ImGuiKey_H, false)) {
        workspace_state.setWindowRequested(
            crimson::workspace::Window::Help,
            !workspace_state.windowRequested(crimson::workspace::Window::Help));
      }
      drawAppleHelpWindow(
          workspace_state.windowRequested(crimson::workspace::Window::Help));
      drawAppleErrorPopup(&show_error_popup, error_popup_message);
      AppleMetalVideoViewport camera_window_viewport;
      AppleMetalVideoViewport crop_preview_window_viewport;
      AppleMetalVideoViewport stimulus_debug_window_viewport;
      std::optional<AppleDecodedVideoFrame> selected_stimulus_debug_frame;
      AppleCompositeVideoViewports video_viewports;
      bool multistream_composite_exact = false;
      int64_t multistream_composite_frame = -1;
      bool ui_reference_overlay_ready = false;
      bool ui_reference_analysis_ready = false;
      bool ui_reference_camera_exact = false;
      bool ui_reference_crop_exact = false;
      bool ui_reference_stimulus_exact = false;
      bool ui_reference_polar_ready = false;
      crimson::polar::ChaserDistancePolarScene ui_reference_polar_scene;
      AppleMetalVideoViewport ui_reference_polar_viewport;
      bool ui_reference_stimulus_overlay_ready = false;
      crimson::stimulus::StimulusCameraOverlayScene
          ui_reference_stimulus_overlay_scene;
      AppleMetalVideoViewport ui_reference_stimulus_overlay_viewport;
      nlohmann::json ui_reference_overlay_counts = nlohmann::json::object();
      if (video_enabled) {
        if (viewer_stats.process_memory_mib == 0.0 ||
            std::chrono::duration<double>(now - last_process_metric_time)
                    .count() >= 1.0) {
          sampleAppleVideoViewerSystemMetrics(viewer_stats);
          last_process_metric_time = now;
          if (options->multistream_smoke && smoke_start_memory_mib == 0.0 &&
              viewer_stats.process_memory_mib > 0.0) {
            smoke_start_memory_mib = viewer_stats.process_memory_mib;
          }
        }
        const bool was_playing = video_clock.isPlaying();
        const auto transport_tick = video_clock.update(now);
        viewer_stats.requested_frame = transport_tick.requested_frame;
        bool transport_stop_target_committed = false;
        if (options->multistream_smoke && video_clock.isPlaying() &&
            multistream_smoke.stage == MultistreamSmokeStage::PlayToPause &&
            viewer_stats.requested_frame >= multistream_smoke.pause_frame) {
          video_clock.pause(now);
          video_clock.seek(multistream_smoke.pause_frame, now);
          viewer_stats.requested_frame = multistream_smoke.pause_frame;
          transport_stop_target_committed = true;
          multistream_smoke.stage = MultistreamSmokeStage::WaitForPausedExact;
          pending_camera_discontinuity = true;
          viewer_presentation_discontinuity = true;
        } else if (options->multistream_smoke && video_clock.isPlaying() &&
                   multistream_smoke.stage ==
                       MultistreamSmokeStage::ResumeToEnd &&
                   viewer_stats.requested_frame >=
                       multistream_smoke.end_frame) {
          video_clock.pause(now);
          video_clock.seek(multistream_smoke.end_frame, now);
          viewer_stats.requested_frame = multistream_smoke.end_frame;
          transport_stop_target_committed = true;
          multistream_smoke.stage = MultistreamSmokeStage::WaitForEndExact;
          pending_camera_discontinuity = true;
          viewer_presentation_discontinuity = true;
        } else if (options->video_smoke && !options->multistream_smoke &&
                   video_clock.isPlaying() &&
                   viewer_stats.requested_frame >= options->video_smoke_end) {
          video_clock.pause(now);
          video_clock.seek(options->video_smoke_end, now);
          viewer_stats.requested_frame = options->video_smoke_end;
          transport_stop_target_committed = true;
          pending_camera_discontinuity = true;
          viewer_presentation_discontinuity = true;
        } else if (!options->video_smoke && transport_tick.reached_end) {
          viewer_stats.requested_frame = video_playback.info().frame_count - 1;
          transport_stop_target_committed = true;
          pending_camera_discontinuity = true;
          viewer_presentation_discontinuity = true;
        }
        crop_controls.metrics =
            crop_enabled ? &crop_presentation.metrics() : nullptr;
        overlay_availability.keypoints = keypoint_overlay_available;
        overlay_availability.headings = keypoint_overlay_available;
        overlay_availability.subject_masks = subject_mask_overlay_available;
        overlay_availability.subject_shape = subject_shape_overlay_available;
        overlay_availability.eye_geometry = eye_geometry_overlay_available;
        std::shared_ptr<const crimson::timeline::AnalysisSeriesTimelineWindow>
            motion_timeline_window;
        if (analysis_presentation_demand_enabled && motion_timeline_available &&
            viewer_stats.requested_frame >= 0 &&
            viewer_stats.requested_frame <
                static_cast<int64_t>(motion_timeline_descriptor.frame_count)) {
          if (!motion_timeline_buffer.requestFrame(
                  viewer_stats.requested_frame,
                  workspace_state.selections().motion_source_key,
                  video_playback.info().nominal_frame_rate,
                  pending_camera_discontinuity, &motion_timeline_error)) {
            motion_timeline_failed = true;
            motion_timeline_available = false;
            std::fprintf(stderr, "[AppleMotionTimeline] Request failed: %s\n",
                         motion_timeline_error.c_str());
          } else {
            motion_timeline_window = motion_timeline_buffer.window(
                viewer_stats.requested_frame,
                workspace_state.selections().motion_source_key);
            if (motion_timeline_window && motion_timeline_window->ready()) {
              ++motion_timeline_presentations;
            }
          }
        }
        std::shared_ptr<const crimson::timeline::SwimBoutTimelineWindow>
            swim_bout_timeline_window;
        if (analysis_presentation_demand_enabled &&
            swim_bout_timeline_available &&
            !workspace_state.selections().swim_bout_candidate_key.empty() &&
            viewer_stats.requested_frame >= 0 &&
            viewer_stats.requested_frame <
                static_cast<int64_t>(
                    swim_bout_timeline_descriptor.frame_count)) {
          const bool include_detector =
              analysis_timeline_controls.swim_bouts.show_detector_response;
          if (!swim_bout_timeline_buffer.requestFrame(
                  viewer_stats.requested_frame,
                  workspace_state.selections().swim_bout_candidate_key,
                  video_playback.info().nominal_frame_rate, include_detector,
                  pending_camera_discontinuity, &swim_bout_timeline_error)) {
            swim_bout_timeline_failed = true;
            swim_bout_timeline_available = false;
            std::fprintf(stderr, "[AppleSwimBoutTimeline] Request failed: %s\n",
                         swim_bout_timeline_error.c_str());
          } else {
            swim_bout_timeline_window = swim_bout_timeline_buffer.window(
                viewer_stats.requested_frame,
                workspace_state.selections().swim_bout_candidate_key,
                include_detector);
            if (swim_bout_timeline_window &&
                swim_bout_timeline_window->ready()) {
              ++swim_bout_timeline_presentations;
            }
          }
        }
        std::shared_ptr<const crimson::timeline::EyeAngleTimelineWindow>
            eye_angle_window;
        if (analysis_presentation_demand_enabled &&
            eye_angle_timeline_available && viewer_stats.requested_frame >= 0 &&
            viewer_stats.requested_frame <
                static_cast<int64_t>(
                    eye_angle_timeline_descriptor.frame_count)) {
          if (!eye_angle_timeline_buffer.requestFrame(
                  viewer_stats.requested_frame,
                  workspace_state.selections().eye_angle_representation_key,
                  video_playback.info().nominal_frame_rate,
                  pending_camera_discontinuity, &eye_angle_timeline_error)) {
            eye_angle_timeline_failed = true;
            eye_angle_timeline_available = false;
            std::fprintf(stderr, "[AppleEyeAngleTimeline] Request failed: %s\n",
                         eye_angle_timeline_error.c_str());
          } else {
            eye_angle_window = eye_angle_timeline_buffer.window(
                viewer_stats.requested_frame,
                workspace_state.selections().eye_angle_representation_key);
            if (eye_angle_window && eye_angle_window->ready()) {
              ++eye_angle_timeline_presentations;
              ui_reference_analysis_ready =
                  options->ui_reference.enabled &&
                  options->ui_reference.state ==
                      AppleUiReferenceState::AnalysisEye &&
                  eye_angle_window->coversFrame(
                      options->ui_reference.target_frame,
                      workspace_state.selections()
                          .eye_angle_representation_key);
            }
          }
        }
        std::shared_ptr<const crimson::timeline::AnalysisSeriesTimelineWindow>
            tail_kinematics_timeline_window;
        if (analysis_presentation_demand_enabled &&
            tail_kinematics_timeline_available &&
            viewer_stats.requested_frame >= 0 &&
            viewer_stats.requested_frame <
                static_cast<int64_t>(
                    tail_kinematics_timeline_descriptor.frame_count)) {
          if (!tail_kinematics_timeline_buffer.requestFrame(
                  viewer_stats.requested_frame,
                  workspace_state.selections().tail_kinematics_source_key,
                  video_playback.info().nominal_frame_rate,
                  pending_camera_discontinuity,
                  &tail_kinematics_timeline_error)) {
            tail_kinematics_timeline_failed = true;
            tail_kinematics_timeline_available = false;
            std::fprintf(stderr,
                         "[AppleTailKinematicsTimeline] Request failed: %s\n",
                         tail_kinematics_timeline_error.c_str());
          } else {
            tail_kinematics_timeline_window =
                tail_kinematics_timeline_buffer.window(
                    viewer_stats.requested_frame,
                    workspace_state.selections().tail_kinematics_source_key);
            if (tail_kinematics_timeline_window &&
                tail_kinematics_timeline_window->ready()) {
              ++tail_kinematics_timeline_presentations;
            }
          }
        }
        if (stimulus_context_timeline_available &&
            viewer_stats.requested_frame >= 0 &&
            viewer_stats.requested_frame <
                static_cast<int64_t>(
                    stimulus_context_timeline_descriptor.frame_count)) {
          ++stimulus_context_timeline_presentations;
        }
        const bool analysis_timeline_available =
            motion_timeline_available || eye_angle_timeline_available ||
            tail_kinematics_timeline_available ||
            stimulus_context_timeline_available;
        bool detection_quality_requested = workspace_state.windowRequested(
            crimson::workspace::Window::DetectionQualityTimeline);
        bool keypoint_quality_requested = workspace_state.windowRequested(
            crimson::workspace::Window::KeypointQualityTimeline);
        crimson::gui::QualityTimelineSessionRequest quality_request;
        crimson::gui::QualityTimelineRepositoryFactories quality_factories;
        if (analysis_archive && canonical_detection_available) {
          quality_request.detection_archive_path = options->zarr_path;
          quality_request.detection_surface =
              canonical_detection_descriptor.surface_kind;
          quality_request.detection_run_name =
              canonical_detection_descriptor.run_name;
          quality_request.allow_selector_ineligible_refined_run =
              options->allow_selector_ineligible_refined_run;
          const auto archive = analysis_archive;
          const crimson::zarr::DetectionQualityTimelineOpenRequest open_request{
              canonical_detection_descriptor.surface_kind,
              canonical_detection_descriptor.run_name,
              options->allow_selector_ineligible_refined_run};
          quality_factories.detection = [archive,
                                         open_request](std::string *error) {
            return crimson::zarr::OpenDetectionQualityTimelineRepository(
                archive, open_request, error);
          };
        }
        const auto quality_selection = [](const auto &selection) {
          return crimson::gui::QualityTimelineArtifactSelection{
              selection.archive_path, selection.run, selection.manifest_digest};
        };
        if (options->keypoint_v2.enabled()) {
          quality_request.raw_keypoints =
              quality_selection(options->keypoint_v2.raw);
          quality_request.keypoint_quality =
              quality_selection(options->keypoint_v2.quality);
          quality_request.refined_keypoints =
              quality_selection(options->keypoint_v2.refined);
          quality_request.body_frame =
              quality_selection(options->keypoint_v2.body_frame);
          quality_request.allow_selector_ineligible_keypoints =
              options->keypoint_v2.allow_selector_ineligible;
          quality_request.deep_validate_keypoint_identity =
              options->keypoint_v2.deep_validate_identity;
        }
        if (keypoint_overlay_available) {
          quality_factories.keypoints =
              [&keypoint_overlay_buffer](std::string *error) {
                return keypoint_overlay_buffer.createQualityTimelineRepository(
                    error);
              };
        }
        quality_timeline_session.configure(std::move(quality_request),
                                           std::move(quality_factories));
        quality_timeline_session.update(
            viewer_stats.requested_frame, pending_camera_discontinuity,
            detection_quality_requested && canonical_detection_available,
            detection_quality_timeline_controls,
            keypoint_quality_requested && keypoint_overlay_available,
            keypoint_quality_timeline_controls);
        const auto detection_quality_timeline_window =
            quality_timeline_session.detectionWindow();
        const auto detection_quality_timeline_overview =
            quality_timeline_session.detectionOverview();
        const auto keypoint_quality_timeline_window =
            quality_timeline_session.keypointWindow();
        const auto keypoint_quality_timeline_overview =
            quality_timeline_session.keypointOverview();
        crimson::workspace::WorkspaceCapabilities workspace_capabilities;
        workspace_capabilities.video_loaded = true;
        workspace_capabilities.playback_ready = video_playback.isOpen();
        workspace_capabilities.playing = video_clock.isPlaying();
        workspace_capabilities.zarr_loaded = analysis_requested;
        workspace_capabilities.crop_preview_available = crop_enabled;
        workspace_capabilities.stimulus_video_loaded = stimulus_enabled;
        workspace_capabilities.analysis_timeline_available =
            analysis_timeline_available;
        workspace_capabilities.detection_quality_available =
            canonical_detection_available;
        workspace_capabilities.keypoint_quality_available =
            keypoint_overlay_available && options->keypoint_v2.enabled();
        if (!workspace_state.readOnlyInvariant(workspace_capabilities)) {
          error_popup_message = "A write-capable repository was opened in the "
                                "read-only workspace.";
          show_error_popup = true;
          render_failed = true;
          glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        bool crop_preview_requested = workspace_state.windowRequested(
            crimson::workspace::Window::AdvancedCropPreview);
        bool stimulus_debug_requested = workspace_state.windowRequested(
            crimson::workspace::Window::Stimulus);
        const auto presented_subject_mask_resolution =
            subject_mask_overlay_available && viewer_stats.presented_frame >= 0
                ? subject_mask_overlay_buffer.frame(
                      viewer_stats.presented_frame)
                : nullptr;
        const auto presented_eye_geometry_resolution =
            eye_geometry_overlay_available && viewer_stats.presented_frame >= 0
                ? eye_geometry_overlay_buffer.frame(
                      viewer_stats.presented_frame)
                : nullptr;
        const auto presented_subject_shape_resolution =
            subject_shape_overlay_available && viewer_stats.presented_frame >= 0
                ? subject_shape_overlay_buffer.frame(
                      viewer_stats.presented_frame)
                : nullptr;
        drawAppleFrameInspectWindow(
            &workspace_state.selections(), &overlay_controls,
            overlay_availability, crop_enabled ? &crop_controls : nullptr,
            stimulus_enabled, chaser_distance_polar_available,
            canonical_detection_available ? &canonical_detection_descriptor
                                          : nullptr,
            presented_canonical_detection_frame,
            quality_timeline_session.detectionState(),
            quality_timeline_session.detectionError(), &detection_inspect_state,
            &detection_quality_requested,
            keypoint_overlay_available ? &keypoint_descriptor : nullptr,
            presented_keypoint_resolution,
            quality_timeline_session.keypointState(),
            quality_timeline_session.keypointError(), &keypoint_inspect_state,
            &keypoint_quality_requested,
            subject_mask_overlay_available ? &subject_mask_descriptor : nullptr,
            presented_subject_mask_resolution, &subject_mask_inspect_state,
            subject_shape_overlay_available ? &subject_shape_descriptor
                                            : nullptr,
            presented_subject_shape_resolution, &subject_shape_inspect_state,
            eye_geometry_overlay_available ? &eye_geometry_descriptor : nullptr,
            presented_eye_geometry_resolution, &eye_angle_inspect_state,
            viewer_stats, &frame_inspect_presentation, &crop_preview_requested,
            &stimulus_debug_requested, ui_interactive);
        workspace_state.setWindowRequested(
            crimson::workspace::Window::AdvancedCropPreview,
            crop_preview_requested);
        workspace_state.setWindowRequested(crimson::workspace::Window::Stimulus,
                                           stimulus_debug_requested);
        workspace_state.setWindowRequested(
            crimson::workspace::Window::DetectionQualityTimeline,
            detection_quality_requested);
        workspace_state.setWindowRequested(
            crimson::workspace::Window::KeypointQualityTimeline,
            keypoint_quality_requested);

        const AppleVideoPlaybackBufferMetrics playback_metrics =
            video_playback.metrics();
        const auto diagnostics_result = drawAppleDiagnosticsWindow(
            viewer_stats, playback_metrics, video_playback.capacity(),
            video_clock.isPlaying(), decode_dump_root.string(),
            decode_debug_status,
            stimulus_enabled ? &stimulus_presentation.metrics() : nullptr,
            ui_interactive);
        if (diagnostics_result.request_random_seek_dump) {
          std::uniform_int_distribution<int64_t> distribution(
              0, video_playback.info().frame_count - 1);
          const int64_t target = distribution(diagnostic_random);
          video_clock.pause(now);
          video_clock.seek(target, now);
          std::string seek_error;
          if (!video_playback.requestSeek(target, &seek_error) ||
              !video_playback.waitForFrame(target, std::chrono::seconds(10))) {
            decode_debug_status =
                seek_error.empty() ? "Random seek did not settle in 10 seconds."
                                   : seek_error;
          } else {
            dumpAppleDecodeBuffer(video_playback, decode_dump_root,
                                  "random_seek_" + std::to_string(target),
                                  &decode_debug_status);
          }
          pending_camera_discontinuity = true;
          viewer_presentation_discontinuity = true;
        } else if (diagnostics_result.request_dump_decode_buffers) {
          dumpAppleDecodeBuffer(video_playback, decode_dump_root, "manual_dump",
                                &decode_debug_status);
        }
        if (workspace_state.shouldSubmit(
                crimson::workspace::Window::FramesInBuffer,
                workspace_capabilities)) {
          const auto buffered_frame_numbers =
              video_playback.bufferedFrameNumbers();
          const bool buffer_discontinuity = drawAppleFramesInBufferWindow(
              viewer_stats, playback_metrics, video_playback.capacity(),
              buffered_frame_numbers, video_clock, video_playback,
              ui_interactive);
          pending_camera_discontinuity =
              pending_camera_discontinuity || buffer_discontinuity;
          viewer_presentation_discontinuity =
              viewer_presentation_discontinuity || buffer_discontinuity;
        }
        const auto control_result = drawAppleCameraViewWindow(
            camera_window_name, video_clock, video_playback, viewer_stats,
            &camera_window_viewport, &camera_view_state, ui_interactive);

        if (workspace_state.shouldSubmit(
                crimson::workspace::Window::AdvancedCropPreview,
                workspace_capabilities) &&
            crop_view_info) {
          drawAppleAdvancedCropPreviewWindow(&crop_preview_requested,
                                             *crop_view_info,
                                             &crop_preview_window_viewport);
          workspace_state.setWindowRequested(
              crimson::workspace::Window::AdvancedCropPreview,
              crop_preview_requested);
        }
        if (workspace_state.shouldSubmit(crimson::workspace::Window::Stimulus,
                                         workspace_capabilities)) {
          drawAppleStimulusDebugWindows(
              true, stimulus_playback.info(), stimulus_playback.metrics(),
              stimulus_playback.bufferedFrameNumbers(), &stimulus_debug_state,
              &stimulus_debug_window_viewport, ui_interactive);
          if (stimulus_debug_state.selected_frame) {
            selected_stimulus_debug_frame =
                stimulus_playback.frameForStimulusFrame(
                    *stimulus_debug_state.selected_frame);
            if (!selected_stimulus_debug_frame) {
              stimulus_debug_state.selected_frame.reset();
            }
          }
        }
        video_viewports = appleWorkspaceVideoViewports(
            camera_window_viewport, crop_preview_window_viewport,
            stimulus_debug_window_viewport,
            crop_enabled && crop_view_info &&
                    frame_inspect_presentation.roi_inset.visible
                ? &*crop_view_info
                : nullptr,
            stimulus_enabled && frame_inspect_presentation.show_stimulus_inset
                ? &stimulus_playback.info()
                : nullptr,
            frame_inspect_presentation.roi_inset.width_px *
                io.DisplayFramebufferScale.x,
            frame_inspect_presentation.stimulus_inset_width *
                io.DisplayFramebufferScale.x);

        bool stimulus_timeline_discontinuity = false;
        if (stimulus_context_timeline_available &&
            stimulus_context_timeline_snapshot) {
          stimulus_timeline_discontinuity = drawAppleStimulusEventTimeline(
              &analysis_timeline_controls, stimulus_context_timeline_descriptor,
              stimulus_context_timeline_snapshot,
              viewer_stats.presented_frame >= 0 ? viewer_stats.presented_frame
                                                : viewer_stats.requested_frame,
              video_clock, video_playback, ui_interactive);
        }
        const bool analysis_timeline_discontinuity =
            analysis_timeline_available &&
            drawAppleAnalysisTimeline(
                &analysis_timeline_controls,
                motion_timeline_available ? &motion_timeline_descriptor
                                          : nullptr,
                motion_timeline_window,
                swim_bout_timeline_available ? &swim_bout_timeline_descriptor
                                             : nullptr,
                swim_bout_timeline_window,
                eye_angle_timeline_available ? &eye_angle_timeline_descriptor
                                             : nullptr,
                eye_angle_window,
                tail_kinematics_timeline_available
                    ? &tail_kinematics_timeline_descriptor
                    : nullptr,
                tail_kinematics_timeline_window,
                stimulus_context_timeline_available
                    ? &stimulus_context_timeline_descriptor
                    : nullptr,
                stimulus_context_timeline_snapshot,
                viewer_stats.presented_frame >= 0
                    ? viewer_stats.presented_frame
                    : viewer_stats.requested_frame,
                video_clock, video_playback, ui_interactive);
        const bool detection_quality_timeline_discontinuity =
            workspace_state.shouldSubmit(
                crimson::workspace::Window::DetectionQualityTimeline,
                workspace_capabilities) &&
            drawAppleDetectionQualityTimeline(
                &detection_quality_timeline_controls,
                &detection_quality_requested,
                quality_timeline_session.detectionState(),
                quality_timeline_session.detectionDescriptor(),
                detection_quality_timeline_window,
                detection_quality_timeline_overview,
                quality_timeline_session.detectionError(),
                viewer_stats.presented_frame >= 0
                    ? viewer_stats.presented_frame
                    : viewer_stats.requested_frame,
                video_clock, video_playback, ui_interactive);
        workspace_state.setWindowRequested(
            crimson::workspace::Window::DetectionQualityTimeline,
            detection_quality_requested);
        const bool keypoint_quality_timeline_discontinuity =
            workspace_state.shouldSubmit(
                crimson::workspace::Window::KeypointQualityTimeline,
                workspace_capabilities) &&
            drawAppleKeypointQualityTimeline(
                &keypoint_quality_timeline_controls,
                &keypoint_quality_requested,
                quality_timeline_session.keypointState(),
                quality_timeline_session.keypointDescriptor(),
                keypoint_quality_timeline_window,
                keypoint_quality_timeline_overview,
                quality_timeline_session.keypointError(),
                viewer_stats.presented_frame >= 0
                    ? viewer_stats.presented_frame
                    : viewer_stats.requested_frame,
                video_clock, video_playback, ui_interactive);
        workspace_state.setWindowRequested(
            crimson::workspace::Window::KeypointQualityTimeline,
            keypoint_quality_requested);
        pending_camera_discontinuity =
            pending_camera_discontinuity ||
            control_result.camera_discontinuity ||
            stimulus_timeline_discontinuity ||
            analysis_timeline_discontinuity ||
            detection_quality_timeline_discontinuity ||
            keypoint_quality_timeline_discontinuity;
        viewer_presentation_discontinuity =
            viewer_presentation_discontinuity ||
            control_result.camera_discontinuity ||
            stimulus_timeline_discontinuity || analysis_timeline_discontinuity;
        viewer_presentation_discontinuity =
            viewer_presentation_discontinuity ||
            detection_quality_timeline_discontinuity ||
            keypoint_quality_timeline_discontinuity;
        viewer_stats.requested_frame = video_clock.requestedFrame();
        const bool is_playing = video_clock.isPlaying();
        if (was_playing && !is_playing && !transport_stop_target_committed) {
          const int64_t pause_frame = viewer_stats.presented_frame >= 0
                                          ? viewer_stats.presented_frame
                                          : viewer_stats.requested_frame;
          video_clock.seek(pause_frame, now);
          viewer_stats.requested_frame = pause_frame;
          if (!video_playback.selectBufferedFrame(pause_frame)) {
            pending_camera_discontinuity = true;
            viewer_presentation_discontinuity = true;
            std::string pause_error;
            if (!video_playback.requestSeek(pause_frame, &pause_error)) {
              std::fprintf(
                  stderr, "[AppleVideo] Pause exact-frame request failed: %s\n",
                  pause_error.c_str());
            }
          }
        }
        pending_crop_discontinuity =
            pending_crop_discontinuity || pending_camera_discontinuity;
        if (composite_enabled && pending_camera_discontinuity) {
          pending_video_frame.reset();
          candidate_stimulus_frame.reset();
          stimulus_candidate_ready = false;
        }
        const auto visible_camera_frame =
            current_video_frame
                ? std::optional<int64_t>(
                      current_video_frame->metadata.frame_number)
                : std::nullopt;
        auto apple_presentation = updateApplePlaybackPresentation(
            video_playback,
            {viewer_stats.requested_frame, video_clock.isPlaying(),
             video_clock.effectiveFramesPerSecond(),
             pending_camera_discontinuity, visible_camera_frame});
        auto &selected = apple_presentation.selected_frame;
        const auto &camera_presentation_decision = apple_presentation.decision;
        const bool present_selected = apple_presentation.present_selected;
        if (!composite_enabled) {
          if (present_selected) {
            current_video_frame = std::move(selected);
          } else if (camera_presentation_decision.action ==
                     crimson::playback::FramePresentationAction::Clear) {
            current_video_frame.reset();
          }
        } else if (!stimulus_enabled) {
          if (present_selected &&
              (!pending_video_frame || pending_camera_discontinuity)) {
            pending_video_frame = std::move(selected);
          }
          candidate_stimulus_frame.reset();
          candidate_stimulus_resolution = {};
          stimulus_candidate_ready = pending_video_frame.has_value();
          pending_camera_discontinuity = false;
        } else if (!stimulus_failed) {
          if (present_selected &&
              (!pending_video_frame || pending_camera_discontinuity)) {
            pending_video_frame = std::move(selected);
          }
          if (pending_video_frame) {
            const int64_t camera_frame =
                pending_video_frame->metadata.frame_number;
            if (camera_frame < 0 ||
                camera_frame > std::numeric_limits<int32_t>::max()) {
              stimulus_error =
                  "camera frame exceeds the stimulus mapping range";
              stimulus_failed = true;
            } else {
              const int32_t mapped_camera_frame =
                  static_cast<int32_t>(camera_frame);
              if (last_stimulus_camera_request != camera_frame ||
                  pending_camera_discontinuity) {
                if (!stimulus_playback.requestCameraFrame(
                        mapped_camera_frame, pending_camera_discontinuity,
                        &stimulus_error)) {
                  std::fprintf(stderr,
                               "[AppleStimulus] Camera frame %d request "
                               "failed: %s\n",
                               mapped_camera_frame, stimulus_error.c_str());
                  stimulus_failed = true;
                } else {
                  last_stimulus_camera_request = camera_frame;
                  pending_camera_discontinuity = false;
                }
              }

              if (!stimulus_failed) {
                const auto resolution =
                    stimulus_playback.resolveCameraFrame(mapped_camera_frame);
                auto aligned =
                    stimulus_playback.frameForCameraFrame(mapped_camera_frame);
                const std::optional<int32_t> decoded_stimulus_frame =
                    aligned ? std::optional<int32_t>(
                                  aligned->decoded_frame.metadata.frame_number)
                            : std::nullopt;
                const auto presentation = stimulus_presentation.update(
                    mapped_camera_frame, resolution, decoded_stimulus_frame);
                if (presentation.commit_composite) {
                  candidate_stimulus_resolution = resolution;
                  if (presentation.render_current) {
                    if (aligned) {
                      candidate_stimulus_frame = std::move(aligned);
                    } else if (!candidate_stimulus_frame) {
                      candidate_stimulus_frame = current_stimulus_frame;
                    }
                    if (!candidate_stimulus_frame ||
                        !resolution.stimulus_frame ||
                        candidate_stimulus_frame->decoded_frame.metadata
                                .frame_number != *resolution.stimulus_frame) {
                      stimulus_error =
                          "composite stimulus frame does not match camera "
                          "mapping";
                      stimulus_failed = true;
                    }
                  } else {
                    candidate_stimulus_frame.reset();
                  }
                  stimulus_candidate_ready = !stimulus_failed;
                }
              }
            }
          }
          if (stimulus_failed) {
            candidate_stimulus_frame.reset();
            stimulus_candidate_ready = pending_video_frame.has_value();
            stimulus_presentation.resetVisibleFrame();
          }
        } else {
          if (present_selected &&
              (!pending_video_frame || pending_camera_discontinuity)) {
            pending_video_frame = std::move(selected);
          }
          candidate_stimulus_frame.reset();
          stimulus_candidate_ready = pending_video_frame.has_value();
          pending_camera_discontinuity = false;
        }

        auto commit_stimulus_candidate = [&] {
          if (!pending_video_frame || !stimulus_candidate_ready) {
            return;
          }
          current_video_frame = std::move(pending_video_frame);
          pending_video_frame.reset();
          current_stimulus_frame = std::move(candidate_stimulus_frame);
          candidate_stimulus_frame.reset();
          current_stimulus_resolution = candidate_stimulus_resolution;
          stimulus_candidate_ready = false;
        };

        if (crop_enabled &&
            crop_controls.preference != active_crop_preference) {
          active_crop_preference = crop_controls.preference;
          workspace_state.setCropSourcePreference(active_crop_preference);
          last_crop_camera_request = -1;
          pending_crop_discontinuity = true;
          if (active_crop_preference ==
              crimson::crop::CropSourcePreference::PreferLiveGeometry) {
            if (crop_playback.isOpen()) {
              crop_playback.suspend();
            }
          }
        }

        if (crop_enabled && pending_video_frame && stimulus_candidate_ready &&
            !crop_failed) {
          const int64_t camera_frame =
              pending_video_frame->metadata.frame_number;
          const bool acquisition_preferred =
              crop_controls.acquisition_available &&
              crop_controls.preference ==
                  crimson::crop::CropSourcePreference::PreferAcquisitionVideo;
          if (acquisition_preferred &&
              (last_crop_camera_request != camera_frame ||
               pending_crop_discontinuity)) {
            if (!crop_playback.requestCameraFrame(
                    camera_frame, pending_crop_discontinuity, &crop_error)) {
              std::fprintf(
                  stderr, "[AppleCrop] Camera frame %lld request failed: %s\n",
                  static_cast<long long>(camera_frame), crop_error.c_str());
              crop_failed = true;
            } else {
              last_crop_camera_request = camera_frame;
              pending_crop_discontinuity = false;
            }
          }

          if (!crop_failed) {
            auto aligned =
                acquisition_preferred
                    ? crop_playback.frameForCameraFrame(camera_frame)
                    : std::optional<AppleAlignedAcquisitionCropFrame>{};
            crimson::crop::CropFrameSourceState source_state;
            source_state.camera_frame = camera_frame;
            source_state.exact_full_frame = camera_frame;
            crimson::crop::CropSourceCapabilities source_capabilities;
            int64_t crop_camera_frame_count = 0;
            if (const auto *repository = crop_playback.repository()) {
              source_capabilities = repository->sourceCapabilities();
              source_state.live_geometry = repository->liveGeometry(
                  camera_frame, video_playback.info().width,
                  video_playback.info().height);
              source_state.acquisition = repository->acquisitionFrameState(
                  camera_frame,
                  aligned ? std::optional<int64_t>(
                                aligned->decoded_frame.metadata.frame_number)
                          : std::nullopt,
                  video_playback.info().width, video_playback.info().height);
              crop_camera_frame_count =
                  static_cast<int64_t>(repository->cameraFrameCount());
            }
            if (analysis_crop_geometry) {
              const auto geometry_resolution =
                  analysis_crop_geometry->resolveCameraFrame(
                      camera_frame, video_playback.info().width,
                      video_playback.info().height);
              source_state.live_geometry = geometry_resolution.geometry;
              source_capabilities.live_geometry = true;
              crop_camera_frame_count = std::max<int64_t>(
                  crop_camera_frame_count,
                  static_cast<int64_t>(
                      analysis_crop_geometry->descriptor().camera_frame_count));
            }
            const auto selection = crimson::crop::SelectCropSource(
                source_capabilities, source_state, crop_controls.preference,
                crimson::crop::CropFallbackPolicy::WaitForPreferred,
                crop_camera_frame_count);
            crop_controls.selection_status = selection.status;
            std::optional<int64_t> surface_camera_frame;
            if (selection.selected()) {
              if (selection.source ==
                  crimson::crop::CropSourceKind::LiveGeometry) {
                surface_camera_frame = camera_frame;
              } else if (aligned) {
                surface_camera_frame = aligned->resolution.camera_frame;
              }
            }
            const auto crop_decision = crop_presentation.update(
                camera_frame, selection, surface_camera_frame);
            if (selection.status ==
                crimson::crop::CropSourceSelectionStatus::InvalidState) {
              crop_error = "crop source selection returned invalid state";
              crop_failed = true;
            } else if (crop_decision.action ==
                       crimson::crop::CropPresentationAction::Present) {
              current_crop_selection = selection;
              if (selection.source ==
                  crimson::crop::CropSourceKind::AcquisitionVideo) {
                if (!aligned) {
                  crop_error =
                      "selected acquisition crop has no exact decoded frame";
                  crop_failed = true;
                } else {
                  current_crop_frame = std::move(aligned);
                }
              } else {
                current_crop_frame.reset();
              }
              if (!crop_failed) {
                commit_stimulus_candidate();
              }
            } else if (crop_decision.action ==
                       crimson::crop::CropPresentationAction::Hold) {
              commit_stimulus_candidate();
            } else if (crop_decision.action ==
                       crimson::crop::CropPresentationAction::Clear) {
              current_crop_frame.reset();
              current_crop_selection = {};
              commit_stimulus_candidate();
            }
          }
        } else if ((!crop_enabled || crop_failed) && stimulus_candidate_ready) {
          commit_stimulus_candidate();
        }

        if (current_video_frame) {
          const bool presentation_was_discontinuous =
              viewer_presentation_discontinuity;
          viewer_stats.presented_frame =
              current_video_frame->metadata.frame_number;
          const auto active_seek =
              video_clock.seekCoordinator().activeTransaction();
          if (active_seek.has_value() && active_seek->request.target_frame ==
                                             viewer_stats.presented_frame) {
            crimson::playback::PlaybackSeekExecutionResult completion;
            completion.status =
                crimson::playback::PlaybackSeekExecutionStatus::Completed;
            completion.path =
                crimson::playback::PlaybackSeekExecutionPath::BackendDecoder;
            completion.resolved_frame = viewer_stats.presented_frame;
            video_clock.seekCoordinator().recordActive(std::move(completion));
          }
          const int64_t previous_presented_frame =
              viewer_stats.last_presented_frame;
          if (viewer_stats.presented_frame != previous_presented_frame &&
              (!apple_presentation.plan.active ||
               apple_presentation.commit.committed)) {
            viewer_presentation_discontinuity = false;
          }
          const auto &metadata = current_video_frame->metadata;
          ui_reference_camera_exact =
              options->ui_reference.enabled && !video_clock.isPlaying() &&
              viewer_stats.requested_frame ==
                  options->ui_reference.target_frame &&
              metadata.frame_number == options->ui_reference.target_frame &&
              viewportHasArea(video_viewports.camera);
          std::optional<double> presentation_lag_frames;
          if (metadata.time_base.isValid() &&
              video_playback.info().nominal_frame_rate > 0.0) {
            const double presented_seconds =
                static_cast<double>(metadata.frame_pts) *
                static_cast<double>(metadata.time_base.numerator) /
                static_cast<double>(metadata.time_base.denominator);
            const double requested_seconds =
                static_cast<double>(viewer_stats.requested_frame) /
                video_playback.info().nominal_frame_rate;
            viewer_stats.pts_error_frames =
                (presented_seconds - requested_seconds) *
                video_playback.info().nominal_frame_rate;
            const double lag_frames =
                std::max(0.0, -viewer_stats.pts_error_frames);
            presentation_lag_frames = lag_frames;
          }
          camera_presentation_tracker.record(
              viewer_stats.requested_frame, viewer_stats.presented_frame,
              presentation_was_discontinuous, presentation_lag_frames);
          const auto &camera_presentation_metrics =
              camera_presentation_tracker.metrics();
          viewer_stats.repeated_presentations =
              camera_presentation_metrics.repeated_presentations;
          viewer_stats.skipped_source_frames =
              camera_presentation_metrics.skipped_source_frames;
          viewer_stats.late_presentations =
              camera_presentation_metrics.late_presentations;
          viewer_stats.presentation_count =
              camera_presentation_metrics.presentation_count;
          viewer_stats.last_presented_frame =
              camera_presentation_metrics.last_presented_frame;
          viewer_stats.max_lag_frames =
              camera_presentation_metrics.max_lag_frames;
          bool stimulus_frame_encoded = false;
          bool crop_frame_encoded = false;
          std::string render_error;
          std::shared_ptr<const crimson::polar::ChaserDistancePolarFrameSample>
              chaser_distance_polar_sample;
          crimson::polar::ChaserDistancePolarScene chaser_distance_polar_scene;
          crimson::stimulus::StimulusCameraOverlayScene
              stimulus_camera_overlay_scene;
          if (analysis_presentation_demand_enabled &&
              chaser_distance_polar_available) {
            const bool polar_discontinuity =
                presentation_was_discontinuous &&
                last_chaser_distance_polar_camera_request >= 0;
            if (!chaser_distance_polar_buffer.requestFrame(
                    metadata.frame_number, polar_discontinuity,
                    &chaser_distance_polar_error)) {
              std::fprintf(stderr, "[AppleChaserPolar] Request failed: %s\n",
                           chaser_distance_polar_error.c_str());
              chaser_distance_polar_failed = true;
              chaser_distance_polar_available = false;
            } else {
              last_chaser_distance_polar_camera_request = metadata.frame_number;
              chaser_distance_polar_sample =
                  chaser_distance_polar_buffer.frame(metadata.frame_number);
            }
          }
          if (chaser_distance_polar_sample &&
              viewportHasArea(video_viewports.camera) &&
              io.DisplayFramebufferScale.x > 0.0f &&
              io.DisplayFramebufferScale.y > 0.0f) {
            chaser_distance_polar_scene =
                crimson::polar::buildChaserDistancePolarScene(
                    *chaser_distance_polar_sample,
                    {video_viewports.camera.width /
                         io.DisplayFramebufferScale.x,
                     video_viewports.camera.height /
                         io.DisplayFramebufferScale.y},
                    frame_inspect_presentation.polar_inset);
          }
          if (stimulus_context_timeline_snapshot &&
              viewportHasArea(video_viewports.camera) &&
              io.DisplayFramebufferScale.x > 0.0f &&
              io.DisplayFramebufferScale.y > 0.0f) {
            const auto stimulus_camera_overlay_frame =
                crimson::stimulus::resolveStimulusCameraOverlayFrame(
                    stimulus_context_timeline_snapshot.get(),
                    metadata.frame_number);
            const std::string stimulus_event_text =
                crimson::stimulus::stimulusCameraOverlayEventText(
                    stimulus_camera_overlay_frame);
            ImVec2 event_text_size{};
            if (!stimulus_event_text.empty()) {
              event_text_size = ImGui::CalcTextSize(stimulus_event_text.c_str(),
                                                    nullptr, false, -1.0f);
            }
            stimulus_camera_overlay_scene =
                crimson::stimulus::buildStimulusCameraOverlayScene(
                    stimulus_camera_overlay_frame,
                    {video_viewports.camera.width /
                         io.DisplayFramebufferScale.x,
                     video_viewports.camera.height /
                         io.DisplayFramebufferScale.y},
                    {event_text_size.x, event_text_size.y});
          }
          const std::array<AppleMetalVideoViewport *, 5> requested_viewports = {
              &video_viewports.camera, &video_viewports.crop_inset,
              &video_viewports.crop_preview, &video_viewports.stimulus_inset,
              &video_viewports.stimulus_debug};
          for (AppleMetalVideoViewport *viewport : requested_viewports) {
            if (!viewportHasArea(*viewport) ||
                viewportFitsDrawable(*viewport, width, height)) {
              continue;
            }
            if (!invalid_workspace_viewport_logged) {
              std::fprintf(
                  stderr,
                  "[MacShell] Suppressed out-of-bounds workspace viewport "
                  "drawable=%dx%d scale=%.3f viewport=%.3f,%.3f %.3fx%.3f\n",
                  width, height, io.DisplayFramebufferScale.y, viewport->x,
                  viewport->y, viewport->width, viewport->height);
              invalid_workspace_viewport_logged = true;
            }
            *viewport = {};
          }
          if (viewportHasArea(video_viewports.camera) &&
              !video_renderer.encodeRegion(
                  *current_video_frame,
                  reinterpret_cast<uintptr_t>((__bridge void *)command_buffer),
                  reinterpret_cast<uintptr_t>((__bridge void *)encoder),
                  video_viewports.camera,
                  {camera_view_state.source_region.x,
                   camera_view_state.source_region.y,
                   camera_view_state.source_region.width,
                   camera_view_state.source_region.height},
                  &render_error)) {
            std::fprintf(stderr, "[AppleVideo] Metal encode failed: %s\n",
                         render_error.c_str());
            render_failed = true;
            [encoder endEncoding];
            break;
          }
          if (stimulus_enabled && current_stimulus_frame) {
            if (viewportHasArea(video_viewports.stimulus_inset)) {
              if (!video_renderer.encodeRegion(
                      current_stimulus_frame->decoded_frame,
                      reinterpret_cast<uintptr_t>(
                          (__bridge void *)command_buffer),
                      reinterpret_cast<uintptr_t>((__bridge void *)encoder),
                      video_viewports.stimulus_inset,
                      {0.0, 0.0, 1.0, 1.0, false,
                       frame_inspect_presentation.stimulus_inset_opacity},
                      &render_error)) {
                std::fprintf(stderr,
                             "[AppleStimulus] Metal encode failed: %s\n",
                             render_error.c_str());
                render_failed = true;
              } else {
                stimulus_frame_encoded = true;
              }
            }
            if (render_failed) {
              [encoder endEncoding];
              break;
            }
          }
          if (stimulus_enabled &&
              viewportHasArea(video_viewports.stimulus_debug)) {
            const AppleDecodedVideoFrame *debug_frame =
                selected_stimulus_debug_frame ? &*selected_stimulus_debug_frame
                : current_stimulus_frame
                    ? &current_stimulus_frame->decoded_frame
                    : nullptr;
            if (debug_frame != nullptr &&
                !video_renderer.encode(
                    *debug_frame,
                    reinterpret_cast<uintptr_t>(
                        (__bridge void *)command_buffer),
                    reinterpret_cast<uintptr_t>((__bridge void *)encoder),
                    video_viewports.stimulus_debug, &render_error)) {
              std::fprintf(stderr,
                           "[AppleStimulus] Debug Metal encode failed: %s\n",
                           render_error.c_str());
              render_failed = true;
              [encoder endEncoding];
              break;
            }
          }
          if (crop_enabled && current_crop_selection.selected() &&
              current_crop_selection.camera_frame == metadata.frame_number) {
            auto encode_crop = [&](const AppleMetalVideoViewport &viewport) {
              if (!viewportHasArea(viewport)) {
                return true;
              }
              bool encoded = false;
              if (current_crop_selection.source ==
                      crimson::crop::CropSourceKind::AcquisitionVideo &&
                  current_crop_frame &&
                  current_crop_frame->resolution.camera_frame ==
                      metadata.frame_number) {
                encoded = video_renderer.encodeRegion(
                    current_crop_frame->decoded_frame,
                    reinterpret_cast<uintptr_t>(
                        (__bridge void *)command_buffer),
                    reinterpret_cast<uintptr_t>((__bridge void *)encoder),
                    viewport, {0.0, 0.0, 1.0, 1.0, false}, &render_error);
              } else if (current_crop_selection.source ==
                             crimson::crop::CropSourceKind::LiveGeometry &&
                         current_crop_selection.geometry) {
                const auto &geometry = *current_crop_selection.geometry;
                const AppleMetalVideoSourceRegion source_region{
                    geometry.full_frame_crop.x / geometry.source_width,
                    geometry.full_frame_crop.y / geometry.source_height,
                    geometry.full_frame_crop.width / geometry.source_width,
                    geometry.full_frame_crop.height / geometry.source_height,
                    false};
                encoded = video_renderer.encodeRegion(
                    *current_video_frame,
                    reinterpret_cast<uintptr_t>(
                        (__bridge void *)command_buffer),
                    reinterpret_cast<uintptr_t>((__bridge void *)encoder),
                    viewport, source_region, &render_error);
              }
              crop_frame_encoded = crop_frame_encoded || encoded;
              return encoded;
            };
            for (const AppleMetalVideoViewport *viewport :
                 {&video_viewports.crop_inset, &video_viewports.crop_preview}) {
              if (!viewportHasArea(*viewport)) {
                continue;
              }
              if (!encode_crop(*viewport)) {
                std::fprintf(stderr, "[AppleCrop] Metal encode failed: %s\n",
                             render_error.c_str());
                render_failed = true;
                break;
              }
            }
            if (render_failed) {
              [encoder endEncoding];
              break;
            }
          }
          crimson::overlay::ReadOnlyOverlayInput overlay_input;
          overlay_input.identity = {0, metadata.frame_number, 0,
                                    metadata.frame_number};
          overlay_input.source_width = video_playback.info().width;
          overlay_input.source_height = video_playback.info().height;
          overlay_input.show_boxes = false;
          bool canonical_detection_ready = false;
          size_t presented_canonical_detections = 0;
          std::shared_ptr<const crimson::zarr::CanonicalDetectionFrame>
              canonical_detection_frame;
          if (analysis_presentation_demand_enabled &&
              canonical_detection_available) {
            const bool detection_discontinuity =
                presentation_was_discontinuous &&
                last_canonical_detection_camera_request >= 0;
            if (!canonical_detection_buffer.requestFrame(
                    metadata.frame_number, detection_discontinuity,
                    &canonical_detection_error)) {
              canonical_detection_failed = true;
              canonical_detection_available = false;
              presented_canonical_detection_frame.reset();
              std::fprintf(stderr,
                           "[AppleCanonicalDetection] Request failed: %s\n",
                           canonical_detection_error.c_str());
            } else {
              last_canonical_detection_camera_request = metadata.frame_number;
              canonical_detection_frame =
                  canonical_detection_buffer.frame(metadata.frame_number);
              canonical_detection_ready =
                  canonical_detection_frame &&
                  canonical_detection_frame->camera_frame ==
                      metadata.frame_number;
              if (canonical_detection_ready) {
                presented_canonical_detection_frame = canonical_detection_frame;
                presented_canonical_detections =
                    canonical_detection_frame->detections.size();
              }
            }
          }
          bool keypoint_overlay_ready = false;
          size_t presented_keypoint_detections = 0;
          if (analysis_presentation_demand_enabled &&
              keypoint_overlay_available) {
            const bool keypoint_discontinuity =
                presentation_was_discontinuous &&
                last_keypoint_camera_request >= 0;
            if (!keypoint_overlay_buffer.requestFrame(
                    metadata.frame_number, video_playback.info().width,
                    video_playback.info().height, keypoint_discontinuity,
                    &keypoint_error)) {
              keypoint_overlay_failed = true;
              keypoint_overlay_available = false;
              presented_keypoint_resolution.reset();
              std::fprintf(stderr, "[AppleKeypoints] Request failed: %s\n",
                           keypoint_error.c_str());
            } else {
              last_keypoint_camera_request = metadata.frame_number;
            }
            const auto keypoint_resolution =
                keypoint_overlay_buffer.frame(metadata.frame_number);
            if (keypoint_resolution &&
                keypoint_resolution->camera_frame == metadata.frame_number) {
              presented_keypoint_resolution = keypoint_resolution;
            }
            if (keypoint_resolution &&
                keypoint_resolution->status ==
                    crimson::zarr::KeypointOverlayStatus::Mapped &&
                keypoint_resolution->camera_frame == metadata.frame_number) {
              overlay_input = crimson::zarr::makeKeypointOverlaySceneInput(
                  keypoint_descriptor, *keypoint_resolution, 0,
                  metadata.frame_number, 0, video_playback.info().width,
                  video_playback.info().height);
              keypoint_overlay_ready = true;
              presented_keypoint_detections =
                  keypoint_resolution->detections.size();
            }
          }

          bool subject_mask_overlay_ready = false;
          size_t presented_subject_mask_detections = 0;
          size_t presented_subject_mask_components = 0;
          const auto subject_mask_presentation =
              subject_mask_frame_presentation.update(
                  {metadata.frame_number, video_playback.info().width,
                   video_playback.info().height,
                   analysis_presentation_demand_enabled,
                   overlay_controls.show_subject_masks,
                   subject_mask_overlay_available,
                   presentation_was_discontinuous},
                  subject_mask_overlay_buffer, subject_mask_descriptor,
                  overlay_input, &subject_mask_error);
          if (subject_mask_presentation.action ==
              crimson::overlay::ReadOnlyOverlayFrameAction::RequestRejected) {
            std::fprintf(stderr, "[AppleSubjectMasks] Request failed: %s\n",
                         subject_mask_error.c_str());
            subject_mask_overlay_failed = true;
            subject_mask_overlay_available = false;
          } else if (subject_mask_presentation.overlay_ready) {
            logSubjectMaskFirstReady();
            subject_mask_overlay_ready = true;
            presented_subject_mask_detections =
                subject_mask_presentation.detection_count;
            presented_subject_mask_components =
                subject_mask_presentation.component_count;
          }

          bool subject_shape_overlay_ready = false;
          size_t presented_subject_shape_detections = 0;
          if (analysis_presentation_demand_enabled &&
              subject_shape_overlay_available) {
            const bool subject_shape_discontinuity =
                presentation_was_discontinuous &&
                last_subject_shape_camera_request >= 0;
            if (!subject_shape_overlay_buffer.requestFrame(
                    metadata.frame_number, video_playback.info().width,
                    video_playback.info().height, subject_shape_discontinuity,
                    &subject_shape_error)) {
              std::fprintf(stderr, "[AppleSubjectShape] Request failed: %s\n",
                           subject_shape_error.c_str());
              subject_shape_overlay_failed = true;
              subject_shape_overlay_available = false;
            } else {
              last_subject_shape_camera_request = metadata.frame_number;
              const auto resolution =
                  subject_shape_overlay_buffer.frame(metadata.frame_number);
              if (resolution &&
                  resolution->status ==
                      crimson::zarr::SubjectShapeOverlayStatus::Mapped &&
                  resolution->camera_frame == metadata.frame_number) {
                subject_shape_overlay_ready =
                    crimson::zarr::appendSubjectShapeOverlaySceneInput(
                        subject_shape_descriptor, *resolution,
                        metadata.frame_number, &overlay_input);
                if (subject_shape_overlay_ready) {
                  presented_subject_shape_detections =
                      resolution->detections.size();
                }
              }
            }
          }

          bool eye_geometry_overlay_ready = false;
          size_t presented_eye_geometry_detections = 0;
          if (analysis_presentation_demand_enabled &&
              eye_geometry_overlay_available) {
            const bool eye_geometry_discontinuity =
                presentation_was_discontinuous &&
                last_eye_geometry_camera_request >= 0;
            if (!eye_geometry_overlay_buffer.requestFrame(
                    metadata.frame_number, video_playback.info().width,
                    video_playback.info().height, eye_geometry_discontinuity,
                    &eye_geometry_error)) {
              std::fprintf(stderr, "[AppleEyeGeometry] Request failed: %s\n",
                           eye_geometry_error.c_str());
              eye_geometry_overlay_failed = true;
              eye_geometry_overlay_available = false;
            } else {
              last_eye_geometry_camera_request = metadata.frame_number;
              const auto resolution =
                  eye_geometry_overlay_buffer.frame(metadata.frame_number);
              if (resolution &&
                  resolution->status ==
                      crimson::zarr::EyeGeometryOverlayStatus::Mapped &&
                  resolution->camera_frame == metadata.frame_number) {
                eye_geometry_overlay_ready =
                    crimson::zarr::appendEyeGeometryOverlaySceneInput(
                        eye_geometry_descriptor, *resolution,
                        metadata.frame_number, &overlay_input);
                if (eye_geometry_overlay_ready) {
                  presented_eye_geometry_detections =
                      resolution->detections.size();
                }
              }
            }
          }

          crimson::overlay::applyReadOnlyOverlayControls(overlay_controls,
                                                         &overlay_input);
          auto overlay_scene =
              crimson::overlay::buildReadOnlyOverlayScene(overlay_input);
          if (canonical_detection_ready && canonical_detection_frame) {
            auto detection_input =
                crimson::zarr::makeCanonicalDetectionOverlaySceneInput(
                    canonical_detection_descriptor, *canonical_detection_frame,
                    0, metadata.frame_number, 0, video_playback.info().width,
                    video_playback.info().height);
            auto detection_scene =
                crimson::overlay::buildReadOnlyOverlayScene(detection_input);
            overlay_scene.primitives.insert(overlay_scene.primitives.end(),
                                            detection_scene.primitives.begin(),
                                            detection_scene.primitives.end());
            overlay_scene.text_annotations.insert(
                overlay_scene.text_annotations.end(),
                detection_scene.text_annotations.begin(),
                detection_scene.text_annotations.end());
          }
          if (options->ui_reference.enabled &&
              (options->ui_reference.state ==
                   AppleUiReferenceState::Keypoints ||
               options->ui_reference.state == AppleUiReferenceState::Overlays ||
               options->ui_reference.state ==
                   AppleUiReferenceState::AnalysisEye) &&
              metadata.frame_number == options->ui_reference.target_frame) {
            ui_reference_overlay_counts = {
                {"keypoints",
                 overlay_scene.count(
                     crimson::overlay::CameraOverlayLayer::Keypoints)},
                {"headings",
                 overlay_scene.count(
                     crimson::overlay::CameraOverlayLayer::KeypointHeading)},
                {"subject_mask_primitives",
                 overlay_scene.count(
                     crimson::overlay::CameraOverlayLayer::SubjectMasks)},
                {"subject_mask_rasters",
                 overlay_scene.rasterCount(
                     crimson::overlay::CameraOverlayLayer::SubjectMasks)},
                {"subject_mask_text",
                 overlay_scene.textCount(
                     crimson::overlay::CameraOverlayLayer::SubjectMasks)},
                {"subject_shape",
                 overlay_scene.count(
                     crimson::overlay::CameraOverlayLayer::SubjectShape)}};
            if (options->ui_reference.state ==
                AppleUiReferenceState::Keypoints) {
              ui_reference_overlay_ready =
                  overlay_scene.ready() && keypoint_overlay_ready &&
                  overlay_scene.count(
                      crimson::overlay::CameraOverlayLayer::Keypoints) > 0 &&
                  overlay_scene.count(
                      crimson::overlay::CameraOverlayLayer::KeypointHeading) >
                      0;
            } else {
              ui_reference_overlay_ready =
                  overlay_scene.ready() && keypoint_overlay_ready &&
                  subject_mask_overlay_ready && eye_geometry_overlay_ready &&
                  overlay_scene.rasterCount(
                      crimson::overlay::CameraOverlayLayer::SubjectMasks) > 0;
            }
          }
          if (current_crop_selection.selected() &&
              current_crop_selection.camera_frame == metadata.frame_number &&
              current_crop_selection.geometry &&
              current_crop_selection.geometry->full_frame_detection) {
            const auto &geometry = *current_crop_selection.geometry;
            if (geometry.camera_frame == metadata.frame_number &&
                geometry.source_width == video_playback.info().width &&
                geometry.source_height == video_playback.info().height) {
              const auto &box = *geometry.full_frame_detection;
              crimson::overlay::ReadOnlyOverlayInput overlay_input;
              overlay_input.identity = {0, metadata.frame_number, 0,
                                        geometry.camera_frame};
              overlay_input.source_width = geometry.source_width;
              overlay_input.source_height = geometry.source_height;
              overlay_input.show_headings = false;
              overlay_input.show_keypoints = false;
              crimson::overlay::DetectionOverlayInput detection;
              detection.box = crimson::overlay::DetectionBoxInput{
                  {box.x, box.y, box.width, box.height},
                  0,
                  crimson::overlay::BoxProvenance::Clean};
              overlay_input.detections.push_back(std::move(detection));
              const auto crop_overlay_scene =
                  crimson::overlay::buildReadOnlyOverlayScene(overlay_input);
              overlay_scene.primitives.insert(
                  overlay_scene.primitives.end(),
                  crop_overlay_scene.primitives.begin(),
                  crop_overlay_scene.primitives.end());
            }
          }
          if (viewportHasArea(video_viewports.camera) &&
              overlay_scene.ready() &&
              (!overlay_scene.primitives.empty() ||
               !overlay_scene.raster_masks.empty() ||
               !overlay_scene.text_annotations.empty())) {
            const crimson::overlay::SourceViewportTransform transform{
                {camera_view_state.source_region.x * overlay_scene.source_width,
                 camera_view_state.source_region.y *
                     overlay_scene.source_height,
                 camera_view_state.source_region.width *
                     overlay_scene.source_width,
                 camera_view_state.source_region.height *
                     overlay_scene.source_height},
                {video_viewports.camera.x, video_viewports.camera.y,
                 video_viewports.camera.width, video_viewports.camera.height}};
            if (!overlay_renderer.encode(
                    overlay_scene, transform,
                    reinterpret_cast<uintptr_t>((__bridge void *)encoder),
                    static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                    &render_error)) {
              std::fprintf(stderr, "[AppleOverlay] Metal encode failed: %s\n",
                           render_error.c_str());
              render_failed = true;
              [encoder endEncoding];
              break;
            }
            const size_t drawn_overlay_labels = drawAppleReadOnlyOverlayText(
                overlay_scene, transform, io.DisplayFramebufferScale.x,
                io.DisplayFramebufferScale.y);
            ++read_only_overlay_presentations;
            if (canonical_detection_ready &&
                presented_canonical_detections > 0) {
              ++canonical_detection_presentations;
              canonical_detection_detections += presented_canonical_detections;
            }
            if (keypoint_overlay_ready &&
                (overlay_scene.count(
                     crimson::overlay::CameraOverlayLayer::KeypointHeading) >
                     0 ||
                 overlay_scene.count(
                     crimson::overlay::CameraOverlayLayer::Keypoints) > 0)) {
              ++keypoint_overlay_presentations;
              keypoint_overlay_detections += presented_keypoint_detections;
            }
            if (subject_mask_overlay_ready &&
                (overlay_scene.rasterCount(
                     crimson::overlay::CameraOverlayLayer::SubjectMasks) > 0 ||
                 overlay_scene.count(
                     crimson::overlay::CameraOverlayLayer::SubjectMasks) > 0)) {
              ++subject_mask_overlay_presentations;
              subject_mask_overlay_detections +=
                  presented_subject_mask_detections;
              subject_mask_overlay_components +=
                  presented_subject_mask_components;
            }
            if (subject_shape_overlay_ready &&
                overlay_scene.count(
                    crimson::overlay::CameraOverlayLayer::SubjectShape) > 0) {
              ++subject_shape_overlay_presentations;
              subject_shape_overlay_detections +=
                  presented_subject_shape_detections;
            }
            if (eye_geometry_overlay_ready &&
                presented_eye_geometry_detections > 0) {
              ++eye_geometry_overlay_presentations;
              eye_geometry_overlay_detections +=
                  presented_eye_geometry_detections;
              eye_geometry_overlay_labels += drawn_overlay_labels;
            }
          }
          if (stimulus_camera_overlay_scene.ready()) {
            if (!overlay_renderer.encodeStimulusCameraOverlay(
                    stimulus_camera_overlay_scene,
                    {video_viewports.camera.x, video_viewports.camera.y},
                    io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y,
                    reinterpret_cast<uintptr_t>((__bridge void *)encoder),
                    static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                    &render_error)) {
              std::fprintf(
                  stderr,
                  "[AppleStimulusCameraOverlay] Metal encode failed: %s\n",
                  render_error.c_str());
              render_failed = true;
              [encoder endEncoding];
              break;
            }
            drawAppleStimulusCameraOverlayText(
                stimulus_camera_overlay_scene, video_viewports.camera,
                io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
            if (options->ui_reference.enabled &&
                options->ui_reference.state ==
                    AppleUiReferenceState::StimulusOverlay &&
                metadata.frame_number == options->ui_reference.target_frame) {
              ui_reference_stimulus_overlay_ready =
                  stimulus_camera_overlay_scene.requested_camera_frame ==
                      options->ui_reference.target_frame &&
                  stimulus_camera_overlay_scene.source_camera_frame ==
                      options->ui_reference.target_frame &&
                  !stimulus_camera_overlay_scene.primitives.empty() &&
                  !stimulus_camera_overlay_scene.text.empty();
              ui_reference_stimulus_overlay_scene =
                  stimulus_camera_overlay_scene;
              ui_reference_stimulus_overlay_viewport = video_viewports.camera;
            }
          }
          if (chaser_distance_polar_scene.ready()) {
            if (!overlay_renderer.encodePolar(
                    chaser_distance_polar_scene,
                    {video_viewports.camera.x, video_viewports.camera.y},
                    io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y,
                    reinterpret_cast<uintptr_t>((__bridge void *)encoder),
                    static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                    &render_error)) {
              std::fprintf(stderr,
                           "[AppleChaserPolar] Metal encode failed: %s\n",
                           render_error.c_str());
              render_failed = true;
              [encoder endEncoding];
              break;
            }
            drawAppleChaserDistancePolarText(
                chaser_distance_polar_scene, video_viewports.camera,
                io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
            ++chaser_distance_polar_presentations;
            chaser_distance_polar_points +=
                chaser_distance_polar_scene.point_count;
            if (options->ui_reference.enabled &&
                options->ui_reference.state == AppleUiReferenceState::Polar &&
                metadata.frame_number == options->ui_reference.target_frame) {
              ui_reference_polar_ready =
                  chaser_distance_polar_scene.requested_camera_frame ==
                      options->ui_reference.target_frame &&
                  chaser_distance_polar_scene.source_camera_frame ==
                      options->ui_reference.target_frame &&
                  chaser_distance_polar_scene.point_count > 0;
              ui_reference_polar_scene = chaser_distance_polar_scene;
              ui_reference_polar_viewport = video_viewports.camera;
            }
          }
          if (crop_enabled) {
            if (frame_inspect_presentation.roi_inset.show_label) {
              drawAppleCropPreviewOverlay(
                  video_viewports.crop_inset, io.DisplayFramebufferScale.y,
                  crop_frame_encoded ? &current_crop_selection : nullptr,
                  crop_frame_encoded ? current_crop_selection.status
                                     : crop_controls.selection_status);
            }
            drawAppleCropPreviewOverlay(
                video_viewports.crop_preview, io.DisplayFramebufferScale.y,
                crop_frame_encoded ? &current_crop_selection : nullptr,
                crop_frame_encoded ? current_crop_selection.status
                                   : crop_controls.selection_status);
          }
          if (stimulus_frame_encoded &&
              frame_inspect_presentation.show_stimulus_frame_label &&
              current_stimulus_frame) {
            drawAppleStimulusInsetOverlay(
                video_viewports.stimulus_inset, io.DisplayFramebufferScale.y,
                metadata.frame_number,
                current_stimulus_frame->decoded_frame.metadata.frame_number);
          }
          if (options->ui_reference.enabled) {
            ui_reference_crop_exact =
                crop_frame_encoded && current_crop_selection.selected() &&
                current_crop_selection.source ==
                    crimson::crop::CropSourceKind::LiveGeometry &&
                current_crop_selection.camera_frame ==
                    options->ui_reference.target_frame &&
                crop_presentation.metrics().presented_crop_camera_frame ==
                    options->ui_reference.target_frame;
            ui_reference_stimulus_exact =
                stimulus_frame_encoded && current_stimulus_frame &&
                current_stimulus_resolution.status ==
                    crimson::zarr::StimulusMappingStatus::Mapped &&
                current_stimulus_resolution.camera_frame ==
                    options->ui_reference.target_frame &&
                current_stimulus_resolution.stimulus_frame &&
                current_stimulus_frame->decoded_frame.metadata.frame_number ==
                    *current_stimulus_resolution.stimulus_frame;
          }
          if (options->stimulus_smoke &&
              metadata.frame_number >= options->video_smoke_end) {
            if (current_stimulus_resolution.status !=
                crimson::zarr::StimulusMappingStatus::Mapped) {
              stimulus_error = "stimulus smoke end camera frame is not mapped";
              stimulus_failed = true;
            } else {
              stimulus_smoke_end_satisfied =
                  stimulus_frame_encoded &&
                  current_stimulus_resolution.stimulus_frame &&
                  current_stimulus_frame &&
                  current_stimulus_frame->decoded_frame.metadata.frame_number ==
                      *current_stimulus_resolution.stimulus_frame;
            }
          }
          if (options->crop_smoke &&
              metadata.frame_number >= options->video_smoke_end) {
            const auto expected_source =
                crop_controls.preference ==
                        crimson::crop::CropSourcePreference::PreferLiveGeometry
                    ? crimson::crop::CropSourceKind::LiveGeometry
                    : crimson::crop::CropSourceKind::AcquisitionVideo;
            crop_smoke_end_satisfied =
                crop_frame_encoded && current_crop_selection.selected() &&
                current_crop_selection.source == expected_source &&
                current_crop_selection.camera_frame == metadata.frame_number &&
                crop_presentation.metrics().presented_crop_camera_frame ==
                    metadata.frame_number;
          }
          if (options->multistream_smoke) {
            const bool stimulus_exact =
                stimulus_frame_encoded &&
                current_stimulus_resolution.status ==
                    crimson::zarr::StimulusMappingStatus::Mapped &&
                current_stimulus_resolution.camera_frame ==
                    metadata.frame_number &&
                current_stimulus_resolution.stimulus_frame &&
                current_stimulus_frame &&
                current_stimulus_frame->decoded_frame.metadata.frame_number ==
                    *current_stimulus_resolution.stimulus_frame;
            const auto expected_source =
                options->crop_preference ==
                        crimson::crop::CropSourcePreference::PreferLiveGeometry
                    ? crimson::crop::CropSourceKind::LiveGeometry
                    : crimson::crop::CropSourceKind::AcquisitionVideo;
            const bool crop_exact =
                crop_frame_encoded && current_crop_selection.selected() &&
                current_crop_selection.source == expected_source &&
                current_crop_selection.camera_frame == metadata.frame_number &&
                crop_presentation.metrics().presented_crop_camera_frame ==
                    metadata.frame_number;
            multistream_composite_exact = stimulus_exact && crop_exact;
            multistream_composite_frame = metadata.frame_number;
          }
        }
      }

      crimson::ui::drawSessionLoadingModal(
          crimson::session::makeSessionLoadingPresentation(analysis_progress,
                                                           analysis_readiness));
      ImGui::Render();
      bool ui_reference_timed_out = ui_reference_capture.pollTimeout();
      bool ui_reference_state_ready = false;
      if (options->ui_reference.enabled) {
        ui_semantic_snapshot =
            crimson::ui::finishSemanticFrame(ImGui::GetCurrentContext());
        auto has_window = [&](const std::string &name) {
          return std::any_of(ui_semantic_snapshot.windows.begin(),
                             ui_semantic_snapshot.windows.end(),
                             [&](const crimson::ui::SemanticWindow &candidate) {
                               return candidate.visible_name == name &&
                                      !candidate.collapsed;
                             });
        };
        const bool loaded_windows_ready =
            has_window("File Browser") && has_window("Frame Inspect") &&
            has_window("Diagnostics") && has_window("Frames in the buffer") &&
            has_window(camera_window_name) &&
            has_window("Stimulus Event Timeline") &&
            has_window("Analysis Timeline");
        switch (options->ui_reference.state) {
        case AppleUiReferenceState::Empty:
          ui_reference_state_ready =
              !video_enabled && has_window("File Browser");
          break;
        case AppleUiReferenceState::Workspace:
          ui_reference_state_ready =
              ui_reference_camera_exact && loaded_windows_ready;
          break;
        case AppleUiReferenceState::Keypoints:
          ui_reference_state_ready = ui_reference_camera_exact &&
                                     loaded_windows_ready &&
                                     ui_reference_overlay_ready;
          break;
        case AppleUiReferenceState::Overlays:
          ui_reference_state_ready = ui_reference_camera_exact &&
                                     loaded_windows_ready &&
                                     ui_reference_overlay_ready;
          break;
        case AppleUiReferenceState::Polar:
          ui_reference_state_ready = ui_reference_camera_exact &&
                                     loaded_windows_ready &&
                                     ui_reference_polar_ready;
          break;
        case AppleUiReferenceState::StimulusOverlay:
          ui_reference_state_ready = ui_reference_camera_exact &&
                                     loaded_windows_ready &&
                                     ui_reference_stimulus_overlay_ready;
          break;
        case AppleUiReferenceState::CropPreview:
          ui_reference_state_ready =
              ui_reference_camera_exact && loaded_windows_ready &&
              ui_reference_crop_exact && has_window("Advanced Crop Preview");
          break;
        case AppleUiReferenceState::AnalysisEye:
          ui_reference_state_ready =
              ui_reference_camera_exact && loaded_windows_ready &&
              ui_reference_overlay_ready && ui_reference_analysis_ready;
          break;
        case AppleUiReferenceState::StimulusDebug:
          ui_reference_state_ready =
              ui_reference_camera_exact && loaded_windows_ready &&
              ui_reference_stimulus_exact && has_window("Stimulus") &&
              has_window("Stimulus Frames in Buffer");
          break;
        case AppleUiReferenceState::AnalysisTailStimulus:
        case AppleUiReferenceState::Count:
          ui_reference_state_ready = false;
          break;
        }
        // The state predicate already includes exact-frame requirements. Empty
        // references intentionally have no camera frame to present.
        (void)ui_reference_capture.observeFrame(true,
                                                ui_reference_state_ready);
        ui_reference_timed_out =
            ui_reference_timed_out ||
            ui_reference_capture.phase() ==
                crimson::ui_reference::CapturePhase::TimedOut;
      }
      ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), command_buffer,
                                     encoder);
      [encoder endEncoding];

      const bool capture_ui_reference =
          ui_reference_capture.captureRequested();
      id<MTLBuffer> ui_reference_buffer = nil;
      size_t ui_reference_bytes_per_row = 0;
      if (capture_ui_reference) {
        ui_reference_bytes_per_row =
            (static_cast<size_t>(width) * 4 + 255u) & ~size_t(255u);
        ui_reference_buffer =
            [device newBufferWithLength:ui_reference_bytes_per_row *
                                        static_cast<size_t>(height)
                                options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> blit =
            ui_reference_buffer == nil ? nil
                                       : [command_buffer blitCommandEncoder];
        if (blit == nil) {
          std::fprintf(stderr,
                       "[AppleUiReference] Failed to create Metal readback\n");
          ui_reference_capture.fail("failed to create Metal readback");
          render_failed = true;
        } else {
          [blit copyFromTexture:drawable.texture
                           sourceSlice:0
                           sourceLevel:0
                          sourceOrigin:MTLOriginMake(0, 0, 0)
                            sourceSize:MTLSizeMake(width, height, 1)
                              toBuffer:ui_reference_buffer
                     destinationOffset:0
                destinationBytesPerRow:ui_reference_bytes_per_row
              destinationBytesPerImage:ui_reference_bytes_per_row *
                                       static_cast<size_t>(height)];
          [blit endEncoding];
        }
      }
      [command_buffer presentDrawable:drawable];
      [command_buffer commit];
      last_command_buffer = command_buffer;

      if (capture_ui_reference && !render_failed) {
        [command_buffer waitUntilCompleted];
        if (command_buffer.status != MTLCommandBufferStatusCompleted) {
          std::fprintf(stderr,
                       "[AppleUiReference] Metal capture command failed: %s\n",
                       command_buffer.error.localizedDescription.UTF8String);
          ui_reference_capture.fail("Metal capture command failed");
          render_failed = true;
        } else {
          const std::filesystem::path image_path =
              crimson::ui_reference::uiReferenceImagePath(
                  options->ui_reference.ready_file);
          std::string capture_error;
          if (!writeBgraPng(
                  image_path,
                  static_cast<const uint8_t *>(ui_reference_buffer.contents),
                  static_cast<size_t>(width), static_cast<size_t>(height),
                  ui_reference_bytes_per_row, &capture_error)) {
            std::fprintf(stderr, "[AppleUiReference] PNG capture failed: %s\n",
                         capture_error.c_str());
            ui_reference_capture.fail(capture_error);
            render_failed = true;
          } else {
            (void)ui_reference_capture.markCaptureComplete();
            int client_width = 0;
            int client_height = 0;
            glfwGetWindowSize(window, &client_width, &client_height);
            const auto buffered_frames =
                video_enabled ? video_playback.bufferedFrameNumbers()
                              : std::vector<int64_t>{};
            const char *crop_source =
                current_crop_selection.source ==
                        crimson::crop::CropSourceKind::LiveGeometry
                    ? "live-geometry"
                : current_crop_selection.source ==
                        crimson::crop::CropSourceKind::AcquisitionVideo
                    ? "acquisition-video"
                    : "none";
            nlohmann::json marker =
                crimson::ui_reference::makeMarkerEnvelope(
                    {"macos-metal",
                     options->ui_reference.state,
                     options->zarr_path,
                     options->ui_reference.target_frame,
                     viewer_stats.presented_frame,
                     ui_reference_capture.stableFrameCount(),
                     {client_width, client_height},
                     {width, height},
                     {image_path, width, height,
                      "metal_drawable_pre_present"}});
            marker.update({
                {"video", options->video_path},
                {"requested_frame", viewer_stats.requested_frame},
                {"logical_content_size",
                 {{"width", options->ui_reference.logical_width},
                  {"height", options->ui_reference.logical_height}}},
                {"framebuffer_scale",
                 {{"x", io.DisplayFramebufferScale.x},
                  {"y", io.DisplayFramebufferScale.y}}},
                {"buffers",
                 {{"camera_valid", buffered_frames.size()},
                  {"camera_capacity",
                   video_enabled ? video_playback.capacity() : 0},
                  {"stimulus_valid",
                   stimulus_enabled
                       ? stimulus_playback.bufferedFrameNumbers().size()
                       : 0},
                  {"stimulus_capacity",
                   stimulus_enabled ? options->stimulus_buffer_capacity : 0}}},
                {"viewports",
                 {{"camera", viewportJson(video_viewports.camera)},
                  {"crop_inset", viewportJson(video_viewports.crop_inset)},
                  {"crop_preview", viewportJson(video_viewports.crop_preview)},
                  {"stimulus_inset",
                   viewportJson(video_viewports.stimulus_inset)},
                  {"stimulus_debug",
                   viewportJson(video_viewports.stimulus_debug)}}},
                {"overlays",
                 {{"ready", ui_reference_overlay_ready},
                  {"counts", ui_reference_overlay_counts}}},
                {"polar",
                 polarSceneReferenceJson(
                     ui_reference_polar_scene, ui_reference_polar_viewport.x,
                     ui_reference_polar_viewport.y,
                     io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y,
                     &chaser_distance_polar_descriptor)},
                {"stimulus_camera_overlay",
                 stimulusCameraOverlaySceneReferenceJson(
                     ui_reference_stimulus_overlay_scene,
                     ui_reference_stimulus_overlay_viewport.x,
                     ui_reference_stimulus_overlay_viewport.y,
                     io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y,
                     stimulus_context_timeline_snapshot != nullptr
                         ? &stimulus_context_timeline_descriptor
                         : nullptr)},
                {"crop",
                 {{"ready", ui_reference_crop_exact},
                  {"source", crop_source},
                  {"camera_frame", current_crop_selection.camera_frame}}},
                {"stimulus",
                 {{"ready", ui_reference_stimulus_exact},
                  {"camera_frame", current_stimulus_resolution.camera_frame},
                  {"target_frame",
                   current_stimulus_resolution.stimulus_frame
                       ? *current_stimulus_resolution.stimulus_frame
                       : -1},
                  {"presented_frame",
                   current_stimulus_frame
                       ? current_stimulus_frame->decoded_frame.metadata
                             .frame_number
                       : -1}}},
                {"analysis",
                 {{"ready", ui_reference_analysis_ready},
                  {"eye_representation",
                   workspace_state.selections().eye_angle_representation_key}}},
                {"semantic_snapshot",
                 crimson::ui::semanticSnapshotJson(ui_semantic_snapshot)}});
            if (!crimson::ui_reference::writeUiReferenceMarkerAtomically(
                    options->ui_reference.ready_file, marker, &capture_error)) {
              std::fprintf(stderr,
                           "[AppleUiReference] Marker write failed: %s\n",
                           capture_error.c_str());
              ui_reference_capture.fail(capture_error);
              render_failed = true;
            } else {
              (void)ui_reference_capture.markPublished();
              std::printf(
                  "[AppleUiReference] READY state=%s target_frame=%d "
                  "presented_frame=%lld stable_frames=%d image=%s\n",
                  appleUiReferenceStateName(options->ui_reference.state),
                  options->ui_reference.target_frame,
                  static_cast<long long>(viewer_stats.presented_frame),
                  ui_reference_capture.stableFrameCount(),
                  image_path.string().c_str());
              glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
          }
        }
      }

      if (ui_reference_timed_out) {
        std::fprintf(
            stderr,
            "[AppleUiReference] Timed out state=%s target=%d requested=%lld "
            "presented=%lld stable=%d camera=%d overlay=%d crop=%d "
            "stimulus=%d analysis=%d polar=%d\n",
            appleUiReferenceStateName(options->ui_reference.state),
            options->ui_reference.target_frame,
            static_cast<long long>(viewer_stats.requested_frame),
            static_cast<long long>(viewer_stats.presented_frame),
            ui_reference_capture.stableFrameCount(), ui_reference_camera_exact,
            ui_reference_overlay_ready, ui_reference_crop_exact,
            ui_reference_stimulus_exact, ui_reference_analysis_ready,
            ui_reference_polar_ready);
        render_failed = true;
        glfwSetWindowShouldClose(window, GLFW_TRUE);
      }

      if (subject_mask_smoke_start_pending &&
          viewer_stats.presented_frame == options->video_smoke_start) {
        subject_mask_smoke_start_pending = false;
        const auto playback_started = std::chrono::steady_clock::now();
        video_clock.play(playback_started);
        video_playback.setPlaybackState(viewer_stats.presented_frame, true,
                                        video_clock.effectiveFramesPerSecond());
        video_smoke_started = playback_started;
      }

      if (options->smoke) {
        const auto command_wait_started = std::chrono::steady_clock::now();
        [command_buffer waitUntilCompleted];
        if (video_enabled) {
          viewer_stats.max_command_wait_ms = std::max(
              viewer_stats.max_command_wait_ms,
              std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - command_wait_started)
                  .count());
        }
        if (command_buffer.status != MTLCommandBufferStatusCompleted) {
          std::fprintf(stderr, "[MacShellSmoke] Metal command failed: %s\n",
                       command_buffer.error.localizedDescription.UTF8String);
          render_failed = true;
          break;
        }
      }

      if (options->multistream_smoke && multistream_composite_exact) {
        auto request_exact_frame = [&](int64_t target,
                                       MultistreamSmokeStage next_stage) {
          video_clock.pause();
          video_clock.seek(target);
          viewer_stats.requested_frame = target;
          pending_camera_discontinuity = true;
          pending_crop_discontinuity = true;
          viewer_presentation_discontinuity = true;
          std::string seek_error;
          if (!video_playback.requestSeek(target, &seek_error)) {
            multistream_smoke.error = seek_error;
            render_failed = true;
            return false;
          }
          multistream_smoke.stage = next_stage;
          return true;
        };

        if (multistream_smoke.stage ==
                MultistreamSmokeStage::WaitForPausedExact &&
            multistream_composite_frame == multistream_smoke.pause_frame) {
          ++multistream_smoke.exact_settlements;
          request_exact_frame(multistream_smoke.step_frame,
                              MultistreamSmokeStage::WaitForStepExact);
        } else if (multistream_smoke.stage ==
                       MultistreamSmokeStage::WaitForStepExact &&
                   multistream_composite_frame ==
                       multistream_smoke.step_frame) {
          ++multistream_smoke.exact_settlements;
          request_exact_frame(multistream_smoke.backward_frame,
                              MultistreamSmokeStage::WaitForBackwardSeekExact);
        } else if (multistream_smoke.stage ==
                       MultistreamSmokeStage::WaitForBackwardSeekExact &&
                   multistream_composite_frame ==
                       multistream_smoke.backward_frame) {
          ++multistream_smoke.exact_settlements;
          request_exact_frame(multistream_smoke.forward_frame,
                              MultistreamSmokeStage::WaitForForwardSeekExact);
        } else if (multistream_smoke.stage ==
                       MultistreamSmokeStage::WaitForForwardSeekExact &&
                   multistream_composite_frame ==
                       multistream_smoke.forward_frame) {
          ++multistream_smoke.exact_settlements;
          video_clock.play();
          video_playback.setPlaybackState(
              multistream_smoke.forward_frame, true,
              video_clock.effectiveFramesPerSecond());
          multistream_smoke.stage = MultistreamSmokeStage::ResumeToEnd;
        } else if (multistream_smoke.stage ==
                       MultistreamSmokeStage::WaitForEndExact &&
                   multistream_composite_frame == multistream_smoke.end_frame) {
          ++multistream_smoke.exact_settlements;
          multistream_smoke.stage = MultistreamSmokeStage::Complete;
        }
      }

      ++presented_frames;
      if (options->smoke && !options->video_smoke &&
          presented_frames >= options->smoke_frames) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
      }
      if (options->multistream_smoke &&
          multistream_smoke.stage == MultistreamSmokeStage::Complete) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
      } else if (options->video_smoke && !options->multistream_smoke &&
                 viewer_stats.presented_frame >= options->video_smoke_end &&
                 (!options->stimulus_smoke || stimulus_smoke_end_satisfied ||
                  stimulus_failed) &&
                 (!options->crop_smoke || crop_smoke_end_satisfied ||
                  crop_failed)) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
      }
      if (options->video_smoke &&
          std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                        video_smoke_started)
                  .count() > video_smoke_timeout_seconds) {
        std::fprintf(
            stderr,
            "[AppleVideoSmoke] Timed out requested=%lld presented=%lld "
            "end=%d\n",
            static_cast<long long>(viewer_stats.requested_frame),
            static_cast<long long>(viewer_stats.presented_frame),
            options->video_smoke_end);
        render_failed = true;
        glfwSetWindowShouldClose(window, GLFW_TRUE);
      }
    }
  }

  if (last_command_buffer != nil) {
    [last_command_buffer waitUntilCompleted];
    if (last_command_buffer.status != MTLCommandBufferStatusCompleted) {
      std::fprintf(stderr, "[MacShell] Final Metal command failed: %s\n",
                   last_command_buffer.error.localizedDescription.UTF8String);
      render_failed = true;
    }
  }

  session_lifecycle.beginClose();
  video_clock.seekCoordinator().cancelActive();

  const AppleVideoPlaybackBufferMetrics final_video_metrics =
      video_enabled ? video_playback.metrics()
                    : AppleVideoPlaybackBufferMetrics{};
  const size_t final_video_capacity =
      video_enabled ? video_playback.capacity() : 0;
  const AppleVideoAssetInfo final_video_info =
      video_enabled ? video_playback.info() : AppleVideoAssetInfo{};
  const AppleStimulusPlaybackMetrics final_stimulus_metrics =
      stimulus_enabled ? stimulus_playback.metrics()
                       : AppleStimulusPlaybackMetrics{};
  const crimson::playback::StimulusPresentationMetrics
      final_stimulus_presentation = stimulus_presentation.metrics();
  const AppleAcquisitionCropPlaybackMetrics final_crop_metrics =
      crop_enabled ? crop_playback.metrics()
                   : AppleAcquisitionCropPlaybackMetrics{};
  const crimson::crop::CropPresentationMetrics final_crop_presentation =
      crop_presentation.metrics();
  const int64_t final_crop_decoded_frame =
      current_crop_frame
          ? current_crop_frame->decoded_frame.metadata.frame_number
          : -1;
  const int32_t final_paired_stimulus_frame =
      current_stimulus_frame
          ? current_stimulus_frame->decoded_frame.metadata.frame_number
          : -1;
  const auto final_analysis_loading_progress = analysis_loader.progress();
  current_stimulus_frame.reset();
  current_crop_frame.reset();
  analysis_loader.close();
  quality_timeline_session.close();
  const auto final_quality_timeline_metrics =
      quality_timeline_session.metrics();
  const auto &detection_quality_timeline_descriptor =
      final_quality_timeline_metrics.detection_descriptor;
  const auto &final_detection_quality_repository_metrics =
      final_quality_timeline_metrics.detection_repository;
  const auto &final_detection_quality_buffer_metrics =
      final_quality_timeline_metrics.detection_buffer;
  const auto &keypoint_quality_timeline_descriptor =
      final_quality_timeline_metrics.keypoint_descriptor;
  const auto &final_keypoint_quality_repository_metrics =
      final_quality_timeline_metrics.keypoint_repository;
  const auto &final_keypoint_quality_buffer_metrics =
      final_quality_timeline_metrics.keypoint_buffer;
  const auto final_canonical_detection_repository_metrics =
      canonical_detection_buffer.repositoryMetrics();
  canonical_detection_buffer.close();
  const auto final_canonical_detection_buffer_metrics =
      canonical_detection_buffer.metrics();
  const auto final_canonical_detection_residency_metrics =
      canonical_detection_buffer.residencyMetrics();
  keypoint_overlay_buffer.close();
  chaser_distance_polar_buffer.close();
  const crimson::polar::ChaserDistancePolarBufferMetrics
      final_chaser_distance_polar_metrics =
          chaser_distance_polar_buffer.metrics();
  subject_mask_overlay_buffer.close();
  const SubjectMaskOverlayBufferMetrics final_subject_mask_metrics =
      subject_mask_overlay_buffer.metrics();
  const crimson::zarr::SubjectMaskOverlayRepositoryMetrics
      final_subject_mask_repository_metrics =
          subject_mask_overlay_buffer.repositoryMetrics();
  const crimson::overlay::ReadOnlyOverlayFrameMetrics
      final_subject_mask_presentation_metrics =
          subject_mask_frame_presentation.metrics();
  subject_shape_overlay_buffer.close();
  const SubjectShapeOverlayBufferMetrics final_subject_shape_metrics =
      subject_shape_overlay_buffer.metrics();
  eye_geometry_overlay_buffer.close();
  const EyeGeometryOverlayBufferMetrics final_eye_geometry_metrics =
      eye_geometry_overlay_buffer.metrics();
  motion_timeline_buffer.close();
  const AnalysisSeriesTimelineBufferMetrics final_motion_timeline_metrics =
      motion_timeline_buffer.metrics();
  swim_bout_timeline_buffer.close();
  const SwimBoutTimelineBufferMetrics final_swim_bout_timeline_metrics =
      swim_bout_timeline_buffer.metrics();
  eye_angle_timeline_buffer.close();
  const EyeAngleTimelineBufferMetrics final_eye_angle_timeline_metrics =
      eye_angle_timeline_buffer.metrics();
  tail_kinematics_timeline_buffer.close();
  const AnalysisSeriesTimelineBufferMetrics
      final_tail_kinematics_timeline_metrics =
          tail_kinematics_timeline_buffer.metrics();
  analysis_data_scheduler->waitUntilIdle();
  const crimson::data::DataAccessSchedulerMetrics
      final_analysis_data_scheduler_metrics =
          analysis_data_scheduler->metrics();
  analysis_data_scheduler->shutdown();
  if (crop_enabled) {
    crop_playback.close();
  }
  if (stimulus_enabled) {
    stimulus_playback.close();
  }
  if (video_enabled) {
    current_video_frame.reset();
    video_playback.close();
    video_renderer.reset();
    overlay_renderer.reset();
  }

  ImGui_ImplMetal_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImPlot::DestroyContext();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  session_lifecycle.completeClose();

  const auto session_relaunch = session_lifecycle.replacementRequest();
  if (session_relaunch && session_relaunch->requested) {
    std::string relaunch_error;
    if (!launchReplacementSession(*session_relaunch, argv[0],
                                  &relaunch_error)) {
      std::fprintf(stderr, "[MacSession] Relaunch failed: %s\n",
                   relaunch_error.c_str());
      render_failed = true;
    } else {
      std::printf("[MacSession] Relaunched video=%s clip_index=%s zarr=%s "
                  "stimulus=%s\n",
                  session_relaunch->video_path.c_str(),
                  session_relaunch->recording_clip_index_path.c_str(),
                  session_relaunch->zarr_path.c_str(),
                  session_relaunch->stimulus_video_path.c_str());
    }
  }
  crimson::diagnostics::writeRuntimeDiagnostics(
      std::cout, "Apple",
      {session_lifecycle.snapshot(), final_analysis_loading_progress,
       camera_presentation_tracker.metrics()});

  crimson::data::writeDataAccessSchedulerDiagnostics(
      std::cout, "Apple", final_analysis_data_scheduler_metrics);
  crimson::playback::writePlaybackSeekDiagnostics(
      std::cout, "Apple", video_clock.seekCoordinator().metrics());

  if (!canonical_detection_descriptor.run_name.empty()) {
    reportCanonicalDetectionResidency(
        "summary", canonical_detection_descriptor,
        final_canonical_detection_residency_metrics);
    std::printf(
        "[AppleCanonicalDetection] presentations=%llu detections=%llu "
        "requests=%llu cache_hits=%llu demand_pages=%llu lead_pages=%llu "
        "resolved_pages=%llu failed_pages=%llu discarded_pages=%llu "
        "evicted_pages=%llu peak_cached_pages=%zu peak_cached_bytes=%llu "
        "peak_pending=%zu range_reads=%llu resolved_frames=%llu "
        "resolved_rows=%llu field_reads=%llu peak_concurrent_fields=%zu "
        "max_resolve_ms=%.1f error=%s\n",
        static_cast<unsigned long long>(canonical_detection_presentations),
        static_cast<unsigned long long>(canonical_detection_detections),
        static_cast<unsigned long long>(
            final_canonical_detection_buffer_metrics.requests),
        static_cast<unsigned long long>(
            final_canonical_detection_buffer_metrics.cache_hits),
        static_cast<unsigned long long>(
            final_canonical_detection_buffer_metrics.demand_pages),
        static_cast<unsigned long long>(
            final_canonical_detection_buffer_metrics.lead_pages),
        static_cast<unsigned long long>(
            final_canonical_detection_buffer_metrics.resolved_pages),
        static_cast<unsigned long long>(
            final_canonical_detection_buffer_metrics.failed_pages),
        static_cast<unsigned long long>(
            final_canonical_detection_buffer_metrics.discarded_pages),
        static_cast<unsigned long long>(
            final_canonical_detection_buffer_metrics.evicted_pages),
        final_canonical_detection_buffer_metrics.peak_cached_pages,
        static_cast<unsigned long long>(
            final_canonical_detection_buffer_metrics.peak_cached_bytes),
        final_canonical_detection_buffer_metrics.peak_pending_pages,
        static_cast<unsigned long long>(
            final_canonical_detection_repository_metrics.range_reads),
        static_cast<unsigned long long>(
            final_canonical_detection_repository_metrics.resolved_frames),
        static_cast<unsigned long long>(
            final_canonical_detection_repository_metrics.resolved_rows),
        static_cast<unsigned long long>(
            final_canonical_detection_repository_metrics.ui_field_reads),
        final_canonical_detection_repository_metrics
            .peak_concurrent_ui_field_reads,
        final_canonical_detection_buffer_metrics.maximum_resolve_ms,
        final_canonical_detection_buffer_metrics.last_error.c_str());
  }

  if (!detection_quality_timeline_descriptor.run_name.empty()) {
    std::printf(
        "[AppleDetectionQualityTimeline] requests=%llu cache_hits=%llu "
        "resolved=%llu failed=%llu discarded=%llu peak_cached=%zu "
        "peak_pending=%zu range_reads=%llu source_rows=%llu "
        "instance_rows=%llu decoded_bytes=%llu peak_concurrent_fields=%zu "
        "overview_requests=%llu overview_cache_hits=%llu "
        "overview_resolved=%llu overview_failed=%llu overview_discarded=%llu "
        "overview_reads=%llu overview_source_rows=%llu "
        "overview_instance_rows=%llu overview_decoded_bytes=%llu "
        "max_resolve_ms=%.1f max_read_ms=%.1f "
        "max_overview_resolve_ms=%.1f max_overview_read_ms=%.1f error=%s\n",
        static_cast<unsigned long long>(
            final_detection_quality_buffer_metrics.requests),
        static_cast<unsigned long long>(
            final_detection_quality_buffer_metrics.cache_hits),
        static_cast<unsigned long long>(
            final_detection_quality_buffer_metrics.resolved_windows),
        static_cast<unsigned long long>(
            final_detection_quality_buffer_metrics.failed_windows),
        static_cast<unsigned long long>(
            final_detection_quality_buffer_metrics.discarded_results),
        final_detection_quality_buffer_metrics.peak_cached_windows,
        final_detection_quality_buffer_metrics.peak_pending_windows,
        static_cast<unsigned long long>(
            final_detection_quality_repository_metrics.range_reads),
        static_cast<unsigned long long>(
            final_detection_quality_repository_metrics.source_rows_read),
        static_cast<unsigned long long>(
            final_detection_quality_repository_metrics.instance_rows_read),
        static_cast<unsigned long long>(
            final_detection_quality_repository_metrics.decoded_bytes),
        final_detection_quality_repository_metrics.peak_concurrent_field_reads,
        static_cast<unsigned long long>(
            final_detection_quality_buffer_metrics.overview_requests),
        static_cast<unsigned long long>(
            final_detection_quality_buffer_metrics.overview_cache_hits),
        static_cast<unsigned long long>(
            final_detection_quality_buffer_metrics.resolved_overviews),
        static_cast<unsigned long long>(
            final_detection_quality_buffer_metrics.failed_overviews),
        static_cast<unsigned long long>(
            final_detection_quality_buffer_metrics.discarded_overviews),
        static_cast<unsigned long long>(
            final_detection_quality_repository_metrics.overview_reads),
        static_cast<unsigned long long>(
            final_detection_quality_repository_metrics
                .overview_source_rows_read),
        static_cast<unsigned long long>(
            final_detection_quality_repository_metrics
                .overview_instance_rows_read),
        static_cast<unsigned long long>(
            final_detection_quality_repository_metrics.overview_decoded_bytes),
        final_detection_quality_buffer_metrics.maximum_resolve_ms,
        final_detection_quality_repository_metrics.maximum_range_read_ms,
        final_detection_quality_buffer_metrics.maximum_overview_resolve_ms,
        final_detection_quality_repository_metrics.maximum_overview_read_ms,
        final_detection_quality_buffer_metrics.last_error.c_str());
  }

  if (!keypoint_quality_timeline_descriptor.run_name.empty()) {
    std::printf(
        "[AppleKeypointQualityTimeline] requests=%llu cache_hits=%llu "
        "resolved=%llu failed=%llu discarded=%llu peak_cached=%zu "
        "peak_pending=%zu range_reads=%llu rows=%llu decoded_bytes=%llu "
        "overview_requests=%llu overview_cache_hits=%llu "
        "overview_resolved=%llu overview_failed=%llu overview_discarded=%llu "
        "overview_reads=%llu overview_rows=%llu overview_decoded_bytes=%llu "
        "peak_concurrent_fields=%zu max_resolve_ms=%.1f max_read_ms=%.1f "
        "max_overview_resolve_ms=%.1f max_overview_read_ms=%.1f "
        "error=%s\n",
        static_cast<unsigned long long>(
            final_keypoint_quality_buffer_metrics.requests),
        static_cast<unsigned long long>(
            final_keypoint_quality_buffer_metrics.cache_hits),
        static_cast<unsigned long long>(
            final_keypoint_quality_buffer_metrics.resolved_windows),
        static_cast<unsigned long long>(
            final_keypoint_quality_buffer_metrics.failed_windows),
        static_cast<unsigned long long>(
            final_keypoint_quality_buffer_metrics.discarded_results),
        final_keypoint_quality_buffer_metrics.peak_cached_windows,
        final_keypoint_quality_buffer_metrics.peak_pending_windows,
        static_cast<unsigned long long>(
            final_keypoint_quality_repository_metrics.range_reads),
        static_cast<unsigned long long>(
            final_keypoint_quality_repository_metrics.rows_read),
        static_cast<unsigned long long>(
            final_keypoint_quality_repository_metrics.decoded_bytes),
        static_cast<unsigned long long>(
            final_keypoint_quality_buffer_metrics.overview_requests),
        static_cast<unsigned long long>(
            final_keypoint_quality_buffer_metrics.overview_cache_hits),
        static_cast<unsigned long long>(
            final_keypoint_quality_buffer_metrics.resolved_overviews),
        static_cast<unsigned long long>(
            final_keypoint_quality_buffer_metrics.failed_overviews),
        static_cast<unsigned long long>(
            final_keypoint_quality_buffer_metrics.discarded_overviews),
        static_cast<unsigned long long>(
            final_keypoint_quality_repository_metrics.overview_reads),
        static_cast<unsigned long long>(
            final_keypoint_quality_repository_metrics.overview_rows_read),
        static_cast<unsigned long long>(
            final_keypoint_quality_repository_metrics.overview_decoded_bytes),
        final_keypoint_quality_repository_metrics.peak_concurrent_field_reads,
        final_keypoint_quality_buffer_metrics.maximum_resolve_ms,
        final_keypoint_quality_repository_metrics.maximum_range_read_ms,
        final_keypoint_quality_buffer_metrics.maximum_overview_resolve_ms,
        final_keypoint_quality_repository_metrics.maximum_overview_read_ms,
        final_keypoint_quality_buffer_metrics.last_error.c_str());
  }

  if (!subject_mask_descriptor.run_name.empty()) {
    crimson::overlay::writeReadOnlyOverlayFrameDiagnostics(
        std::cout, "Apple", "subject_masks",
        final_subject_mask_presentation_metrics);
    std::printf(
        "[AppleSubjectMasks] presentations=%llu detections=%llu "
        "components=%llu requests=%llu cache_hits=%llu resolved=%llu "
        "missing=%llu failed=%llu discarded=%llu peak_cached=%zu "
        "peak_pending=%zu max_resolve_ms=%.1f demand_chunks=%llu "
        "prefetched_chunks=%llu chunk_cache_hits=%llu prefetch_requests=%llu "
        "chunk_evictions=%llu chunk_failures=%llu peak_chunks=%zu "
        "max_chunk_ms=%.1f dense_reads=%llu contour_reads=%llu "
        "contour_source_bytes=%llu source_point_count_opens=%llu "
        "source_point_count_reads=%llu error=%s\n",
        static_cast<unsigned long long>(subject_mask_overlay_presentations),
        static_cast<unsigned long long>(subject_mask_overlay_detections),
        static_cast<unsigned long long>(subject_mask_overlay_components),
        static_cast<unsigned long long>(final_subject_mask_metrics.requests),
        static_cast<unsigned long long>(final_subject_mask_metrics.cache_hits),
        static_cast<unsigned long long>(
            final_subject_mask_metrics.resolved_frames),
        static_cast<unsigned long long>(
            final_subject_mask_metrics.missing_frames),
        static_cast<unsigned long long>(
            final_subject_mask_metrics.failed_frames),
        static_cast<unsigned long long>(
            final_subject_mask_metrics.discarded_results),
        final_subject_mask_metrics.peak_cached_frames,
        final_subject_mask_metrics.peak_pending_frames,
        final_subject_mask_metrics.maximum_resolve_ms,
        static_cast<unsigned long long>(
            final_subject_mask_repository_metrics.demand_chunk_loads),
        static_cast<unsigned long long>(
            final_subject_mask_repository_metrics.prefetched_chunk_loads),
        static_cast<unsigned long long>(
            final_subject_mask_repository_metrics.chunk_cache_hits),
        static_cast<unsigned long long>(
            final_subject_mask_repository_metrics.prefetch_requests),
        static_cast<unsigned long long>(
            final_subject_mask_repository_metrics.chunk_evictions),
        static_cast<unsigned long long>(
            final_subject_mask_repository_metrics.chunk_load_failures),
        final_subject_mask_repository_metrics.peak_cached_chunks,
        final_subject_mask_repository_metrics.maximum_chunk_load_ms,
        static_cast<unsigned long long>(
            final_subject_mask_repository_metrics.dense_mask_payload_reads),
        static_cast<unsigned long long>(
            final_subject_mask_repository_metrics.contour_payload_reads),
        static_cast<unsigned long long>(
            final_subject_mask_repository_metrics.contour_source_bytes_read),
        static_cast<unsigned long long>(final_subject_mask_repository_metrics
                                            .source_point_count_open_attempts),
        static_cast<unsigned long long>(final_subject_mask_repository_metrics
                                            .source_point_count_payload_reads),
        final_subject_mask_metrics.last_error.c_str());
    std::printf(
        "[AppleDataAccess] source=subject_masks phase=summary "
        "open_ms=%.1f lazy_mapping=%d catalog_ms=%.1f mapping_ms=%.1f "
        "storage_ms=%.1f "
        "contour_open_ms=%.1f index_ms=%.1f metadata_decoded_bytes=%llu "
        "metadata_retained_bytes=%llu frame_index_ms=%.1f "
        "frame_index_rows=%llu frame_index_source_bytes=%llu "
        "frame_index_retained_bytes=%llu fallback_index_builds=%llu "
        "fallback_index_rows=%llu mapping_page_reads=%llu "
        "mapping_page_hits=%llu mapping_page_evictions=%llu "
        "mapping_page_source_bytes=%llu cached_mapping_bytes=%llu "
        "peak_mapping_bytes=%llu mapping_initialize_failures=%llu "
        "source_bytes=%llu "
        "retained_chunk_bytes=%llu cached_chunk_bytes=%llu "
        "peak_chunk_bytes=%llu evicted_chunk_bytes=%llu "
        "chunk_read_ms=%.1f chunk_convert_ms=%.1f contour_load_ms=%.1f "
        "max_read_ms=%.1f max_convert_ms=%.1f max_contour_ms=%.1f "
        "cached_frame_bytes=%llu peak_frame_bytes=%llu "
        "released_frame_bytes=%llu\n",
        final_subject_mask_repository_metrics.open_total_ms,
        final_subject_mask_repository_metrics.lazy_mapping ? 1 : 0,
        final_subject_mask_repository_metrics.catalog_ms,
        final_subject_mask_repository_metrics.mapping_read_ms,
        final_subject_mask_repository_metrics.storage_open_ms,
        final_subject_mask_repository_metrics.contour_open_ms,
        final_subject_mask_repository_metrics.metadata_index_ms,
        static_cast<unsigned long long>(
            final_subject_mask_repository_metrics.metadata_decoded_bytes),
        static_cast<unsigned long long>(
            final_subject_mask_repository_metrics.metadata_retained_bytes),
        final_subject_mask_repository_metrics.frame_index_initialize_ms,
        static_cast<unsigned long long>(
            final_subject_mask_repository_metrics.frame_index_rows_read),
        static_cast<unsigned long long>(
            final_subject_mask_repository_metrics.frame_index_source_bytes),
        static_cast<unsigned long long>(
            final_subject_mask_repository_metrics.frame_index_retained_bytes),
        static_cast<unsigned long long>(
            final_subject_mask_repository_metrics.fallback_frame_index_builds),
        static_cast<unsigned long long>(
            final_subject_mask_repository_metrics.fallback_frame_index_rows),
        static_cast<unsigned long long>(
            final_subject_mask_repository_metrics.mapping_page_reads),
        static_cast<unsigned long long>(
            final_subject_mask_repository_metrics.mapping_page_cache_hits),
        static_cast<unsigned long long>(
            final_subject_mask_repository_metrics.mapping_page_evictions),
        static_cast<unsigned long long>(
            final_subject_mask_repository_metrics.mapping_page_source_bytes),
        static_cast<unsigned long long>(
            final_subject_mask_repository_metrics.cached_mapping_bytes),
        static_cast<unsigned long long>(
            final_subject_mask_repository_metrics.peak_cached_mapping_bytes),
        static_cast<unsigned long long>(
            final_subject_mask_repository_metrics.mapping_initialize_failures),
        static_cast<unsigned long long>(
            final_subject_mask_repository_metrics.chunk_source_bytes_read),
        static_cast<unsigned long long>(final_subject_mask_repository_metrics
                                            .chunk_retained_bytes_produced),
        static_cast<unsigned long long>(
            final_subject_mask_repository_metrics.cached_payload_bytes),
        static_cast<unsigned long long>(
            final_subject_mask_repository_metrics.peak_cached_payload_bytes),
        static_cast<unsigned long long>(
            final_subject_mask_repository_metrics.evicted_payload_bytes),
        final_subject_mask_repository_metrics.chunk_read_ms,
        final_subject_mask_repository_metrics.chunk_convert_ms,
        final_subject_mask_repository_metrics.contour_load_ms,
        final_subject_mask_repository_metrics.maximum_chunk_read_ms,
        final_subject_mask_repository_metrics.maximum_chunk_convert_ms,
        final_subject_mask_repository_metrics.maximum_contour_load_ms,
        static_cast<unsigned long long>(
            final_subject_mask_metrics.cached_payload_bytes),
        static_cast<unsigned long long>(
            final_subject_mask_metrics.peak_cached_payload_bytes),
        static_cast<unsigned long long>(
            final_subject_mask_metrics.released_payload_bytes));
  }
  if (!chaser_distance_polar_descriptor.provenance.run_name.empty()) {
    std::printf(
        "[AppleChaserPolar] presentations=%llu points=%llu requests=%llu "
        "cache_hits=%llu ready=%llu empty=%llu missing=%llu failed=%llu "
        "discarded=%llu source_points=%llu published_points=%llu "
        "peak_cached=%zu peak_pending=%zu max_resolve_ms=%.1f "
        "runtime_failed=%d error=%s\n",
        static_cast<unsigned long long>(chaser_distance_polar_presentations),
        static_cast<unsigned long long>(chaser_distance_polar_points),
        static_cast<unsigned long long>(
            final_chaser_distance_polar_metrics.requests),
        static_cast<unsigned long long>(
            final_chaser_distance_polar_metrics.cache_hits),
        static_cast<unsigned long long>(
            final_chaser_distance_polar_metrics.ready_frames),
        static_cast<unsigned long long>(
            final_chaser_distance_polar_metrics.empty_frames),
        static_cast<unsigned long long>(
            final_chaser_distance_polar_metrics.missing_frames),
        static_cast<unsigned long long>(
            final_chaser_distance_polar_metrics.failed_frames),
        static_cast<unsigned long long>(
            final_chaser_distance_polar_metrics.discarded_results),
        static_cast<unsigned long long>(
            final_chaser_distance_polar_metrics.source_points),
        static_cast<unsigned long long>(
            final_chaser_distance_polar_metrics.published_points),
        final_chaser_distance_polar_metrics.peak_cached_frames,
        final_chaser_distance_polar_metrics.peak_pending_frames,
        final_chaser_distance_polar_metrics.maximum_resolve_ms,
        chaser_distance_polar_failed ? 1 : 0,
        final_chaser_distance_polar_metrics.last_error.c_str());
  }
  if (!subject_shape_descriptor.run_name.empty()) {
    std::printf(
        "[AppleSubjectShape] presentations=%llu detections=%llu requests=%llu "
        "cache_hits=%llu resolved=%llu missing=%llu failed=%llu discarded=%llu "
        "peak_cached=%zu peak_pending=%zu max_resolve_ms=%.1f error=%s\n",
        static_cast<unsigned long long>(subject_shape_overlay_presentations),
        static_cast<unsigned long long>(subject_shape_overlay_detections),
        static_cast<unsigned long long>(final_subject_shape_metrics.requests),
        static_cast<unsigned long long>(final_subject_shape_metrics.cache_hits),
        static_cast<unsigned long long>(
            final_subject_shape_metrics.resolved_frames),
        static_cast<unsigned long long>(
            final_subject_shape_metrics.missing_frames),
        static_cast<unsigned long long>(
            final_subject_shape_metrics.failed_frames),
        static_cast<unsigned long long>(
            final_subject_shape_metrics.discarded_results),
        final_subject_shape_metrics.peak_cached_frames,
        final_subject_shape_metrics.peak_pending_frames,
        final_subject_shape_metrics.maximum_resolve_ms,
        final_subject_shape_metrics.last_error.c_str());
  }
  if (!eye_geometry_descriptor.run_name.empty()) {
    std::printf(
        "[AppleEyeGeometry] presentations=%llu detections=%llu labels=%llu "
        "requests=%llu cache_hits=%llu resolved=%llu missing=%llu failed=%llu "
        "discarded=%llu peak_cached=%zu peak_pending=%zu max_resolve_ms=%.1f "
        "error=%s\n",
        static_cast<unsigned long long>(eye_geometry_overlay_presentations),
        static_cast<unsigned long long>(eye_geometry_overlay_detections),
        static_cast<unsigned long long>(eye_geometry_overlay_labels),
        static_cast<unsigned long long>(final_eye_geometry_metrics.requests),
        static_cast<unsigned long long>(final_eye_geometry_metrics.cache_hits),
        static_cast<unsigned long long>(
            final_eye_geometry_metrics.resolved_frames),
        static_cast<unsigned long long>(
            final_eye_geometry_metrics.missing_frames),
        static_cast<unsigned long long>(
            final_eye_geometry_metrics.failed_frames),
        static_cast<unsigned long long>(
            final_eye_geometry_metrics.discarded_results),
        final_eye_geometry_metrics.peak_cached_frames,
        final_eye_geometry_metrics.peak_pending_frames,
        final_eye_geometry_metrics.maximum_resolve_ms,
        final_eye_geometry_metrics.last_error.c_str());
  }
  if (!motion_timeline_descriptor.sources.empty()) {
    std::printf(
        "[AppleMotionTimeline] presentations=%llu requests=%llu "
        "cache_hits=%llu resolved=%llu missing=%llu failed=%llu "
        "discarded=%llu rows_read=%llu points=%llu peak_cached=%zu "
        "peak_pending=%zu index_reads=%llu index_hits=%llu "
        "index_evictions=%llu index_source_bytes=%llu "
        "index_cached_bytes=%llu index_peak_bytes=%llu "
        "max_index_read_ms=%.1f max_resolve_ms=%.1f error=%s\n",
        static_cast<unsigned long long>(motion_timeline_presentations),
        static_cast<unsigned long long>(final_motion_timeline_metrics.requests),
        static_cast<unsigned long long>(
            final_motion_timeline_metrics.cache_hits),
        static_cast<unsigned long long>(
            final_motion_timeline_metrics.resolved_windows),
        static_cast<unsigned long long>(
            final_motion_timeline_metrics.missing_windows),
        static_cast<unsigned long long>(
            final_motion_timeline_metrics.failed_windows),
        static_cast<unsigned long long>(
            final_motion_timeline_metrics.discarded_results),
        static_cast<unsigned long long>(
            final_motion_timeline_metrics.source_rows_read),
        static_cast<unsigned long long>(
            final_motion_timeline_metrics.published_points),
        final_motion_timeline_metrics.peak_cached_windows,
        final_motion_timeline_metrics.peak_pending_windows,
        static_cast<unsigned long long>(
            final_motion_timeline_metrics.frame_index_block_reads),
        static_cast<unsigned long long>(
            final_motion_timeline_metrics.frame_index_cache_hits),
        static_cast<unsigned long long>(
            final_motion_timeline_metrics.frame_index_cache_evictions),
        static_cast<unsigned long long>(
            final_motion_timeline_metrics.frame_index_source_bytes),
        static_cast<unsigned long long>(
            final_motion_timeline_metrics.cached_frame_index_bytes),
        static_cast<unsigned long long>(
            final_motion_timeline_metrics.peak_cached_frame_index_bytes),
        final_motion_timeline_metrics.maximum_frame_index_read_ms,
        final_motion_timeline_metrics.maximum_resolve_ms,
        final_motion_timeline_metrics.last_error.c_str());
  }
  if (!swim_bout_timeline_descriptor.candidates.empty()) {
    std::printf(
        "[AppleSwimBoutTimeline] presentations=%llu requests=%llu "
        "cache_hits=%llu resolved=%llu missing=%llu failed=%llu "
        "discarded=%llu intervals_scanned=%llu detector_rows=%llu "
        "intervals=%llu detector_points=%llu peak_cached=%zu "
        "peak_pending=%zu max_resolve_ms=%.1f error=%s\n",
        static_cast<unsigned long long>(swim_bout_timeline_presentations),
        static_cast<unsigned long long>(
            final_swim_bout_timeline_metrics.requests),
        static_cast<unsigned long long>(
            final_swim_bout_timeline_metrics.cache_hits),
        static_cast<unsigned long long>(
            final_swim_bout_timeline_metrics.resolved_windows),
        static_cast<unsigned long long>(
            final_swim_bout_timeline_metrics.missing_windows),
        static_cast<unsigned long long>(
            final_swim_bout_timeline_metrics.failed_windows),
        static_cast<unsigned long long>(
            final_swim_bout_timeline_metrics.discarded_results),
        static_cast<unsigned long long>(
            final_swim_bout_timeline_metrics.candidate_intervals_scanned),
        static_cast<unsigned long long>(
            final_swim_bout_timeline_metrics.source_detector_rows_read),
        static_cast<unsigned long long>(
            final_swim_bout_timeline_metrics.published_intervals),
        static_cast<unsigned long long>(
            final_swim_bout_timeline_metrics.published_detector_points),
        final_swim_bout_timeline_metrics.peak_cached_windows,
        final_swim_bout_timeline_metrics.peak_pending_windows,
        final_swim_bout_timeline_metrics.maximum_resolve_ms,
        final_swim_bout_timeline_metrics.last_error.c_str());
  }
  if (!eye_angle_timeline_descriptor.run_name.empty()) {
    std::printf(
        "[AppleEyeAngleTimeline] presentations=%llu requests=%llu "
        "cache_hits=%llu resolved=%llu missing=%llu failed=%llu "
        "discarded=%llu rows_read=%llu points=%llu peak_cached=%zu "
        "peak_pending=%zu max_resolve_ms=%.1f error=%s\n",
        static_cast<unsigned long long>(eye_angle_timeline_presentations),
        static_cast<unsigned long long>(
            final_eye_angle_timeline_metrics.requests),
        static_cast<unsigned long long>(
            final_eye_angle_timeline_metrics.cache_hits),
        static_cast<unsigned long long>(
            final_eye_angle_timeline_metrics.resolved_windows),
        static_cast<unsigned long long>(
            final_eye_angle_timeline_metrics.missing_windows),
        static_cast<unsigned long long>(
            final_eye_angle_timeline_metrics.failed_windows),
        static_cast<unsigned long long>(
            final_eye_angle_timeline_metrics.discarded_results),
        static_cast<unsigned long long>(
            final_eye_angle_timeline_metrics.source_rows_read),
        static_cast<unsigned long long>(
            final_eye_angle_timeline_metrics.published_points),
        final_eye_angle_timeline_metrics.peak_cached_windows,
        final_eye_angle_timeline_metrics.peak_pending_windows,
        final_eye_angle_timeline_metrics.maximum_resolve_ms,
        final_eye_angle_timeline_metrics.last_error.c_str());
  }
  if (!tail_kinematics_timeline_descriptor.sources.empty()) {
    std::printf(
        "[AppleTailKinematicsTimeline] presentations=%llu requests=%llu "
        "cache_hits=%llu resolved=%llu missing=%llu failed=%llu "
        "discarded=%llu rows_read=%llu points=%llu peak_cached=%zu "
        "peak_pending=%zu index_reads=%llu index_hits=%llu "
        "index_evictions=%llu index_source_bytes=%llu "
        "index_cached_bytes=%llu index_peak_bytes=%llu "
        "max_index_read_ms=%.1f max_resolve_ms=%.1f error=%s\n",
        static_cast<unsigned long long>(tail_kinematics_timeline_presentations),
        static_cast<unsigned long long>(
            final_tail_kinematics_timeline_metrics.requests),
        static_cast<unsigned long long>(
            final_tail_kinematics_timeline_metrics.cache_hits),
        static_cast<unsigned long long>(
            final_tail_kinematics_timeline_metrics.resolved_windows),
        static_cast<unsigned long long>(
            final_tail_kinematics_timeline_metrics.missing_windows),
        static_cast<unsigned long long>(
            final_tail_kinematics_timeline_metrics.failed_windows),
        static_cast<unsigned long long>(
            final_tail_kinematics_timeline_metrics.discarded_results),
        static_cast<unsigned long long>(
            final_tail_kinematics_timeline_metrics.source_rows_read),
        static_cast<unsigned long long>(
            final_tail_kinematics_timeline_metrics.published_points),
        final_tail_kinematics_timeline_metrics.peak_cached_windows,
        final_tail_kinematics_timeline_metrics.peak_pending_windows,
        static_cast<unsigned long long>(
            final_tail_kinematics_timeline_metrics.frame_index_block_reads),
        static_cast<unsigned long long>(
            final_tail_kinematics_timeline_metrics.frame_index_cache_hits),
        static_cast<unsigned long long>(
            final_tail_kinematics_timeline_metrics.frame_index_cache_evictions),
        static_cast<unsigned long long>(
            final_tail_kinematics_timeline_metrics.frame_index_source_bytes),
        static_cast<unsigned long long>(
            final_tail_kinematics_timeline_metrics.cached_frame_index_bytes),
        static_cast<unsigned long long>(final_tail_kinematics_timeline_metrics
                                            .peak_cached_frame_index_bytes),
        final_tail_kinematics_timeline_metrics.maximum_frame_index_read_ms,
        final_tail_kinematics_timeline_metrics.maximum_resolve_ms,
        final_tail_kinematics_timeline_metrics.last_error.c_str());
  }
  if (!stimulus_context_timeline_descriptor.run_name.empty()) {
    std::printf("[AppleStimulusContextTimeline] presentations=%llu events=%zu "
                "steps=%zu event_types=%zu camera_frames=%zu error=%s\n",
                static_cast<unsigned long long>(
                    stimulus_context_timeline_presentations),
                stimulus_context_timeline_descriptor.event_count,
                stimulus_context_timeline_descriptor.step_count,
                stimulus_context_timeline_descriptor.event_types.size(),
                stimulus_context_timeline_descriptor.frame_count,
                stimulus_context_timeline_error.c_str());
  }

  if (options->video_smoke) {
    const double elapsed_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      video_smoke_started)
            .count();
    const double maximum_accepted_lag_frames =
        final_video_info.nominal_frame_rate * 5.0;
    const bool subject_mask_validation_failed =
        !subject_mask_descriptor.run_name.empty() &&
        (subject_mask_overlay_failed ||
         final_subject_mask_metrics.failed_frames != 0 ||
         (final_subject_mask_metrics.resolved_frames != 0 &&
          subject_mask_overlay_presentations == 0));
    const bool subject_shape_validation_failed =
        !subject_shape_descriptor.run_name.empty() &&
        (subject_shape_overlay_failed ||
         final_subject_shape_metrics.failed_frames != 0 ||
         (final_subject_shape_metrics.resolved_frames != 0 &&
          subject_shape_overlay_presentations == 0));
    const bool eye_geometry_validation_failed =
        !eye_geometry_descriptor.run_name.empty() &&
        (eye_geometry_overlay_failed ||
         final_eye_geometry_metrics.failed_frames != 0 ||
         (final_eye_geometry_metrics.resolved_frames != 0 &&
          eye_geometry_overlay_presentations == 0));
    const bool motion_timeline_validation_failed =
        (options->require_motion_timeline &&
         motion_timeline_descriptor.sources.empty()) ||
        (!motion_timeline_descriptor.sources.empty() &&
         (motion_timeline_failed ||
          final_motion_timeline_metrics.failed_windows != 0 ||
          final_motion_timeline_metrics.resolved_windows == 0 ||
          motion_timeline_presentations == 0));
    const bool swim_bout_timeline_validation_failed =
        (options->require_swim_bout_timeline &&
         swim_bout_timeline_descriptor.candidates.empty()) ||
        (!swim_bout_timeline_descriptor.candidates.empty() &&
         (swim_bout_timeline_failed ||
          final_swim_bout_timeline_metrics.failed_windows != 0 ||
          final_swim_bout_timeline_metrics.resolved_windows == 0 ||
          swim_bout_timeline_presentations == 0));
    const bool eye_angle_timeline_validation_failed =
        (options->require_eye_angle_timeline &&
         eye_angle_timeline_descriptor.run_name.empty()) ||
        (!eye_angle_timeline_descriptor.run_name.empty() &&
         (eye_angle_timeline_failed ||
          final_eye_angle_timeline_metrics.failed_windows != 0 ||
          final_eye_angle_timeline_metrics.resolved_windows == 0 ||
          eye_angle_timeline_presentations == 0));
    const bool tail_kinematics_timeline_validation_failed =
        (options->require_tail_kinematics_timeline &&
         tail_kinematics_timeline_descriptor.sources.empty()) ||
        (!tail_kinematics_timeline_descriptor.sources.empty() &&
         (tail_kinematics_timeline_failed ||
          final_tail_kinematics_timeline_metrics.failed_windows != 0 ||
          final_tail_kinematics_timeline_metrics.resolved_windows == 0 ||
          tail_kinematics_timeline_presentations == 0));
    const bool stimulus_context_timeline_validation_failed =
        (options->require_stimulus_context_timeline &&
         stimulus_context_timeline_descriptor.run_name.empty()) ||
        (!stimulus_context_timeline_descriptor.run_name.empty() &&
         (stimulus_context_timeline_failed ||
          stimulus_context_timeline_presentations == 0));
    const std::string &smoke_error =
        stimulus_context_timeline_validation_failed
            ? stimulus_context_timeline_error
        : tail_kinematics_timeline_validation_failed
            ? (!tail_kinematics_timeline_error.empty()
                   ? tail_kinematics_timeline_error
                   : final_tail_kinematics_timeline_metrics.last_error)
        : eye_angle_timeline_validation_failed
            ? (!eye_angle_timeline_error.empty()
                   ? eye_angle_timeline_error
                   : final_eye_angle_timeline_metrics.last_error)
        : motion_timeline_validation_failed
            ? (!motion_timeline_error.empty()
                   ? motion_timeline_error
                   : final_motion_timeline_metrics.last_error)
        : swim_bout_timeline_validation_failed
            ? (!swim_bout_timeline_error.empty()
                   ? swim_bout_timeline_error
                   : final_swim_bout_timeline_metrics.last_error)
        : eye_geometry_validation_failed
            ? (!eye_geometry_error.empty()
                   ? eye_geometry_error
                   : final_eye_geometry_metrics.last_error)
        : subject_shape_validation_failed
            ? (!subject_shape_error.empty()
                   ? subject_shape_error
                   : final_subject_shape_metrics.last_error)
        : subject_mask_validation_failed
            ? (!subject_mask_error.empty()
                   ? subject_mask_error
                   : final_subject_mask_metrics.last_error)
            : final_video_metrics.last_error;
    if (render_failed || subject_mask_validation_failed ||
        subject_shape_validation_failed || eye_geometry_validation_failed ||
        motion_timeline_validation_failed ||
        swim_bout_timeline_validation_failed ||
        eye_angle_timeline_validation_failed ||
        tail_kinematics_timeline_validation_failed ||
        stimulus_context_timeline_validation_failed ||
        viewer_stats.presented_frame < options->video_smoke_end ||
        std::fabs(viewer_stats.pts_error_frames) > 0.51 ||
        viewer_stats.max_lag_frames > maximum_accepted_lag_frames) {
      std::fprintf(
          stderr,
          "[AppleVideoSmoke] FAIL start=%d end=%d requested=%lld "
          "presented=%lld decoded=%llu "
          "buffered_peak=%zu clip_switches=%llu active_clip=%lld "
          "max_lag_frames=%.1f lag_limit_frames=%.1f "
          "elapsed_s=%.3f "
          "memory_mib=%.1f peak_memory_mib=%.1f thermal=%s "
          "subject_mask_presentations=%llu subject_mask_resolved=%llu "
          "subject_mask_failed=%llu error=%s\n",
          options->video_smoke_start, options->video_smoke_end,
          static_cast<long long>(viewer_stats.requested_frame),
          static_cast<long long>(viewer_stats.presented_frame),
          static_cast<unsigned long long>(final_video_metrics.decoded_frames),
          final_video_metrics.peak_buffered_frames,
          static_cast<unsigned long long>(final_video_metrics.clip_switches),
          static_cast<long long>(final_video_metrics.active_clip_index),
          viewer_stats.max_lag_frames, maximum_accepted_lag_frames,
          elapsed_seconds, viewer_stats.process_memory_mib,
          viewer_stats.peak_process_memory_mib,
          appleViewerThermalStateName(viewer_stats.thermal_state),
          static_cast<unsigned long long>(subject_mask_overlay_presentations),
          static_cast<unsigned long long>(
              final_subject_mask_metrics.resolved_frames),
          static_cast<unsigned long long>(
              final_subject_mask_metrics.failed_frames),
          smoke_error.c_str());
      return 8;
    }
    std::printf(
        "[AppleVideoSmoke] PASS start=%d end=%d requested=%lld "
        "presented=%lld decoded=%llu "
        "peak_buffer=%zu repeats=%llu "
        "clip_switches=%llu active_clip=%lld "
        "skipped_source_frames=%llu late_presentations=%llu "
        "max_lag_frames=%.1f "
        "catchup_discarded_frames=%llu catchup_seeks=%llu "
        "pts_error_frames=%+.3f startup_ms=%.1f "
        "seek_ms=%.1f "
        "next_drawable_max_ms=%.1f "
        "command_wait_max_ms=%.1f elapsed_s=%.3f "
        "memory_mib=%.1f peak_memory_mib=%.1f thermal=%s "
        "subject_mask_presentations=%llu subject_mask_resolved=%llu\n",
        options->video_smoke_start, options->video_smoke_end,
        static_cast<long long>(viewer_stats.requested_frame),
        static_cast<long long>(viewer_stats.presented_frame),
        static_cast<unsigned long long>(final_video_metrics.decoded_frames),
        final_video_metrics.peak_buffered_frames,
        static_cast<unsigned long long>(viewer_stats.repeated_presentations),
        static_cast<unsigned long long>(final_video_metrics.clip_switches),
        static_cast<long long>(final_video_metrics.active_clip_index),
        static_cast<unsigned long long>(viewer_stats.skipped_source_frames),
        static_cast<unsigned long long>(viewer_stats.late_presentations),
        viewer_stats.max_lag_frames,
        static_cast<unsigned long long>(
            final_video_metrics.catchup_discarded_frames),
        static_cast<unsigned long long>(final_video_metrics.catchup_seeks),
        viewer_stats.pts_error_frames, final_video_metrics.startup_ms,
        final_video_metrics.last_seek_ms, viewer_stats.max_next_drawable_ms,
        viewer_stats.max_command_wait_ms, elapsed_seconds,
        viewer_stats.process_memory_mib, viewer_stats.peak_process_memory_mib,
        appleViewerThermalStateName(viewer_stats.thermal_state),
        static_cast<unsigned long long>(subject_mask_overlay_presentations),
        static_cast<unsigned long long>(
            final_subject_mask_metrics.resolved_frames));
    if (options->stimulus_smoke) {
      const bool stimulus_smoke_failed =
          stimulus_failed || !stimulus_smoke_end_satisfied ||
          final_stimulus_presentation.mapped_presentations == 0 ||
          final_stimulus_presentation.last_target_stimulus_frame < 0 ||
          final_paired_stimulus_frame !=
              final_stimulus_presentation.last_target_stimulus_frame ||
          final_stimulus_presentation.presented_stimulus_frame !=
              final_stimulus_presentation.last_target_stimulus_frame ||
          final_stimulus_presentation.mismatched_mapping_frames != 0 ||
          final_stimulus_presentation.mismatched_decoded_frames != 0 ||
          final_stimulus_presentation.camera_skew_frames != 0 ||
          final_stimulus_presentation.max_abs_camera_skew_frames != 0 ||
          final_stimulus_metrics.failed_requests != 0;
      const std::string &reported_stimulus_error =
          stimulus_error.empty() ? final_stimulus_metrics.decoder.last_error
                                 : stimulus_error;
      if (stimulus_smoke_failed) {
        std::fprintf(
            stderr,
            "[AppleStimulusSmoke] FAIL start=%d end=%d camera=%d "
            "target=%d decoded=%d decoder_head=%lld presented=%d "
            "generation=%llu "
            "exact=%llu holds=%llu deferred=%llu "
            "max_deferred_run=%llu mapping_mismatches=%llu "
            "decoded_mismatches=%llu camera_skew=%+lld "
            "max_abs_camera_skew=%llu failed_requests=%llu error=%s\n",
            options->video_smoke_start, options->video_smoke_end,
            final_stimulus_presentation.last_camera_frame,
            final_stimulus_presentation.last_target_stimulus_frame,
            final_paired_stimulus_frame,
            static_cast<long long>(
                final_stimulus_metrics.decoder.last_decoded_frame),
            final_stimulus_presentation.presented_stimulus_frame,
            static_cast<unsigned long long>(
                final_stimulus_presentation.last_rendered_generation),
            static_cast<unsigned long long>(
                final_stimulus_presentation.exact_presentations),
            static_cast<unsigned long long>(
                final_stimulus_presentation.held_presentations),
            static_cast<unsigned long long>(
                final_stimulus_presentation.unavailable_presentations),
            static_cast<unsigned long long>(
                final_stimulus_presentation.max_consecutive_unavailable),
            static_cast<unsigned long long>(
                final_stimulus_presentation.mismatched_mapping_frames),
            static_cast<unsigned long long>(
                final_stimulus_presentation.mismatched_decoded_frames),
            static_cast<long long>(
                final_stimulus_presentation.camera_skew_frames),
            static_cast<unsigned long long>(
                final_stimulus_presentation.max_abs_camera_skew_frames),
            static_cast<unsigned long long>(
                final_stimulus_metrics.failed_requests),
            reported_stimulus_error.c_str());
        return 8;
      }
      std::printf(
          "[AppleStimulusSmoke] PASS start=%d end=%d camera=%d target=%d "
          "decoded=%d decoder_head=%lld presented=%d generation=%llu "
          "paired=%llu holds=%llu deferred=%llu interpolated=%llu "
          "max_deferred_run=%llu seeks=%llu follows=%llu "
          "decoder_peak=%zu camera_skew=%+lld max_abs_camera_skew=%llu\n",
          options->video_smoke_start, options->video_smoke_end,
          final_stimulus_presentation.last_camera_frame,
          final_stimulus_presentation.last_target_stimulus_frame,
          final_paired_stimulus_frame,
          static_cast<long long>(
              final_stimulus_metrics.decoder.last_decoded_frame),
          final_stimulus_presentation.presented_stimulus_frame,
          static_cast<unsigned long long>(
              final_stimulus_presentation.last_rendered_generation),
          static_cast<unsigned long long>(
              final_stimulus_presentation.exact_presentations +
              final_stimulus_presentation.held_presentations),
          static_cast<unsigned long long>(
              final_stimulus_presentation.held_presentations),
          static_cast<unsigned long long>(
              final_stimulus_presentation.unavailable_presentations),
          static_cast<unsigned long long>(
              final_stimulus_presentation.interpolated_presentations),
          static_cast<unsigned long long>(
              final_stimulus_presentation.max_consecutive_unavailable),
          static_cast<unsigned long long>(final_stimulus_metrics.seek_requests),
          static_cast<unsigned long long>(
              final_stimulus_metrics.follow_requests),
          final_stimulus_metrics.decoder.peak_buffered_frames,
          static_cast<long long>(
              final_stimulus_presentation.camera_skew_frames),
          static_cast<unsigned long long>(
              final_stimulus_presentation.max_abs_camera_skew_frames));
    }
    if (options->crop_smoke) {
      const auto expected_source =
          options->crop_preference ==
                  crimson::crop::CropSourcePreference::PreferLiveGeometry
              ? crimson::crop::CropSourceKind::LiveGeometry
              : crimson::crop::CropSourceKind::AcquisitionVideo;
      const bool source_matches =
          final_crop_presentation.presented_source == expected_source;
      const bool crop_smoke_failed =
          crop_failed || !crop_smoke_end_satisfied || !source_matches ||
          read_only_overlay_presentations == 0 ||
          (keypoint_overlay_available && keypoint_overlay_presentations == 0) ||
          final_crop_presentation.presented_crop_camera_frame !=
              options->video_smoke_end ||
          final_crop_presentation.camera_skew_frames != 0 ||
          final_crop_presentation.max_abs_camera_skew_frames != 0 ||
          final_crop_presentation.mismatched_selection_frames != 0 ||
          final_crop_presentation.mismatched_surface_frames != 0 ||
          final_crop_presentation.invalid_presentations != 0 ||
          final_crop_metrics.failed_requests != 0;
      const char *source_name =
          expected_source == crimson::crop::CropSourceKind::LiveGeometry
              ? "geometry"
              : "acquisition";
      const std::string &reported_crop_error =
          crop_error.empty() ? final_crop_metrics.decoder.last_error
                             : crop_error;
      if (crop_smoke_failed) {
        std::fprintf(
            stderr,
            "[AppleCropSmoke] FAIL start=%d end=%d source=%s camera=%lld "
            "decoded=%lld source_frame=%lld generation=%llu exact=%llu "
            "holds=%llu deferred=%llu max_deferred_run=%llu "
            "selection_mismatches=%llu surface_mismatches=%llu "
            "camera_skew=%+lld max_abs_camera_skew=%llu "
            "overlay_presentations=%llu keypoint_presentations=%llu "
            "keypoint_detections=%llu failed_requests=%llu error=%s\n",
            options->video_smoke_start, options->video_smoke_end, source_name,
            static_cast<long long>(
                final_crop_presentation.presented_crop_camera_frame),
            static_cast<long long>(final_crop_decoded_frame),
            static_cast<long long>(
                final_crop_presentation.presented_source_frame),
            static_cast<unsigned long long>(
                final_crop_presentation.last_rendered_generation),
            static_cast<unsigned long long>(
                final_crop_presentation.exact_presentations),
            static_cast<unsigned long long>(
                final_crop_presentation.held_presentations),
            static_cast<unsigned long long>(
                final_crop_presentation.deferred_presentations),
            static_cast<unsigned long long>(
                final_crop_presentation.max_consecutive_unavailable),
            static_cast<unsigned long long>(
                final_crop_presentation.mismatched_selection_frames),
            static_cast<unsigned long long>(
                final_crop_presentation.mismatched_surface_frames),
            static_cast<long long>(final_crop_presentation.camera_skew_frames),
            static_cast<unsigned long long>(
                final_crop_presentation.max_abs_camera_skew_frames),
            static_cast<unsigned long long>(read_only_overlay_presentations),
            static_cast<unsigned long long>(keypoint_overlay_presentations),
            static_cast<unsigned long long>(keypoint_overlay_detections),
            static_cast<unsigned long long>(final_crop_metrics.failed_requests),
            reported_crop_error.c_str());
        return 8;
      }
      std::printf(
          "[AppleCropSmoke] PASS start=%d end=%d source=%s camera=%lld "
          "decoded=%lld source_frame=%lld generation=%llu paired=%llu "
          "holds=%llu deferred=%llu max_deferred_run=%llu seeks=%llu "
          "follows=%llu decoder_peak=%zu camera_skew=%+lld "
          "max_abs_camera_skew=%llu overlay_presentations=%llu "
          "keypoint_presentations=%llu keypoint_detections=%llu\n",
          options->video_smoke_start, options->video_smoke_end, source_name,
          static_cast<long long>(
              final_crop_presentation.presented_crop_camera_frame),
          static_cast<long long>(final_crop_decoded_frame),
          static_cast<long long>(
              final_crop_presentation.presented_source_frame),
          static_cast<unsigned long long>(
              final_crop_presentation.last_rendered_generation),
          static_cast<unsigned long long>(
              final_crop_presentation.exact_presentations +
              final_crop_presentation.held_presentations),
          static_cast<unsigned long long>(
              final_crop_presentation.held_presentations),
          static_cast<unsigned long long>(
              final_crop_presentation.deferred_presentations),
          static_cast<unsigned long long>(
              final_crop_presentation.max_consecutive_unavailable),
          static_cast<unsigned long long>(final_crop_metrics.seek_requests),
          static_cast<unsigned long long>(final_crop_metrics.follow_requests),
          final_crop_metrics.decoder.peak_buffered_frames,
          static_cast<long long>(final_crop_presentation.camera_skew_frames),
          static_cast<unsigned long long>(
              final_crop_presentation.max_abs_camera_skew_frames),
          static_cast<unsigned long long>(read_only_overlay_presentations),
          static_cast<unsigned long long>(keypoint_overlay_presentations),
          static_cast<unsigned long long>(keypoint_overlay_detections));
    }
    if (options->multistream_smoke) {
      const double memory_growth_mib =
          smoke_start_memory_mib > 0.0
              ? std::max(0.0, viewer_stats.peak_process_memory_mib -
                                  smoke_start_memory_mib)
              : 0.0;
      const bool buffers_bounded =
          final_video_metrics.peak_buffered_frames <= final_video_capacity &&
          final_stimulus_metrics.decoder.peak_buffered_frames <=
              options->stimulus_buffer_capacity &&
          final_crop_metrics.decoder.peak_buffered_frames <=
              kAcquisitionCropBufferCapacity;
      const bool memory_metrics_available =
          smoke_start_memory_mib > 0.0 &&
          viewer_stats.peak_process_memory_mib >= smoke_start_memory_mib;
      const bool memory_bounded =
          memory_metrics_available &&
          memory_growth_mib <= kMultistreamMemoryGrowthLimitMiB;
      const bool multistream_failed =
          multistream_smoke.stage != MultistreamSmokeStage::Complete ||
          multistream_smoke.exact_settlements != 5 || !buffers_bounded ||
          !memory_bounded || !multistream_smoke.error.empty();
      if (multistream_failed) {
        std::fprintf(
            stderr,
            "[AppleMultistreamSmoke] FAIL pause=%lld step=%lld backward=%lld "
            "forward=%lld end=%lld exact_settlements=%llu camera_peak=%zu/%zu "
            "stimulus_peak=%zu/%zu crop_peak=%zu/%zu "
            "memory_growth_mib=%.1f/%.1f memory_metrics=%s error=%s\n",
            static_cast<long long>(multistream_smoke.pause_frame),
            static_cast<long long>(multistream_smoke.step_frame),
            static_cast<long long>(multistream_smoke.backward_frame),
            static_cast<long long>(multistream_smoke.forward_frame),
            static_cast<long long>(multistream_smoke.end_frame),
            static_cast<unsigned long long>(
                multistream_smoke.exact_settlements),
            final_video_metrics.peak_buffered_frames, final_video_capacity,
            final_stimulus_metrics.decoder.peak_buffered_frames,
            options->stimulus_buffer_capacity,
            final_crop_metrics.decoder.peak_buffered_frames,
            kAcquisitionCropBufferCapacity, memory_growth_mib,
            kMultistreamMemoryGrowthLimitMiB,
            memory_metrics_available ? "available" : "unavailable",
            multistream_smoke.error.c_str());
        return 8;
      }
      std::printf(
          "[AppleMultistreamSmoke] PASS pause=%lld step=%lld backward=%lld "
          "forward=%lld end=%lld exact_settlements=%llu camera_peak=%zu/%zu "
          "stimulus_peak=%zu/%zu crop_peak=%zu/%zu memory_start_mib=%.1f "
          "memory_end_mib=%.1f memory_peak_mib=%.1f "
          "memory_growth_mib=%.1f/%.1f elapsed_s=%.3f\n",
          static_cast<long long>(multistream_smoke.pause_frame),
          static_cast<long long>(multistream_smoke.step_frame),
          static_cast<long long>(multistream_smoke.backward_frame),
          static_cast<long long>(multistream_smoke.forward_frame),
          static_cast<long long>(multistream_smoke.end_frame),
          static_cast<unsigned long long>(multistream_smoke.exact_settlements),
          final_video_metrics.peak_buffered_frames, final_video_capacity,
          final_stimulus_metrics.decoder.peak_buffered_frames,
          options->stimulus_buffer_capacity,
          final_crop_metrics.decoder.peak_buffered_frames,
          kAcquisitionCropBufferCapacity, smoke_start_memory_mib,
          viewer_stats.process_memory_mib, viewer_stats.peak_process_memory_mib,
          memory_growth_mib, kMultistreamMemoryGrowthLimitMiB, elapsed_seconds);
    }
  } else if (options->smoke) {
    if (render_failed || presented_frames < options->smoke_frames) {
      std::fprintf(stderr,
                   "[MacShellSmoke] FAIL presented_frames=%d expected=%d\n",
                   presented_frames, options->smoke_frames);
      return 8;
    }
    std::printf("[MacShellSmoke] PASS presented_frames=%d renderer=Metal "
                "window=GLFW/Cocoa font=bundled\n",
                presented_frames);
  }
  return render_failed || stimulus_failed || crop_failed ? 8 : 0;
}
