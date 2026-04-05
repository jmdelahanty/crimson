#include "gui/keypoints_window.h"

#include "imgui.h"

void drawKeypointsWindow(const KeypointsWindowContext& context) {
    if (!ImGui::Begin("Keypoints")) {
        ImGui::End();
        return;
    }

    const float text_base_height = ImGui::GetTextLineHeightWithSpacing();
    const int rows_count = context.num_cams;
    const int columns_count =
        (context.skeleton != nullptr) ? context.skeleton->num_nodes + 1 : 1;

    auto frame_it = context.keypoints_map.find(context.current_frame_num);
    KeyPoints* frame_keypoints =
        (frame_it != context.keypoints_map.end()) ? frame_it->second : nullptr;

    static ImGuiTableFlags table_flags =
        ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersOuter |
        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Hideable |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_HighlightHoveredColumn;

    if (ImGui::BeginTable("table_angled_headers",
                          columns_count,
                          table_flags,
                          ImVec2(0.0f, text_base_height * 12))) {
        ImGui::TableSetupColumn("Name",
                                ImGuiTableColumnFlags_NoHide |
                                    ImGuiTableColumnFlags_NoReorder);
        if (context.skeleton != nullptr) {
            for (int column = 1; column < columns_count; column++) {
                ImGui::TableSetupColumn(
                    context.skeleton->node_names[column - 1].c_str(),
                    ImGuiTableColumnFlags_AngledHeader |
                        ImGuiTableColumnFlags_WidthFixed);
            }
        }
        ImGui::TableSetupScrollFreeze(1, 2);
        ImGui::TableAngledHeadersRow();
        ImGui::TableHeadersRow();

        for (int row = 0; row < rows_count; row++) {
            ImGui::PushID(row);
            ImGui::TableNextRow();

            if (context.keypoints_find && row < static_cast<int>(context.is_view_focused.size()) &&
                context.is_view_focused[row]) {
                ImU32 row_bg_color =
                    ImGui::GetColorU32(ImVec4(0.7f, 0.3f, 0.3f, 0.65f));
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, row_bg_color);
            }

            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            const char* camera_name =
                (row < static_cast<int>(context.camera_names.size()))
                    ? context.camera_names[row].c_str()
                    : "unknown";
            ImGui::Text("%s", camera_name);

            for (int column = 1; column < columns_count; column++) {
                if (!ImGui::TableSetColumnIndex(column)) {
                    continue;
                }
                if (!context.keypoints_find || frame_keypoints == nullptr ||
                    context.skeleton == nullptr) {
                    continue;
                }

                ImVec4 node_color = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
                if (frame_keypoints->active_id[row] == column - 1) {
                    node_color = (ImVec4)ImColor::HSV(0.8f, 1.0f, 1.0f);
                } else if (frame_keypoints->keypoints2d[row][column - 1]
                               .is_labeled) {
                    node_color = context.skeleton->node_colors[column - 1];
                    node_color.w = 0.9f;
                }

                if (frame_keypoints->keypoints2d[row][column - 1]
                        .is_triangulated) {
                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "T");
                }

                ImU32 cell_bg_color = ImGui::GetColorU32(node_color);
                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg,
                                       cell_bg_color);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::End();
}
