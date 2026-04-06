#pragma once

#include "imgui.h"

#include <string>
#include <vector>

struct FullFrameRect {
    float x_min = 0.0f;
    float y_min = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    bool valid() const { return width > 0.0f && height > 0.0f; }
};

struct FullFrameRectOverlayItem {
    FullFrameRect rect;
    ImVec4 color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    float line_width = 2.0f;
    std::string label;
};

struct FullFrameRectEditStateView {
    int selected_frame = -1;
    int selected_box = -1;
    bool drag_active = false;
    int drag_mouse_button = -1;
    float drag_offset_x = 0.0f;
    float drag_offset_y = 0.0f;
    bool draw_mode = false;
    bool draw_active = false;
    int draw_frame = -1;
    float draw_anchor_x = 0.0f;
    float draw_anchor_y = 0.0f;
    float draw_current_x = 0.0f;
    float draw_current_y = 0.0f;
};

struct FullFrameRectEditContext {
    int current_frame_num = 0;
    float image_width_px = 0.0f;
    float image_height_px = 0.0f;
    bool plot_hovered = false;
    bool dataset_allows_rect_edit = false;
    bool can_modify_rects = false;
    const std::vector<FullFrameRect>* visible_rects = nullptr;
    float hit_tolerance_px = 6.0f;
};

struct FullFrameRectEditResult {
    FullFrameRectEditStateView state;
    bool request_reset_frame = false;
    bool request_delete_selected = false;
    bool request_add_rect = false;
    FullFrameRect new_rect;
    bool request_move_selected = false;
    int move_box_index = -1;
    float move_target_x = 0.0f;
    float move_target_y = 0.0f;
};

ImVec2 clampFullFramePlotPointToImage(float plot_x,
                                      float plot_y,
                                      float image_width_px,
                                      float image_height_px);

int hitTestFullFrameRectAtPlotPoint(const std::vector<FullFrameRect>& rects,
                                    double plot_x,
                                    double plot_y,
                                    float image_height_px,
                                    float tolerance_px);

bool isPlotPointInsideFullFrameRect(const FullFrameRect& rect,
                                    double plot_x,
                                    double plot_y,
                                    float image_height_px,
                                    float tolerance_px);

FullFrameRectEditResult processFullFrameRectEditInput(
    const FullFrameRectEditContext& context,
    const FullFrameRectEditStateView& initial_state);

void drawFullFrameRectOverlays(const std::vector<FullFrameRectOverlayItem>& items,
                               float image_height_px);

void drawFullFrameRectDraftOverlay(const FullFrameRectEditStateView& state,
                                   int current_frame_num,
                                   float image_height_px,
                                   const char* label_suffix);
