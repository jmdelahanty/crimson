#include "gui/full_frame_rect_edit_overlay.h"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <cmath>

namespace {

void cancelDraw(FullFrameRectEditStateView& state) {
    state.draw_active = false;
    state.draw_frame = -1;
    state.draw_anchor_x = 0.0f;
    state.draw_anchor_y = 0.0f;
    state.draw_current_x = 0.0f;
    state.draw_current_y = 0.0f;
}

void clearSelection(FullFrameRectEditStateView& state) {
    state.selected_frame = -1;
    state.selected_box = -1;
    state.drag_active = false;
    state.drag_mouse_button = -1;
    state.drag_offset_x = 0.0f;
    state.drag_offset_y = 0.0f;
}

}  // namespace

ImVec2 clampFullFramePlotPointToImage(float plot_x,
                                      float plot_y,
                                      float image_width_px,
                                      float image_height_px) {
    const float clamped_x =
        std::clamp(plot_x, 0.0f, std::max(0.0f, image_width_px));
    const float clamped_y =
        std::clamp(image_height_px - plot_y, 0.0f, std::max(0.0f, image_height_px));
    return ImVec2(clamped_x, clamped_y);
}

bool isPlotPointInsideFullFrameRect(const FullFrameRect& rect,
                                    double plot_x,
                                    double plot_y,
                                    float image_height_px,
                                    float tolerance_px) {
    const double image_y = static_cast<double>(image_height_px) - plot_y;
    const double x_min = static_cast<double>(rect.x_min) - tolerance_px;
    const double x_max =
        static_cast<double>(rect.x_min + rect.width) + tolerance_px;
    const double y_min = static_cast<double>(rect.y_min) - tolerance_px;
    const double y_max =
        static_cast<double>(rect.y_min + rect.height) + tolerance_px;
    return plot_x >= x_min && plot_x <= x_max && image_y >= y_min &&
           image_y <= y_max;
}

int hitTestFullFrameRectAtPlotPoint(const std::vector<FullFrameRect>& rects,
                                    double plot_x,
                                    double plot_y,
                                    float image_height_px,
                                    float tolerance_px) {
    for (int idx = static_cast<int>(rects.size()) - 1; idx >= 0; --idx) {
        if (isPlotPointInsideFullFrameRect(
                rects[static_cast<size_t>(idx)],
                plot_x,
                plot_y,
                image_height_px,
                tolerance_px)) {
            return idx;
        }
    }
    return -1;
}

FullFrameRectEditResult processFullFrameRectEditInput(
    const FullFrameRectEditContext& context,
    const FullFrameRectEditStateView& initial_state) {
    FullFrameRectEditResult result;
    result.state = initial_state;

    static const std::vector<FullFrameRect> kEmptyRects;
    const auto& rects =
        context.visible_rects != nullptr ? *context.visible_rects : kEmptyRects;

    if (!context.dataset_allows_rect_edit) {
        result.state.draw_mode = false;
        cancelDraw(result.state);
        clearSelection(result.state);
    }
    if (!context.allow_mouse_rect_interaction) {
        result.state.draw_mode = false;
        cancelDraw(result.state);
        result.state.drag_active = false;
        result.state.drag_mouse_button = -1;
    }

    if (result.state.selected_frame == context.current_frame_num &&
        (result.state.selected_box < 0 ||
         result.state.selected_box >= static_cast<int>(rects.size()))) {
        clearSelection(result.state);
    }

    if (context.plot_hovered) {
        if (context.dataset_allows_rect_edit &&
            context.allow_mouse_rect_interaction &&
            ImGui::IsKeyPressed(ImGuiKey_N, false)) {
            result.state.draw_mode = !result.state.draw_mode;
            cancelDraw(result.state);
            if (result.state.draw_mode) {
                clearSelection(result.state);
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            if (result.state.draw_active) {
                cancelDraw(result.state);
            } else if (result.state.draw_mode) {
                result.state.draw_mode = false;
            } else {
                clearSelection(result.state);
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_R, false) && ImGui::GetIO().KeyShift) {
            result.request_reset_frame = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            result.request_delete_selected = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_B, false)) {
            if (rects.empty()) {
                clearSelection(result.state);
            } else {
                if (result.state.draw_mode) {
                    result.state.draw_mode = false;
                    cancelDraw(result.state);
                }
                int next_idx = 0;
                const bool reverse_cycle = ImGui::GetIO().KeyShift;
                if (result.state.selected_frame == context.current_frame_num &&
                    result.state.selected_box >= 0 &&
                    result.state.selected_box < static_cast<int>(rects.size())) {
                    const int box_count = static_cast<int>(rects.size());
                    const int current_idx = result.state.selected_box;
                    next_idx = reverse_cycle
                                   ? (current_idx - 1 + box_count) % box_count
                                   : (current_idx + 1) % box_count;
                }
                result.state.selected_frame = context.current_frame_num;
                result.state.selected_box = next_idx;
                result.state.drag_active = false;
                result.state.drag_mouse_button = -1;
            }
        }
    }

    if (result.state.draw_mode && result.state.drag_active) {
        result.state.drag_active = false;
        result.state.drag_mouse_button = -1;
    }

    if (result.state.draw_active) {
        if (!result.state.draw_mode || !context.can_modify_rects ||
            result.state.draw_frame != context.current_frame_num) {
            cancelDraw(result.state);
        } else {
            ImPlotPoint mouse_plot = ImPlot::GetPlotMousePos();
            ImVec2 current_img = clampFullFramePlotPointToImage(
                static_cast<float>(mouse_plot.x),
                static_cast<float>(mouse_plot.y),
                context.image_width_px,
                context.image_height_px);
            result.state.draw_current_x = current_img.x;
            result.state.draw_current_y = current_img.y;
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const float x_min =
                    std::min(result.state.draw_anchor_x, result.state.draw_current_x);
                const float y_min =
                    std::min(result.state.draw_anchor_y, result.state.draw_current_y);
                const float width =
                    std::fabs(result.state.draw_current_x - result.state.draw_anchor_x);
                const float height =
                    std::fabs(result.state.draw_current_y - result.state.draw_anchor_y);
                if (width >= 2.0f && height >= 2.0f) {
                    result.request_add_rect = true;
                    result.new_rect = {x_min, y_min, width, height};
                }
                cancelDraw(result.state);
            }
        }
    }

    if (context.allow_mouse_rect_interaction && !result.state.draw_mode &&
        !result.state.drag_active &&
        context.plot_hovered && context.can_modify_rects &&
        ImGui::GetIO().KeyCtrl && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        result.state.selected_frame == context.current_frame_num &&
        result.state.selected_box >= 0 &&
        result.state.selected_box < static_cast<int>(rects.size())) {
        const int selected_idx = result.state.selected_box;
        const FullFrameRect& selected_rect = rects[static_cast<size_t>(selected_idx)];
        ImPlotPoint mouse_plot = ImPlot::GetPlotMousePos();
        if (isPlotPointInsideFullFrameRect(selected_rect,
                                           mouse_plot.x,
                                           mouse_plot.y,
                                           context.image_height_px,
                                           context.hit_tolerance_px)) {
            result.state.drag_active = true;
            result.state.drag_mouse_button = ImGuiMouseButton_Left;
            result.state.drag_offset_x =
                static_cast<float>(mouse_plot.x) - selected_rect.x_min;
            const float mouse_y_img =
                context.image_height_px - static_cast<float>(mouse_plot.y);
            result.state.drag_offset_y = mouse_y_img - selected_rect.y_min;
        }
    }

    if (result.state.drag_active) {
        const int held_button = result.state.drag_mouse_button;
        if (!context.allow_mouse_rect_interaction || held_button < 0 ||
            !ImGui::IsMouseDown(static_cast<ImGuiMouseButton>(held_button)) ||
            result.state.selected_frame != context.current_frame_num ||
            result.state.selected_box < 0) {
            result.state.drag_active = false;
            result.state.drag_mouse_button = -1;
        } else if (context.can_modify_rects &&
                   result.state.selected_box < static_cast<int>(rects.size())) {
            ImPlotPoint mouse_plot = ImPlot::GetPlotMousePos();
            const float mouse_y_img =
                context.image_height_px - static_cast<float>(mouse_plot.y);
            result.request_move_selected = true;
            result.move_box_index = result.state.selected_box;
            result.move_target_x =
                static_cast<float>(mouse_plot.x) - result.state.drag_offset_x;
            result.move_target_y = mouse_y_img - result.state.drag_offset_y;
        } else {
            result.state.drag_active = false;
            result.state.drag_mouse_button = -1;
        }
    }

    int click_button = -1;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left, false)) {
        click_button = ImGuiMouseButton_Left;
    }
    if (context.allow_mouse_rect_interaction && context.plot_hovered &&
        click_button >= 0) {
        if (result.state.draw_mode) {
            if (click_button == ImGuiMouseButton_Left && context.can_modify_rects &&
                ImGui::GetIO().KeyCtrl) {
                ImPlotPoint mouse_plot = ImPlot::GetPlotMousePos();
                ImVec2 start_img = clampFullFramePlotPointToImage(
                    static_cast<float>(mouse_plot.x),
                    static_cast<float>(mouse_plot.y),
                    context.image_width_px,
                    context.image_height_px);
                result.state.draw_active = true;
                result.state.draw_frame = context.current_frame_num;
                result.state.draw_anchor_x = start_img.x;
                result.state.draw_anchor_y = start_img.y;
                result.state.draw_current_x = start_img.x;
                result.state.draw_current_y = start_img.y;
                result.state.drag_active = false;
                result.state.drag_mouse_button = -1;
            } else if (!context.can_modify_rects) {
                cancelDraw(result.state);
            }
        } else {
            ImPlotPoint mouse_plot = ImPlot::GetPlotMousePos();
            bool started_selected_drag = false;
            if (context.can_modify_rects &&
                result.state.selected_frame == context.current_frame_num &&
                result.state.selected_box >= 0 &&
                result.state.selected_box < static_cast<int>(rects.size())) {
                const int selected_idx = result.state.selected_box;
                const FullFrameRect& selected_rect =
                    rects[static_cast<size_t>(selected_idx)];
                if (isPlotPointInsideFullFrameRect(selected_rect,
                                                   mouse_plot.x,
                                                   mouse_plot.y,
                                                   context.image_height_px,
                                                   context.hit_tolerance_px)) {
                    result.state.selected_frame = context.current_frame_num;
                    result.state.selected_box = selected_idx;
                    result.state.drag_active = true;
                    result.state.drag_mouse_button = click_button;
                    result.state.drag_offset_x =
                        static_cast<float>(mouse_plot.x) - selected_rect.x_min;
                    const float mouse_y_img =
                        context.image_height_px -
                        static_cast<float>(mouse_plot.y);
                    result.state.drag_offset_y = mouse_y_img - selected_rect.y_min;
                    started_selected_drag = true;
                }
            }

            if (!started_selected_drag) {
                const int hit_idx = hitTestFullFrameRectAtPlotPoint(
                    rects,
                    mouse_plot.x,
                    mouse_plot.y,
                    context.image_height_px,
                    context.hit_tolerance_px);
                if (hit_idx >= 0 && hit_idx < static_cast<int>(rects.size())) {
                    result.state.selected_frame = context.current_frame_num;
                    result.state.selected_box = hit_idx;
                    if (context.can_modify_rects) {
                        const FullFrameRect& selected_rect =
                            rects[static_cast<size_t>(hit_idx)];
                        result.state.drag_active = true;
                        result.state.drag_mouse_button = click_button;
                        result.state.drag_offset_x =
                            static_cast<float>(mouse_plot.x) - selected_rect.x_min;
                        const float mouse_y_img =
                            context.image_height_px -
                            static_cast<float>(mouse_plot.y);
                        result.state.drag_offset_y =
                            mouse_y_img - selected_rect.y_min;
                    } else {
                        result.state.drag_active = false;
                        result.state.drag_mouse_button = -1;
                    }
                } else {
                    clearSelection(result.state);
                }
            }
        }
    }

    return result;
}

void drawFullFrameRectOverlays(const std::vector<FullFrameRectOverlayItem>& items,
                               float image_height_px) {
    for (const auto& item : items) {
        const auto& rect = item.rect;
        double x_coords[5] = {rect.x_min,
                              rect.x_min + rect.width,
                              rect.x_min + rect.width,
                              rect.x_min,
                              rect.x_min};
        double y_coords[5] = {static_cast<double>(image_height_px) - rect.y_min,
                              static_cast<double>(image_height_px) - rect.y_min,
                              static_cast<double>(image_height_px) -
                                  (rect.y_min + rect.height),
                              static_cast<double>(image_height_px) -
                                  (rect.y_min + rect.height),
                              static_cast<double>(image_height_px) - rect.y_min};
        ImPlot::SetNextLineStyle(item.color, item.line_width);
        ImPlot::PlotLine(item.label.c_str(), x_coords, y_coords, 5);
    }
}

void drawFullFrameRectDraftOverlay(const FullFrameRectEditStateView& state,
                                   int current_frame_num,
                                   float image_height_px,
                                   const char* label_suffix) {
    if (!state.draw_active || state.draw_frame != current_frame_num) {
        return;
    }

    const double draft_x0 = std::min(state.draw_anchor_x, state.draw_current_x);
    const double draft_x1 = std::max(state.draw_anchor_x, state.draw_current_x);
    const double draft_y0 = std::min(state.draw_anchor_y, state.draw_current_y);
    const double draft_y1 = std::max(state.draw_anchor_y, state.draw_current_y);
    if ((draft_x1 - draft_x0) < 1.0 || (draft_y1 - draft_y0) < 1.0) {
        return;
    }

    double x_coords[5] = {draft_x0, draft_x1, draft_x1, draft_x0, draft_x0};
    double y_coords[5] = {static_cast<double>(image_height_px) - draft_y0,
                          static_cast<double>(image_height_px) - draft_y0,
                          static_cast<double>(image_height_px) - draft_y1,
                          static_cast<double>(image_height_px) - draft_y1,
                          static_cast<double>(image_height_px) - draft_y0};
    ImPlot::SetNextLineStyle(ImVec4(1.0f, 1.0f, 0.2f, 0.95f), 2.0f);
    std::string draft_label = std::string("FullFrameDraft##") + label_suffix;
    ImPlot::PlotLine(draft_label.c_str(), x_coords, y_coords, 5);
}
