#include "gui/frame_buffer_window.h"

#include "imgui.h"

#include <chrono>
#include <cstdio>
#include <string>

namespace crimson::gui {
namespace {

double durationMs(std::chrono::steady_clock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

std::string frameLabel(const playback::PlaybackPresentationCandidate &item,
                       int64_t selected_frame, int64_t newest_frame) {
  const int64_t selected_delta = item.frame_number - selected_frame;
  const int64_t newest_delta = item.frame_number - newest_frame;
  char label[160];
  if (item.slot) {
    std::snprintf(label, sizeof(label),
                  "Frame %lld (slot %d, selected %+lld, newest %+lld)",
                  static_cast<long long>(item.frame_number), *item.slot,
                  static_cast<long long>(selected_delta),
                  static_cast<long long>(newest_delta));
  } else {
    std::snprintf(label, sizeof(label),
                  "Frame %lld (selected %+lld, newest %+lld)",
                  static_cast<long long>(item.frame_number),
                  static_cast<long long>(selected_delta),
                  static_cast<long long>(newest_delta));
  }
  return label;
}

} // namespace

FrameBufferWindowResult
drawFrameBufferWindow(const FrameBufferWindowContext &context) {
  const auto draw_start = std::chrono::steady_clock::now();
  FrameBufferWindowResult result;
  const auto &model = context.model;

  ImGui::SetNextWindowSize(ImVec2(500, 440), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Frames in the buffer")) {
    ImGui::Text("Valid frames: %zu / %zu", model.items.size(),
                context.buffer_capacity);
    if (context.pause_origin_frame) {
      ImGui::Text("Pause origin frame: %lld, resume mode: %s",
                  static_cast<long long>(*context.pause_origin_frame),
                  context.buffer_browsed_since_pause
                      ? "buffered resume / camera re-anchor"
                      : "smooth resume from pause frame");
    }
    if (context.last_resume) {
      ImGui::Text("Last resume: %.*s (target %lld)",
                  static_cast<int>(context.last_resume->path.size()),
                  context.last_resume->path.data(),
                  static_cast<long long>(context.last_resume->target_frame));
    }
    if (!model.items.empty()) {
      ImGui::Text("Selected/displayed frame: %lld",
                  static_cast<long long>(model.selected_frame));
      ImGui::Text(
          "Buffered spans: %zu, oldest: %lld, newest: %lld, largest gap: %lld",
          model.spans.size(), static_cast<long long>(model.oldest_frame),
          static_cast<long long>(model.newest_frame),
          static_cast<long long>(model.largest_gap));
      ImGui::Text("Newest buffered frame: %lld",
                  static_cast<long long>(model.newest_frame));
    }

    if (model.items.empty()) {
      ImGui::TextDisabled("No decoded frames currently buffered.");
    } else {
      for (size_t span_index = 0; span_index < model.spans.size();
           ++span_index) {
        const auto &span = model.spans[span_index];
        const auto &first_item = model.items[span.begin_index];
        const auto &last_item = model.items[span.end_index];

        if (span_index > 0) {
          const auto &previous_item =
              model.items[model.spans[span_index - 1].end_index];
          const int64_t missing_frames =
              first_item.frame_number - previous_item.frame_number - 1;
          if (missing_frames > 0) {
            ImGui::Separator();
            ImGui::TextDisabled(
                "Gap: %lld missing frames (%lld..%lld)",
                static_cast<long long>(missing_frames),
                static_cast<long long>(previous_item.frame_number + 1),
                static_cast<long long>(first_item.frame_number - 1));
          }
        }

        const int64_t selected_start =
            first_item.frame_number - model.selected_frame;
        const int64_t selected_end =
            last_item.frame_number - model.selected_frame;
        const int64_t newest_start =
            first_item.frame_number - model.newest_frame;
        const int64_t newest_end = last_item.frame_number - model.newest_frame;
        if (first_item.slot && last_item.slot) {
          ImGui::TextDisabled(
              "Span %lld-%lld (%zu frames, slots %d-%d, selected %+lld..%+lld, "
              "newest %+lld..%+lld)",
              static_cast<long long>(first_item.frame_number),
              static_cast<long long>(last_item.frame_number),
              span.end_index - span.begin_index + 1, *first_item.slot,
              *last_item.slot, static_cast<long long>(selected_start),
              static_cast<long long>(selected_end),
              static_cast<long long>(newest_start),
              static_cast<long long>(newest_end));
        } else {
          ImGui::TextDisabled(
              "Span %lld-%lld (%zu frames, selected %+lld..%+lld, newest "
              "%+lld..%+lld)",
              static_cast<long long>(first_item.frame_number),
              static_cast<long long>(last_item.frame_number),
              span.end_index - span.begin_index + 1,
              static_cast<long long>(selected_start),
              static_cast<long long>(selected_end),
              static_cast<long long>(newest_start),
              static_cast<long long>(newest_end));
        }

        for (size_t index = span.begin_index; index <= span.end_index;
             ++index) {
          const auto &item = model.items[index];
          const std::string label =
              frameLabel(item, model.selected_frame, model.newest_frame);
          ImGui::PushID(static_cast<int>(index));
          if (ImGui::Selectable(label.c_str(),
                                model.selected_index &&
                                    *model.selected_index == index)) {
            result.selection = item;
          }
          ImGui::PopID();
        }
      }
    }
  }
  ImGui::End();
  result.draw_ms = durationMs(std::chrono::steady_clock::now() - draw_start);
  return result;
}

} // namespace crimson::gui
