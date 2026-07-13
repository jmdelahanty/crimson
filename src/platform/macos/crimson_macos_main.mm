#include "apple_metal_presentation_texture.h"
#include "apple_acquisition_crop_playback_session.h"
#include "apple_stimulus_playback_session.h"
#include "apple_video_metal_renderer.h"
#include "apple_video_playback_buffer.h"
#include "apple_video_viewer_ui.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_metal.h"
#include "implot.h"
#include "playback_clock.h"
#include "crop_presentation_coordinator.h"
#include "stimulus_presentation_coordinator.h"
#include "zarr/archive_context.h"
#include "zarr/tensorstore_acquisition_crop_repository.h"
#include "zarr/tensorstore_stimulus_repository.h"

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#if !defined(__arm64__)
#error "The Crimson macOS backend must be compiled for Apple Silicon arm64."
#endif

#ifndef CRIMSON_GIT_COMMIT
#define CRIMSON_GIT_COMMIT "unknown"
#endif

namespace {

struct LaunchOptions {
  bool smoke = false;
  bool validate_metal = false;
  bool video_smoke = false;
  bool stimulus_smoke = false;
  bool crop_smoke = false;
  int smoke_frames = 12;
  int video_smoke_start = 0;
  int video_smoke_end = 0;
  std::string video_path;
  std::string zarr_path;
  std::string stimulus_run;
  crimson::crop::CropSourcePreference crop_preference =
      crimson::crop::CropSourcePreference::PreferAcquisitionVideo;
};

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
         viewport.x >= 0.0 && viewport.y >= 0.0 && viewport.width > 0.0 &&
         viewport.height > 0.0 &&
         viewport.x + viewport.width <= framebuffer_width + 0.001 &&
         viewport.y + viewport.height <= framebuffer_height + 0.001;
}

std::optional<LaunchOptions> parseOptions(int argc, char **argv) {
  LaunchOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--smoke") {
      options.smoke = true;
      continue;
    }
    if (argument == "--validate-metal") {
      options.validate_metal = true;
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
    if (argument == "--zarr") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Missing value for --zarr\n");
        return std::nullopt;
      }
      options.zarr_path = argv[++i];
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
    if (argument == "--crop-source") {
      if (i + 1 >= argc) {
        std::fprintf(stderr,
                     "Missing value for --crop-source\n");
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
  if (options.smoke && options.validate_metal) {
    std::fprintf(stderr,
                 "--smoke and --validate-metal cannot be used together\n");
    return std::nullopt;
  }
  if (options.video_smoke && options.video_path.empty()) {
    std::fprintf(stderr, "--video-smoke requires --video PATH\n");
    return std::nullopt;
  }
  if (!options.zarr_path.empty() && options.video_path.empty()) {
    std::fprintf(stderr, "--zarr requires --video PATH\n");
    return std::nullopt;
  }
  if (!options.stimulus_run.empty() && options.zarr_path.empty()) {
    std::fprintf(stderr, "--stimulus-run requires --zarr PATH\n");
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
  return options;
}

std::optional<std::string> bundledFontPath() {
  NSBundle *bundle = [NSBundle mainBundle];
  NSURL *url = [bundle URLForResource:@"Roboto-Regular"
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

void configureStyle() {
  ImGui::StyleColorsDark();
  ImGuiStyle &style = ImGui::GetStyle();
  style.WindowRounding = 4.0f;
  style.FrameRounding = 3.0f;
  style.GrabRounding = 3.0f;
  style.Colors[ImGuiCol_Button] = ImVec4(0.52f, 0.10f, 0.15f, 1.0f);
  style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.68f, 0.15f, 0.20f, 1.0f);
  style.Colors[ImGuiCol_CheckMark] = ImVec4(0.90f, 0.30f, 0.34f, 1.0f);
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

  const auto font_path = bundledFontPath();
  const bool bundled_font_loaded =
      font_path &&
      io.Fonts->AddFontFromFileTTF(font_path->c_str(), 15.0f) != nullptr;
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
  glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
  GLFWwindow *window = glfwCreateWindow(1280, 800, "Crimson", nullptr, nullptr);
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
  layer.framebufferOnly = YES;
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
  io.IniFilename = nullptr;
  configureStyle();

  const auto font_path = bundledFontPath();
  const bool bundled_font_loaded =
      font_path &&
      io.Fonts->AddFontFromFileTTF(font_path->c_str(), 15.0f) != nullptr;
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

  std::printf("[MacShell] renderer=Metal window=GLFW/Cocoa architecture=arm64 "
              "revision=%s font=%s\n",
              CRIMSON_GIT_COMMIT, font_path->c_str());

  AppleVideoPlaybackBuffer video_playback;
  AppleVideoMetalRenderer video_renderer;
  LogicalPlaybackClock video_clock;
  AppleVideoViewerStats viewer_stats;
  std::optional<AppleDecodedVideoFrame> current_video_frame;
  std::optional<AppleDecodedVideoFrame> pending_video_frame;
  const bool video_enabled = !options->video_path.empty();
  const bool stimulus_enabled = video_enabled && !options->zarr_path.empty();
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
  int64_t last_stimulus_camera_request = -1;
  AppleAcquisitionCropPlaybackSession crop_playback;
  crimson::crop::CropPresentationCoordinator crop_presentation;
  std::optional<AppleAlignedAcquisitionCropFrame> current_crop_frame;
  crimson::crop::CropSourceSelection current_crop_selection;
  AppleCropViewerControls crop_controls;
  crop_controls.preference = options->crop_preference;
  auto active_crop_preference = crop_controls.preference;
  bool crop_enabled = false;
  bool crop_failed = false;
  bool crop_smoke_end_satisfied = false;
  bool pending_crop_discontinuity = true;
  int64_t last_crop_camera_request = -1;
  std::string crop_error;
  auto video_smoke_started = std::chrono::steady_clock::now();
  if (video_enabled) {
    std::string video_error;
    if (!video_renderer.initialize(
            reinterpret_cast<uintptr_t>((__bridge void *)device),
            static_cast<uint64_t>(layer.pixelFormat), &video_error) ||
        !video_playback.open(options->video_path, "camera-main", 6,
                             &video_error)) {
      std::fprintf(stderr, "[AppleVideo] Initialization failed: %s\n",
                   video_error.c_str());
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
    const int64_t initial_frame =
        options->video_smoke ? options->video_smoke_start : 0;
    if (initial_frame > 0 &&
        !video_playback.requestSeek(initial_frame, &video_error)) {
      std::fprintf(stderr, "[AppleVideo] Initial seek failed: %s\n",
                   video_error.c_str());
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
    if (!video_playback.waitForFrame(prebuffer_frame,
                                     std::chrono::seconds(10))) {
      const auto metrics = video_playback.metrics();
      std::fprintf(stderr,
                   "[AppleVideo] Timed out prebuffering through frame %lld: %s\n",
                   static_cast<long long>(prebuffer_frame),
                   metrics.last_error.c_str());
      video_playback.close();
      ImGui_ImplMetal_Shutdown();
      ImGui_ImplGlfw_Shutdown();
      ImPlot::DestroyContext();
      ImGui::DestroyContext();
      glfwDestroyWindow(window);
      glfwTerminate();
      return 9;
    }
    if (stimulus_enabled) {
      auto archive =
          crimson::zarr::ArchiveContext::Open(options->zarr_path,
                                              &stimulus_error);
      std::unique_ptr<crimson::zarr::StimulusRepository> repository;
      if (archive) {
        repository = crimson::zarr::OpenStimulusRepository(
            archive, options->stimulus_run, &stimulus_error);
      }
      const std::string stimulus_run =
          repository ? repository->runName() : std::string{};
      if (!repository ||
          !stimulus_playback.open(std::move(repository), 6,
                                  &stimulus_error) ||
          initial_frame > std::numeric_limits<int32_t>::max() ||
          !stimulus_playback.requestCameraFrame(
              static_cast<int32_t>(initial_frame), true, &stimulus_error)) {
        std::fprintf(stderr, "[AppleStimulus] Initialization failed: %s\n",
                     stimulus_error.c_str());
        stimulus_playback.close();
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
      const auto initial_resolution = stimulus_playback.resolveCameraFrame(
          static_cast<int32_t>(initial_frame));
      if (initial_resolution.status ==
              crimson::zarr::StimulusMappingStatus::Mapped &&
          !stimulus_playback.waitForCameraFrame(
              static_cast<int32_t>(initial_frame), std::chrono::seconds(10),
              &stimulus_error)) {
        std::fprintf(stderr,
                     "[AppleStimulus] Initial mapped frame failed: %s\n",
                     stimulus_error.c_str());
        stimulus_playback.close();
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
      const auto &stimulus_info = stimulus_playback.info();
      std::printf(
          "[AppleStimulus] run=%s asset=%dx%d frames=%lld fps=%.6f "
          "buffer_capacity=6 startup_ms=%.1f path=%s\n",
          stimulus_run.c_str(), stimulus_info.width, stimulus_info.height,
          static_cast<long long>(stimulus_info.frame_count),
          stimulus_info.nominal_frame_rate,
          stimulus_playback.metrics().decoder.startup_ms,
          stimulus_info.path.c_str());

      auto crop_repository = crimson::zarr::OpenAcquisitionCropRepository(
          archive, &crop_error);
      if (crop_repository &&
          crop_playback.open(std::move(crop_repository),
                             video_playback.info().width,
                             video_playback.info().height, 6, &crop_error)) {
        bool crop_initialization_ready = true;
        if (crop_controls.preference ==
            crimson::crop::CropSourcePreference::PreferAcquisitionVideo) {
          crop_initialization_ready =
              crop_playback.requestCameraFrame(initial_frame, true,
                                               &crop_error);
          const auto initial_crop_resolution =
              crop_playback.resolveCameraFrame(initial_frame);
          if (crop_initialization_ready &&
              initial_crop_resolution.status ==
                  crimson::zarr::AcquisitionCropMappingStatus::Mapped) {
            crop_initialization_ready = crop_playback.waitForCameraFrame(
                initial_frame, std::chrono::seconds(10), &crop_error);
          }
        } else {
          crop_playback.suspend();
        }
        if (crop_initialization_ready) {
          crop_enabled = true;
          const auto &crop_info = crop_playback.info();
          std::printf(
              "[AppleCrop] stream=%s asset=%dx%d frames=%lld fps=%.6f "
              "buffer_capacity=6 startup_ms=%.1f path=%s\n",
              crop_playback.repository()->descriptor().stream_id.c_str(),
              crop_info.width, crop_info.height,
              static_cast<long long>(crop_info.frame_count),
              crop_info.nominal_frame_rate,
              crop_playback.metrics().decoder.startup_ms,
              crop_info.path.c_str());
        }
      }
      if (!crop_enabled) {
        crop_playback.close();
        std::fprintf(stderr, "[AppleCrop] Unavailable: %s\n",
                     crop_error.c_str());
        if (options->crop_smoke) {
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
    }
    video_clock.seek(initial_frame);
    video_clock.play();
    video_playback.setPlaybackState(
        initial_frame, true, video_playback.info().nominal_frame_rate);
    video_smoke_started = std::chrono::steady_clock::now();
    const auto &info = video_playback.info();
    std::printf("[AppleVideo] asset=%dx%d frames=%lld fps=%.6f "
                "buffer_capacity=%zu startup_ms=%.1f path=%s\n",
                info.width, info.height,
                static_cast<long long>(info.frame_count),
                info.nominal_frame_rate, video_playback.capacity(),
                video_playback.metrics().startup_ms, info.path.c_str());
  }
  const double video_smoke_timeout_seconds =
      options->video_smoke
          ? std::max(
                30.0,
                static_cast<double>(options->video_smoke_end -
                                    options->video_smoke_start) /
                        video_playback.info().nominal_frame_rate * 1.5 +
                    30.0)
          : 30.0;

  MTLRenderPassDescriptor *render_pass = [MTLRenderPassDescriptor new];
  AppleMetalPresentationTexture presentation_texture;
  id<MTLCommandBuffer> last_command_buffer = nil;
  std::array<float, 120> frame_times_ms{};
  int frame_time_count = 0;
  int presented_frames = 0;
  bool render_failed = false;
  auto previous_frame_time = std::chrono::steady_clock::now();
  auto last_process_metric_time = LogicalPlaybackClock::TimePoint{};

  while (!glfwWindowShouldClose(window)) {
    @autoreleasepool {
      glfwPollEvents();

      int width = 0;
      int height = 0;
      glfwGetFramebufferSize(window, &width, &height);
      if (width <= 0 || height <= 0) {
        glfwWaitEventsTimeout(0.01);
        continue;
      }
      layer.drawableSize = CGSizeMake(width, height);

      const auto drawable_started = std::chrono::steady_clock::now();
      id<CAMetalDrawable> drawable = [layer nextDrawable];
      if (video_enabled) {
        viewer_stats.max_next_drawable_ms = std::max(
            viewer_stats.max_next_drawable_ms,
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
      ImGui::NewFrame();

      const auto now = std::chrono::steady_clock::now();
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
      if (video_enabled) {
        if (viewer_stats.process_memory_mib == 0.0 ||
            std::chrono::duration<double>(now - last_process_metric_time)
                    .count() >= 1.0) {
          sampleAppleVideoViewerSystemMetrics(viewer_stats);
          last_process_metric_time = now;
        }
        const bool was_playing = video_clock.isPlaying();
        viewer_stats.requested_frame = video_clock.requestedFrame(now);
        if (options->video_smoke && video_clock.isPlaying() &&
            viewer_stats.requested_frame >= options->video_smoke_end) {
          video_clock.pause(now);
          video_clock.seek(options->video_smoke_end, now);
          viewer_stats.requested_frame = options->video_smoke_end;
          pending_camera_discontinuity = true;
        } else if (!options->video_smoke && video_clock.isPlaying() &&
                   viewer_stats.requested_frame >=
                       video_playback.info().frame_count - 1) {
          video_clock.pause(now);
          video_clock.seek(video_playback.info().frame_count - 1, now);
          viewer_stats.requested_frame =
              video_playback.info().frame_count - 1;
          pending_camera_discontinuity = true;
        }
        crop_controls.metrics =
            crop_enabled ? &crop_presentation.metrics() : nullptr;
        const auto control_result = drawAppleVideoControls(
            video_clock, video_playback, viewer_stats,
            stimulus_enabled ? &stimulus_presentation.metrics() : nullptr,
            crop_enabled ? &crop_controls : nullptr,
            !options->video_smoke);
        pending_camera_discontinuity =
            pending_camera_discontinuity ||
            control_result.camera_discontinuity;
        viewer_stats.requested_frame = video_clock.requestedFrame();
        const bool is_playing = video_clock.isPlaying();
        if (was_playing && !is_playing) {
          pending_camera_discontinuity = true;
          std::string pause_error;
          if (!video_playback.requestSeek(viewer_stats.requested_frame,
                                          &pause_error)) {
            std::fprintf(stderr,
                         "[AppleVideo] Pause exact-frame request failed: %s\n",
                         pause_error.c_str());
          }
        }
        pending_crop_discontinuity =
            pending_crop_discontinuity || pending_camera_discontinuity;
        video_playback.setPlaybackState(
            viewer_stats.requested_frame, video_clock.isPlaying(),
            video_playback.info().nominal_frame_rate);
        if (stimulus_enabled && pending_camera_discontinuity) {
          pending_video_frame.reset();
          candidate_stimulus_frame.reset();
          stimulus_candidate_ready = false;
        }
        auto selected = video_playback.frameForTarget(
            viewer_stats.requested_frame, !video_clock.isPlaying());
        if (!stimulus_enabled) {
          if (selected) {
            current_video_frame = std::move(selected);
          } else if (!video_clock.isPlaying()) {
            current_video_frame.reset();
          }
        } else if (!stimulus_failed) {
          if (selected &&
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
                    mapped_camera_frame, resolution,
                    decoded_stimulus_frame);
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
        } else if (selected) {
          current_video_frame = std::move(selected);
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
          last_crop_camera_request = -1;
          pending_crop_discontinuity = true;
          if (active_crop_preference ==
              crimson::crop::CropSourcePreference::PreferLiveGeometry) {
            crop_playback.suspend();
          }
        }

        if (crop_enabled && pending_video_frame &&
            stimulus_candidate_ready && !crop_failed) {
          const int64_t camera_frame =
              pending_video_frame->metadata.frame_number;
          const bool acquisition_preferred =
              crop_controls.preference ==
              crimson::crop::CropSourcePreference::PreferAcquisitionVideo;
          if (acquisition_preferred &&
              (last_crop_camera_request != camera_frame ||
               pending_crop_discontinuity)) {
            if (!crop_playback.requestCameraFrame(
                    camera_frame, pending_crop_discontinuity, &crop_error)) {
              std::fprintf(stderr,
                           "[AppleCrop] Camera frame %lld request failed: %s\n",
                           static_cast<long long>(camera_frame),
                           crop_error.c_str());
              crop_failed = true;
            } else {
              last_crop_camera_request = camera_frame;
              pending_crop_discontinuity = false;
            }
          }

          if (!crop_failed) {
            auto aligned = acquisition_preferred
                               ? crop_playback.frameForCameraFrame(camera_frame)
                               : std::optional<
                                     AppleAlignedAcquisitionCropFrame>{};
            const auto *repository = crop_playback.repository();
            crimson::crop::CropFrameSourceState source_state;
            source_state.camera_frame = camera_frame;
            source_state.exact_full_frame = camera_frame;
            source_state.live_geometry = repository->liveGeometry(
                camera_frame, video_playback.info().width,
                video_playback.info().height);
            source_state.acquisition = repository->acquisitionFrameState(
                camera_frame,
                aligned ? std::optional<int64_t>(
                              aligned->decoded_frame.metadata.frame_number)
                        : std::nullopt,
                video_playback.info().width, video_playback.info().height);
            const auto selection = crimson::crop::SelectCropSource(
                repository->sourceCapabilities(), source_state,
                crop_controls.preference,
                crimson::crop::CropFallbackPolicy::WaitForPreferred,
                static_cast<int64_t>(repository->cameraFrameCount()));
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
        } else if ((!crop_enabled || crop_failed) &&
                   stimulus_candidate_ready) {
          commit_stimulus_candidate();
        }

        if (current_video_frame) {
          viewer_stats.presented_frame =
              current_video_frame->metadata.frame_number;
          if (viewer_stats.presented_frame ==
              viewer_stats.last_presented_frame) {
            ++viewer_stats.repeated_presentations;
          } else if (viewer_stats.last_presented_frame >= 0 &&
                     viewer_stats.presented_frame >
                         viewer_stats.last_presented_frame + 1) {
            viewer_stats.skipped_source_frames += static_cast<uint64_t>(
                viewer_stats.presented_frame -
                viewer_stats.last_presented_frame - 1);
          }
          viewer_stats.last_presented_frame =
              viewer_stats.presented_frame;
          ++viewer_stats.presentation_count;
          const auto &metadata = current_video_frame->metadata;
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
            if (lag_frames > 0.5) {
              ++viewer_stats.late_presentations;
              viewer_stats.max_lag_frames =
                  std::max(viewer_stats.max_lag_frames, lag_frames);
            }
          }
          bool stimulus_frame_encoded = false;
          bool crop_frame_encoded = false;
          std::string render_error;
          const AppleCompositeVideoViewports video_viewports =
              appleCompositeVideoViewports(
                  width, height, io.DisplayFramebufferScale.y,
                  video_playback.info(),
                  crop_enabled ? &crop_playback.info() : nullptr,
                  stimulus_enabled ? &stimulus_playback.info() : nullptr);
          if (!viewportFitsDrawable(video_viewports.camera, width, height) ||
              (crop_enabled &&
               !viewportFitsDrawable(video_viewports.crop, width, height)) ||
              (stimulus_enabled &&
               !viewportFitsDrawable(video_viewports.stimulus, width,
                                      height))) {
            std::fprintf(stderr,
                         "[MacShell] Composite viewport exceeds drawable\n");
            render_failed = true;
            break;
          }
          if (!video_renderer.encode(
                  *current_video_frame,
                  reinterpret_cast<uintptr_t>((__bridge void *)command_buffer),
                  reinterpret_cast<uintptr_t>((__bridge void *)encoder),
                  video_viewports.camera, &render_error)) {
            std::fprintf(stderr, "[AppleVideo] Metal encode failed: %s\n",
                         render_error.c_str());
            render_failed = true;
            break;
          }
          if (stimulus_enabled && current_stimulus_frame) {
            if (!video_renderer.encode(
                    current_stimulus_frame->decoded_frame,
                    reinterpret_cast<uintptr_t>(
                        (__bridge void *)command_buffer),
                    reinterpret_cast<uintptr_t>((__bridge void *)encoder),
                    video_viewports.stimulus, &render_error)) {
              std::fprintf(stderr,
                           "[AppleStimulus] Metal encode failed: %s\n",
                           render_error.c_str());
              render_failed = true;
              break;
            }
            stimulus_frame_encoded = true;
          }
          if (crop_enabled && current_crop_selection.selected() &&
              current_crop_selection.camera_frame == metadata.frame_number) {
            bool crop_encoded = false;
            if (current_crop_selection.source ==
                    crimson::crop::CropSourceKind::AcquisitionVideo &&
                current_crop_frame &&
                current_crop_frame->resolution.camera_frame ==
                    metadata.frame_number) {
              crop_encoded = video_renderer.encode(
                  current_crop_frame->decoded_frame,
                  reinterpret_cast<uintptr_t>(
                      (__bridge void *)command_buffer),
                  reinterpret_cast<uintptr_t>((__bridge void *)encoder),
                  video_viewports.crop, &render_error);
            } else if (current_crop_selection.source ==
                           crimson::crop::CropSourceKind::LiveGeometry &&
                       current_crop_selection.geometry) {
              const auto &geometry = *current_crop_selection.geometry;
              const AppleMetalVideoSourceRegion source_region{
                  geometry.full_frame_crop.x / geometry.source_width,
                  geometry.full_frame_crop.y / geometry.source_height,
                  geometry.full_frame_crop.width / geometry.source_width,
                  geometry.full_frame_crop.height / geometry.source_height};
              crop_encoded = video_renderer.encodeRegion(
                  *current_video_frame,
                  reinterpret_cast<uintptr_t>(
                      (__bridge void *)command_buffer),
                  reinterpret_cast<uintptr_t>((__bridge void *)encoder),
                  video_viewports.crop, source_region, &render_error);
            }
            if (!crop_encoded) {
              std::fprintf(stderr,
                           "[AppleCrop] Metal encode failed: %s\n",
                           render_error.c_str());
              render_failed = true;
              break;
            }
            crop_frame_encoded = true;
          }
          if (crop_enabled) {
            drawAppleCropPreviewOverlay(
                video_viewports.crop, io.DisplayFramebufferScale.y,
                crop_frame_encoded ? &current_crop_selection : nullptr,
                crop_frame_encoded ? current_crop_selection.status
                                   : crop_controls.selection_status);
          }
          if (options->stimulus_smoke &&
              metadata.frame_number >= options->video_smoke_end) {
            if (current_stimulus_resolution.status !=
                crimson::zarr::StimulusMappingStatus::Mapped) {
              stimulus_error =
                  "stimulus smoke end camera frame is not mapped";
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
        }
      } else {
        drawShellSurface(frame_times_ms, frame_time_count, width, height);
      }

      ImGui::Render();
      ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), command_buffer,
                                     encoder);
      [encoder endEncoding];
      [command_buffer presentDrawable:drawable];
      [command_buffer commit];
      last_command_buffer = command_buffer;

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

      ++presented_frames;
      if (options->smoke && !options->video_smoke &&
          presented_frames >= options->smoke_frames) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
      }
      if (options->video_smoke &&
          viewer_stats.presented_frame >= options->video_smoke_end &&
          (!options->stimulus_smoke || stimulus_smoke_end_satisfied ||
           stimulus_failed) &&
          (!options->crop_smoke || crop_smoke_end_satisfied || crop_failed)) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
      }
      if (options->video_smoke &&
          std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                        video_smoke_started)
                  .count() > video_smoke_timeout_seconds) {
        std::fprintf(stderr,
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

  const AppleVideoPlaybackBufferMetrics final_video_metrics =
      video_enabled ? video_playback.metrics()
                    : AppleVideoPlaybackBufferMetrics{};
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
  current_stimulus_frame.reset();
  current_crop_frame.reset();
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
  }

  ImGui_ImplMetal_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImPlot::DestroyContext();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();

  if (options->video_smoke) {
    const double elapsed_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      video_smoke_started)
            .count();
    const double maximum_accepted_lag_frames =
        final_video_info.nominal_frame_rate * 5.0;
    if (render_failed ||
        viewer_stats.presented_frame < options->video_smoke_end ||
        std::fabs(viewer_stats.pts_error_frames) > 0.51 ||
        viewer_stats.max_lag_frames > maximum_accepted_lag_frames) {
      std::fprintf(
          stderr,
          "[AppleVideoSmoke] FAIL start=%d end=%d requested=%lld "
          "presented=%lld decoded=%llu "
          "buffered_peak=%zu max_lag_frames=%.1f lag_limit_frames=%.1f "
          "elapsed_s=%.3f "
          "memory_mib=%.1f peak_memory_mib=%.1f thermal=%s error=%s\n",
          options->video_smoke_start, options->video_smoke_end,
          static_cast<long long>(viewer_stats.requested_frame),
          static_cast<long long>(viewer_stats.presented_frame),
          static_cast<unsigned long long>(final_video_metrics.decoded_frames),
          final_video_metrics.peak_buffered_frames,
          viewer_stats.max_lag_frames, maximum_accepted_lag_frames,
          elapsed_seconds,
          viewer_stats.process_memory_mib,
          viewer_stats.peak_process_memory_mib,
          appleViewerThermalStateName(viewer_stats.thermal_state),
          final_video_metrics.last_error.c_str());
      return 8;
    }
    std::printf(
        "[AppleVideoSmoke] PASS start=%d end=%d requested=%lld "
        "presented=%lld decoded=%llu "
        "peak_buffer=%zu repeats=%llu "
        "skipped_source_frames=%llu late_presentations=%llu max_lag_frames=%.1f "
        "catchup_discarded_frames=%llu catchup_seeks=%llu "
        "pts_error_frames=%+.3f startup_ms=%.1f "
        "seek_ms=%.1f "
        "next_drawable_max_ms=%.1f "
        "command_wait_max_ms=%.1f elapsed_s=%.3f "
        "memory_mib=%.1f peak_memory_mib=%.1f thermal=%s\n",
        options->video_smoke_start, options->video_smoke_end,
        static_cast<long long>(viewer_stats.requested_frame),
        static_cast<long long>(viewer_stats.presented_frame),
        static_cast<unsigned long long>(final_video_metrics.decoded_frames),
        final_video_metrics.peak_buffered_frames,
        static_cast<unsigned long long>(viewer_stats.repeated_presentations),
        static_cast<unsigned long long>(viewer_stats.skipped_source_frames),
        static_cast<unsigned long long>(viewer_stats.late_presentations),
        viewer_stats.max_lag_frames,
        static_cast<unsigned long long>(
            final_video_metrics.catchup_discarded_frames),
        static_cast<unsigned long long>(final_video_metrics.catchup_seeks),
        viewer_stats.pts_error_frames,
        final_video_metrics.startup_ms, final_video_metrics.last_seek_ms,
        viewer_stats.max_next_drawable_ms,
        viewer_stats.max_command_wait_ms, elapsed_seconds,
        viewer_stats.process_memory_mib,
        viewer_stats.peak_process_memory_mib,
        appleViewerThermalStateName(viewer_stats.thermal_state));
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
          stimulus_error.empty()
              ? final_stimulus_metrics.decoder.last_error
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
            "failed_requests=%llu error=%s\n",
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
            static_cast<unsigned long long>(
                final_crop_metrics.failed_requests),
            reported_crop_error.c_str());
        return 8;
      }
      std::printf(
          "[AppleCropSmoke] PASS start=%d end=%d source=%s camera=%lld "
          "decoded=%lld source_frame=%lld generation=%llu paired=%llu "
          "holds=%llu deferred=%llu max_deferred_run=%llu seeks=%llu "
          "follows=%llu decoder_peak=%zu camera_skew=%+lld "
          "max_abs_camera_skew=%llu\n",
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
              final_crop_presentation.max_abs_camera_skew_frames));
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
