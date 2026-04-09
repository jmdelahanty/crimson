#include "gui/frame_debug_review_panel.h"

#include "imgui.h"

namespace {

void drawReviewNavigationSection(const FrameDebugWindowContext& context,
                                 FrameDebugWindowResult& result) {
    ImGui::Separator();
    ImGui::Text("Review Navigation:");
    ReviewFrameFilters filters = context.review_frame_filters;
    bool filters_changed = false;
    filters_changed |= ImGui::Checkbox("Interpolated##review_filter_interpolated",
                                       &filters.include_interpolated);
    ImGui::SameLine();
    filters_changed |= ImGui::Checkbox("Non-clean##review_filter_non_clean",
                                       &filters.include_non_clean);
    ImGui::SameLine();
    filters_changed |=
        ImGui::Checkbox("Empty##review_filter_empty", &filters.include_empty);
    if (filters_changed) {
        result.review_filters_changed = true;
        result.review_frame_filters = filters;
    } else {
        result.review_frame_filters = context.review_frame_filters;
    }

    const bool review_filter_enabled =
        filters.include_interpolated || filters.include_non_clean ||
        filters.include_empty;
    ImGui::BeginDisabled(!review_filter_enabled);
    if (ImGui::Button("Prev Review Frame")) {
        result.request_prev_review_frame = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Next Review Frame")) {
        result.request_next_review_frame = true;
    }
    ImGui::EndDisabled();

    if (!review_filter_enabled) {
        ImGui::TextColored(
            ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
            "Enable at least one review filter to jump frames.");
    } else if (context.review_frame_cache_valid) {
        ImGui::Text("  Indexed review frames: %zu", context.review_frame_count);
    }
    if (!context.review_frame_status.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                           "%s",
                           context.review_frame_status.c_str());
    }
}

}  // namespace

void drawFrameDebugReviewPanel(const FrameDebugWindowContext& context,
                               FrameDebugWindowResult& result) {
    drawReviewNavigationSection(context, result);
}
