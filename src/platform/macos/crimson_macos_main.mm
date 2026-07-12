#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_metal.h"
#include "implot.h"

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
  int smoke_frames = 12;
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

  MTLRenderPassDescriptor *render_pass = [MTLRenderPassDescriptor new];
  render_pass.colorAttachments[0].texture = texture;
  render_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  render_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  render_pass.colorAttachments[0].clearColor =
      MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

  id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
  id<MTLRenderCommandEncoder> encoder =
      [command_buffer renderCommandEncoderWithDescriptor:render_pass];
  if (texture == nil || command_buffer == nil || encoder == nil) {
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

  MTLRenderPassDescriptor *render_pass = [MTLRenderPassDescriptor new];
  id<MTLCommandBuffer> last_command_buffer = nil;
  std::array<float, 120> frame_times_ms{};
  int frame_time_count = 0;
  int presented_frames = 0;
  bool render_failed = false;
  auto previous_frame_time = std::chrono::steady_clock::now();

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

      id<CAMetalDrawable> drawable = [layer nextDrawable];
      if (drawable == nil) {
        glfwWaitEventsTimeout(0.01);
        continue;
      }

      id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
      render_pass.colorAttachments[0].texture = drawable.texture;
      render_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      render_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      render_pass.colorAttachments[0].clearColor =
          MTLClearColorMake(0.055, 0.060, 0.065, 1.0);
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
      drawShellSurface(frame_times_ms, frame_time_count, width, height);

      ImGui::Render();
      ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), command_buffer,
                                     encoder);
      [encoder endEncoding];
      [command_buffer presentDrawable:drawable];
      [command_buffer commit];
      last_command_buffer = command_buffer;

      if (options->smoke) {
        [command_buffer waitUntilCompleted];
        if (command_buffer.status != MTLCommandBufferStatusCompleted) {
          std::fprintf(stderr, "[MacShellSmoke] Metal command failed: %s\n",
                       command_buffer.error.localizedDescription.UTF8String);
          render_failed = true;
          break;
        }
      }

      ++presented_frames;
      if (options->smoke && presented_frames >= options->smoke_frames) {
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

  ImGui_ImplMetal_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImPlot::DestroyContext();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();

  if (options->smoke) {
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
