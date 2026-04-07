#include "gui/frame_debug_status_panel.h"

#include "imgui.h"

#include <algorithm>

namespace {

void drawFrameOverviewSection(const FrameDebugWindowContext& context) {
    ImGui::Text("Inspecting Frame: %d", context.current_frame_num);
    ImGui::Text("Display target frame: %d", context.display_target_frame);
    ImGui::Text("Slider frame: %d", context.slider_frame);
    if (context.frame_sync_valid_slots >= 0 &&
        context.frame_sync_empty_slots >= 0) {
        ImGui::Text("Buffer frames: valid=%d empty_remaining=%d total=%u",
                    context.frame_sync_valid_slots,
                    context.frame_sync_empty_slots,
                    context.scene_buffer_size);
    }
    if (context.frame_sync_recording_remaining >= 0 &&
        context.frame_sync_recording_total > 0) {
        ImGui::Text("Recording decode: latest=%d remaining=%d total=%d",
                    context.frame_sync_latest_decoded,
                    context.frame_sync_recording_remaining,
                    context.frame_sync_recording_total);
    }
    if (!context.frame_sync_debug_line.empty()) {
        ImGui::TextWrapped("Frame sync: %s",
                           context.frame_sync_debug_line.c_str());
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
    if (!context.zarr_loader.hasReviewStatus()) {
        return;
    }

    const auto& review_state = context.zarr_loader.getReviewState();
    ImVec4 status_color = (review_state == "approved")
                              ? ImVec4(0.2f, 0.9f, 0.2f, 1.0f)
                          : (review_state == "rejected")
                              ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f)
                              : ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
    ImGui::TextColored(status_color, "Review: %s", review_state.c_str());
    ImGui::SameLine();
    ImGui::Text("| Use: %s | Method: %s",
                context.zarr_loader.getReviewIntendedUse().c_str(),
                context.zarr_loader.getReviewMethod().c_str());
    if (!context.zarr_loader.getReviewTimestamp().empty()) {
        ImGui::Text("  Reviewed: %s",
                    context.zarr_loader.getReviewTimestamp().c_str());
    }
    if (!context.zarr_loader.getReviewReviewer().empty()) {
        ImGui::Text("  Reviewer: %s",
                    context.zarr_loader.getReviewReviewer().c_str());
    }
    if (!context.zarr_loader.getReviewNotes().empty()) {
        ImGui::Text("  Notes: %s",
                    context.zarr_loader.getReviewNotes().c_str());
    }
}

void drawStimulusAlignmentSection(const FrameDebugWindowContext& context) {
    if (!context.zarr_loader.hasStimulusAlignment()) {
        return;
    }

    ImGui::Separator();
    ImGui::Text("Stimulus Alignment:");
    if (context.zarr_loader.hasStimulusFrameMapping()) {
        ImGui::Text("  Mapping variant: %s",
                    context.zarr_loader.hasCorrectedStimulusFrameMapping()
                        ? "corrected"
                        : "legacy");
        if (auto current_stimulus_frame = context.zarr_loader
                                              .getStimulusFrameForCameraFrame(
                                                  context.current_frame_num)) {
            ImGui::Text("  Current stimulus frame: %d", *current_stimulus_frame);
        } else {
            ImGui::Text("  Current stimulus frame: (not mapped)");
        }
        if (auto metadata_index =
                context.zarr_loader.getStimulusMetadataIndexForCameraFrame(
                    context.current_frame_num)) {
            ImGui::Text("  Frame metadata index: %d", *metadata_index);
        }
        if (auto first_cam =
                context.zarr_loader.getFirstCameraFrameWithStimulus()) {
            if (auto first_stim =
                    context.zarr_loader.getFirstStimulusFrameNumber()) {
                ImGui::Text("  First mapped camera frame: %d -> Stim %d",
                            *first_cam,
                            *first_stim);
            } else {
                ImGui::Text("  First mapped camera frame: %d", *first_cam);
            }
        }
        ImGui::Text("  Camera frame offset: %lld",
                    static_cast<long long>(
                        context.zarr_loader.getStimulusCameraFrameOffset()));
    } else {
        ImGui::Text("  Mapping data not available");
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
    drawStimulusAlignmentSection(context);
    drawDetectionSummarySection(context);
}
