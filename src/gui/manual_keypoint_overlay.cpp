#include "gui/manual_keypoint_overlay.h"

#include "imgui.h"
#include "implot.h"

#include <iomanip>
#include <sstream>

void gui_plot_keypoints(KeyPoints *keypoints,
                        SkeletonContext *skeleton,
                        int view_idx,
                        int num_cams) {
    float pt_size = 6.0f;
    for (u32 node = 0; node < static_cast<u32>(skeleton->num_nodes); node++) {
        if (keypoints->keypoints2d[view_idx][node].is_labeled) {
            ImVec4 node_color;
            if (keypoints->active_id[view_idx] == node) {
                node_color = (ImVec4)ImColor::HSV(0.8, 1.0f, 1.0f);
                node_color.w = 0.9f;
                pt_size = 8.0f;
            } else {
                node_color = skeleton->node_colors.at(node);
                node_color.w = 0.9f;
                pt_size = 6.0f;
            }
            int id = skeleton->num_nodes * view_idx + static_cast<int>(node);
            static bool drag_point_clicked;
            static bool drag_point_hovered;
            static bool drag_point_modified;
            drag_point_modified = ImPlot::DragPoint(
                id, &keypoints->keypoints2d[view_idx][node].position.x,
                &keypoints->keypoints2d[view_idx][node].position.y, node_color,
                pt_size, ImPlotDragToolFlags_None, &drag_point_clicked,
                &drag_point_hovered);
            if (drag_point_modified) {
                keypoints->keypoints2d[view_idx][node].is_triangulated = false;
            }
            if (drag_point_hovered) {
                if (keypoints->keypoints2d[view_idx][node].is_triangulated) {
                    std::ostringstream oss;
                    oss << std::fixed << std::setprecision(2);
                    oss << "(" << keypoints->keypoints3d[node].x << ", "
                        << keypoints->keypoints3d[node].y << ", "
                        << keypoints->keypoints3d[node].z << ")";
                    std::string label = oss.str();
                    ImVec2 mouse_pos = ImGui::GetMousePos();
                    ImVec2 textPos = ImVec2(mouse_pos.x + 10, mouse_pos.y + 10);
                    ImGui::GetForegroundDrawList()->AddText(
                        textPos, IM_COL32(220, 20, 60, 255), label.c_str());
                }

                if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
                    keypoints->keypoints2d[view_idx][node].position = {1E7, 1E7};
                    keypoints->keypoints2d[view_idx][node].is_labeled = false;
                    keypoints->keypoints2d[view_idx][node].is_triangulated =
                        false;
                    keypoints->active_id[view_idx] = node;
                }

                if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
                    for (int cam_idx = 0; cam_idx < num_cams; cam_idx++) {
                        keypoints->keypoints2d[cam_idx][node].position = {1E7,
                                                                          1E7};
                        keypoints->keypoints2d[cam_idx][node].is_labeled =
                            false;
                        keypoints->keypoints2d[cam_idx][node].is_triangulated =
                            false;
                        keypoints->active_id[cam_idx] = node;
                    }
                }
            }

            if (drag_point_clicked) {
                keypoints->active_id[view_idx] = node;
            }
        }
    }

    for (u32 edge = 0; edge < static_cast<u32>(skeleton->num_edges); edge++) {
        auto [a, b] = skeleton->edges[edge];

        if (keypoints->keypoints2d[view_idx][a].is_labeled &&
            keypoints->keypoints2d[view_idx][b].is_labeled) {
            double xs[2]{keypoints->keypoints2d[view_idx][a].position.x,
                         keypoints->keypoints2d[view_idx][b].position.x};
            double ys[2]{keypoints->keypoints2d[view_idx][a].position.y,
                         keypoints->keypoints2d[view_idx][b].position.y};
            ImPlot::PlotLine("##line", xs, ys, 2);
        }
    }
}

void gui_plot_bbox_from_keypoints(KeyPoints *keypoints,
                                  SkeletonContext * /*skeleton*/,
                                  int view_idx,
                                  int top_left_idx,
                                  int bottom_right_idx) {
    if (keypoints->keypoints2d[view_idx][top_left_idx].is_labeled &&
        keypoints->keypoints2d[view_idx][bottom_right_idx].is_labeled) {
        double xs[5]{
            keypoints->keypoints2d[view_idx][top_left_idx].position.x,
            keypoints->keypoints2d[view_idx][bottom_right_idx].position.x,
            keypoints->keypoints2d[view_idx][bottom_right_idx].position.x,
            keypoints->keypoints2d[view_idx][top_left_idx].position.x,
            keypoints->keypoints2d[view_idx][top_left_idx].position.x};

        double ys[5]{
            keypoints->keypoints2d[view_idx][top_left_idx].position.y,
            keypoints->keypoints2d[view_idx][top_left_idx].position.y,
            keypoints->keypoints2d[view_idx][bottom_right_idx].position.y,
            keypoints->keypoints2d[view_idx][bottom_right_idx].position.y,
            keypoints->keypoints2d[view_idx][top_left_idx].position.y};

        ImPlot::SetNextLineStyle(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), 3.0f);
        ImPlot::PlotLine("##line", xs, ys, 5);
    }
}
