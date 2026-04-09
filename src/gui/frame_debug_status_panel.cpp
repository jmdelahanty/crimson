#include "gui/frame_debug_status_panel.h"

#include "imgui.h"

#include <algorithm>

namespace {

void drawFrameOverviewSection(const FrameDebugWindowContext& context) {
    ImGui::Text("Inspecting Frame: %d", context.current_frame_num);
    if (context.zarr_loader.hasStimulusAlignment()) {
        if (auto current_stimulus_frame =
                context.zarr_loader.getStimulusFrameForCameraFrame(
                    context.current_frame_num)) {
            ImGui::Text("Stimulus frame: %d", *current_stimulus_frame);
        } else {
            ImGui::TextDisabled("Stimulus frame: not mapped");
        }
    }
    ImGui::Separator();
}

void drawDatasetSelectionSection(const FrameDebugWindowContext& context,
                                 FrameDebugWindowResult& result) {
    if (context.detection_dataset_labels.empty()) {
        return;
    }

    ImGui::Text("Detection dataset:");
    const int clamped_choice = std::min<int>(
        context.detection_dataset_choice,
        static_cast<int>(context.detection_dataset_labels.size()) - 1);
    const char* current_label =
        context.detection_dataset_labels[clamped_choice].c_str();
    if (ImGui::BeginCombo("##detection_dataset_combo", current_label)) {
        for (int i = 0;
             i < static_cast<int>(context.detection_dataset_labels.size());
             ++i) {
            bool selected = (i == context.detection_dataset_choice);
            if (ImGui::Selectable(context.detection_dataset_labels[i].c_str(),
                                  selected)) {
                result.requested_detection_dataset_index = i;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
}

void drawReviewStatusSection(const FrameDebugWindowContext& context) {
    const auto review_artifacts = context.zarr_loader.getAvailableReviewArtifacts();
    if (review_artifacts.empty()) {
        return;
    }

    ImGui::Separator();
    ImGui::Text("Review Artifacts:");
    for (const auto& artifact : review_artifacts) {
        ImGui::Text("%s", artifact.label.c_str());
        if (!artifact.run_name.empty()) {
            ImGui::Text("  Run: %s", artifact.run_name.c_str());
        }
        if (!artifact.has_review_status) {
            ImGui::TextDisabled("  Review metadata unavailable");
            continue;
        }

        const auto& review_state = artifact.review_state;
        ImVec4 status_color = (review_state == "approved")
                                  ? ImVec4(0.2f, 0.9f, 0.2f, 1.0f)
                              : (review_state == "rejected")
                                  ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f)
                                  : ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
        ImGui::TextColored(status_color, "  Review: %s",
                           review_state.c_str());
        if (!artifact.review_intended_use.empty() ||
            !artifact.review_method.empty()) {
            ImGui::Text("  Use: %s | Method: %s",
                        artifact.review_intended_use.c_str(),
                        artifact.review_method.c_str());
        }
        if (!artifact.review_timestamp.empty()) {
            ImGui::Text("  Reviewed: %s",
                        artifact.review_timestamp.c_str());
        }
        if (!artifact.review_reviewer.empty()) {
            ImGui::Text("  Reviewer: %s",
                        artifact.review_reviewer.c_str());
        }
        if (!artifact.review_notes.empty()) {
            ImGui::Text("  Notes: %s",
                        artifact.review_notes.c_str());
        }
    }
}

void drawDetectionSummarySection(const FrameDebugWindowContext& context) {
    if (!context.zarr_loader.hasDetectionData()) {
        ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.25f, 1.0f),
                           "[Zarr] Detection runs: unavailable (metadata/stimulus-only mode)");
        return;
    }

    if (!context.zarr_boxes.empty()) {
        if (context.frame_is_interpolated &&
            context.dataset_has_synthetic_boxes) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.7f, 0.0f, 1.0f),
                "[Zarr] Detections:    Found %zu (INTERPOLATED)",
                context.zarr_boxes.size());
        } else if (context.frame_is_interpolated &&
                   !context.dataset_has_synthetic_boxes) {
            ImGui::TextColored(
                ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                "[Zarr] Detections:    Found %zu (original, interp available)",
                context.zarr_boxes.size());
        } else {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f),
                               "[Zarr] Detections:    Found %zu",
                               context.zarr_boxes.size());
        }

        if (context.detection_details != nullptr &&
            context.zarr_loader.hasScores() &&
            !context.detection_details->scores.empty()) {
            float max_score = *std::max_element(
                context.detection_details->scores.begin(),
                context.detection_details->scores.end());
            ImGui::Text("  Max confidence: %.2f", max_score);
        }

        if (context.zarr_loader.hasClassIDs()) {
            ImGui::Text("  Has class IDs: Yes");
        }

        if (context.zarr_loader.hasHeadingData()) {
            if (!context.dataset_has_synthetic_boxes &&
                context.detection_details != nullptr &&
                !context.detection_details->heading_valid.empty()) {
                size_t valid_headings =
                    std::count(context.detection_details->heading_valid.begin(),
                               context.detection_details->heading_valid.end(),
                               1);
                ImGui::Text("  Heading vectors: %zu valid", valid_headings);
            } else {
                ImGui::Text(
                    "  Heading vectors available (use original detections)");
            }
        }

        if (context.zarr_loader.hasInterpolation()) {
            ImGui::Text("  Interpolation available: Yes");
            ImGui::Text("  Current frame interpolated: %s",
                        context.frame_is_interpolated ? "Yes" : "No");
            ImGui::Text("  Using interpolation: %s",
                        context.dataset_has_synthetic_boxes ? "Yes" : "No");
            ImGui::Text("  Method: %s",
                        context.zarr_loader.getInterpolationMethod().c_str());
        }
    } else {
        if (context.zarr_loader.hasInterpolation() &&
            context.frame_is_interpolated) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.5f, 0.0f, 1.0f),
                "[Zarr] Detections:    None (frame is interpolated)");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
                               "[Zarr] Detections:    None");
        }
    }
}

}  // namespace

void drawFrameDebugStatusPanel(const FrameDebugWindowContext& context,
                               FrameDebugWindowResult& result) {
    drawFrameOverviewSection(context);
    drawDatasetSelectionSection(context, result);
    drawReviewStatusSection(context);
    drawDetectionSummarySection(context);
}
