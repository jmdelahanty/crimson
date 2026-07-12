#include "apple_metal_presentation_texture.h"
#include "apple_video_metal_renderer.h"
#include "apple_video_playback_buffer.h"
#include "apple_video_viewer_ui.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_metal.h"
#include "implot.h"
#include "playback_clock.h"

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
  int smoke_frames = 12;
  int video_smoke_start = 0;
  int video_smoke_end = 0;
  std::string video_path;
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
  const bool video_enabled = !options->video_path.empty();
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
        }
        drawAppleVideoControls(video_clock, video_playback, viewer_stats,
                               !options->video_smoke);
        viewer_stats.requested_frame = video_clock.requestedFrame();
        const bool is_playing = video_clock.isPlaying();
        if (was_playing && !is_playing) {
          std::string pause_error;
          if (!video_playback.requestSeek(viewer_stats.requested_frame,
                                          &pause_error)) {
            std::fprintf(stderr,
                         "[AppleVideo] Pause exact-frame request failed: %s\n",
                         pause_error.c_str());
          }
        }
        video_playback.setPlaybackState(
            viewer_stats.requested_frame, video_clock.isPlaying(),
            video_playback.info().nominal_frame_rate);
        auto selected = video_playback.frameForTarget(
            viewer_stats.requested_frame, !video_clock.isPlaying());
        if (selected) {
          current_video_frame = std::move(selected);
        } else if (!video_clock.isPlaying()) {
          current_video_frame.reset();
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
          std::string render_error;
          const AppleMetalVideoViewport video_viewport = appleVideoViewport(
              width, height, io.DisplayFramebufferScale.y,
              video_playback.info());
          if (!video_renderer.encode(
                  *current_video_frame,
                  reinterpret_cast<uintptr_t>((__bridge void *)command_buffer),
                  reinterpret_cast<uintptr_t>((__bridge void *)encoder),
                  video_viewport, &render_error)) {
            std::fprintf(stderr, "[AppleVideo] Metal encode failed: %s\n",
                         render_error.c_str());
            render_failed = true;
            break;
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
          viewer_stats.presented_frame >= options->video_smoke_end) {
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
  if (video_enabled) {
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
        video_playback.info().nominal_frame_rate * 5.0;
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
  return render_failed ? 8 : 0;
}
