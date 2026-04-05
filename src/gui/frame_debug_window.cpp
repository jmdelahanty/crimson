#include "gui/frame_debug_window.h"

#include "imgui.h"

#include <algorithm>
#include <sstream>

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

    if (context.plot_keypoints_flag) {
        if (context.manual_keypoints_present) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f),
                               "[Manual] Keypoints:   Found");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
                               "[Manual] Keypoints:   None");
        }
    }
}

void drawReviewStatusSection(const FrameDebugWindowContext& context) {
    if (context.zarr_loader.hasReviewStatus()) {
        const auto& review_state = context.zarr_loader.getReviewState();
        ImVec4 status_color = (review_state == "approved")
                                  ? ImVec4(0.2f, 0.9f, 0.2f, 1.0f)
                              : (review_state == "rejected")
                                  ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f)
                                  : ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
        ImGui::TextColored(status_color,
                           "Review: %s",
                           review_state.c_str());
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

void drawDetectionSummarySection(const FrameDebugWindowContext& context) {
    if (!context.zarr_loader.hasDetectionData()) {
        ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.25f, 1.0f),
                           "[Zarr] Detection runs: unavailable (metadata/stimulus-only mode)");
        return;
    }

    if (!context.zarr_boxes.empty()) {
        if (context.frame_is_interpolated && context.dataset_has_synthetic_boxes) {
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

void drawDecodeDebugSection(const FrameDebugWindowContext& context,
                            FrameDebugWindowResult& result) {
    ImGui::Separator();
    ImGui::Text("Decode Debug:");
    if (ImGui::Button("Dump Decode Buffers")) {
        result.request_dump_decode_buffers = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Random Seek + Dump")) {
        result.request_random_seek_dump = true;
    }
    ImGui::TextWrapped("  Output dir: CRIMSON_BUFFER_DUMP_DIR (default %s)",
                       context.default_buffer_dump_root.c_str());
    if (!context.decode_debug_status.empty()) {
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f),
                           "%s",
                           context.decode_debug_status.c_str());
    }
    ImGui::TextWrapped(
        "  Box colors: clean=blue, interpolated=orange, manual=teal");
}

void drawBBoxEditSection(const FrameDebugWindowContext& context,
                         FrameDebugWindowState& state,
                         FrameDebugWindowResult& result) {
    ImGui::Separator();
    ImGui::Text("BBox Edit (in-memory):");
    if (!context.dataset_allows_bbox_edit) {
        ImGui::TextColored(
            ImVec4(0.95f, 0.65f, 0.25f, 1.0f),
            "Read-only: Zarr Raw Detect (read-only) cannot be edited.");
    }
    ImGui::BeginDisabled(!context.dataset_allows_bbox_edit);
    ImGui::Checkbox("Enable bbox drag editing", &context.bbox_edit_state.enabled);
    bool draw_mode_enabled = context.bbox_edit_state.draw_mode;
    if (ImGui::Checkbox("Draw new boxes (N)", &draw_mode_enabled)) {
        context.bbox_edit_state.draw_mode = draw_mode_enabled;
        if (!draw_mode_enabled) {
            context.bbox_edit_state.cancelDraw();
        } else {
            context.bbox_edit_state.clearSelection();
        }
    }
    if (context.play_video && context.bbox_edit_state.enabled &&
        !context.bbox_edit_state.allow_edit_while_playing) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                           "Pause playback to drag boxes.");
    }
    if (context.bbox_edit_state.draw_mode) {
        ImGui::Text(
            "  Draw mode active: Ctrl + left-drag to place; Esc cancels.");
    }
    ImGui::Text("  Cycle/select bbox: B (Shift+B reverse)");
    ImGui::Text("  Move selected bbox: Ctrl + left-drag");
    ImGui::Text("  Pan while editing: Shift + drag");
    ImGui::Text("  Delete selected bbox: Del");
    ImGui::Text("  Current frame: %s",
                context.frame_has_bbox_edits ? "edited (unsaved)" : "unchanged");
    ImGui::Text("  Pending edited frames: %zu",
                context.bbox_edit_state.dirtyFrameCount());
    if (ImGui::Button("Reset Frame BBox Edits (Shift+R)")) {
        result.request_reset_frame_bbox_edits = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear BBox Selection (Esc)")) {
        result.request_clear_bbox_selection = true;
    }
    if (ImGui::Button("Build Manual Payload Preview")) {
        result.request_build_manual_payload_preview = true;
    }
    const char* intended_use_items[] = {"full_recording", "training"};
    ImGui::Combo("Intended Use##manual_write",
                 &state.manual_write_intended_use,
                 intended_use_items,
                 IM_ARRAYSIZE(intended_use_items));
    const char* review_state_items[] = {
        "approved", "needs_review", "pending", "rejected"};
    ImGui::Combo("Review State##manual_write",
                 &state.manual_write_review_state,
                 review_state_items,
                 IM_ARRAYSIZE(review_state_items));
    if (ImGui::Button("Write Manual Payload to Zarr")) {
        result.request_write_manual_payload = true;
    }
    ImGui::TextWrapped(
        "  Writes refined_detect_runs/<latest>/manual and updates manual pointers/status.");
    ImGui::EndDisabled();

    if (!context.bbox_payload_status.empty()) {
        ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f),
                           "%s",
                           context.bbox_payload_status.c_str());
    }
}

void drawOverlaySection(const FrameDebugWindowContext& context,
                        FrameDebugWindowResult& result) {
    result.show_keypoint_markers = context.show_keypoint_markers;
    result.show_heading_arrows = context.show_heading_arrows;
    result.show_eye_masks = context.show_eye_masks;

    if (context.zarr_loader.hasKeypointData() ||
        context.zarr_loader.hasHeadingData()) {
        ImGui::Separator();
        if (context.zarr_loader.hasKeypointData()) {
            ImGui::Text("Keypoint Overlay:");
            ImGui::Checkbox("Show keypoint markers",
                            &result.show_keypoint_markers);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Overlay swim bladder and eye keypoints on the video frame.");
            }
            if (!context.zarr_loader.getKeypointsRunName().empty()) {
                ImGui::Text("  Keypoints run: %s (%s)",
                            context.zarr_loader.getKeypointsRunName().c_str(),
                            context.zarr_loader.isRefinedKeypoints()
                                ? "refined"
                                : "raw");
            }
            if (context.detection_details != nullptr &&
                context.detection_details->is_refined_keypoints &&
                !context.detection_details->keypoint_usable.empty()) {
                size_t usable_count = 0;
                size_t flip_count = 0;
                size_t det_count =
                    context.detection_details->keypoint_usable.size();
                for (size_t qi = 0; qi < det_count; ++qi) {
                    if (context.detection_details->keypoint_usable[qi] != 0) {
                        usable_count++;
                    }
                    if (qi <
                            context.detection_details->keypoint_flip_corrected
                                .size() &&
                        context.detection_details->keypoint_flip_corrected[qi] !=
                            0) {
                        flip_count++;
                    }
                }
                ImGui::Text("  Quality: %zu/%zu usable (%zu flip-corrected)",
                            usable_count,
                            det_count,
                            flip_count);
                if (!context.detection_details->keypoint_reason.empty() &&
                    !context.detection_details->keypoint_reason[0].empty()) {
                    ImGui::TextWrapped(
                        "  Reason: %s",
                        context.detection_details->keypoint_reason[0].c_str());
                }
            }
            if (context.detection_details != nullptr &&
                context.detection_details->keypoints_per_detection > 0 &&
                !context.detection_details->keypoint_labels.empty()) {
                std::string label_list;
                for (size_t i = 0;
                     i < context.detection_details->keypoint_labels.size();
                     ++i) {
                    if (i > 0) {
                        label_list += ", ";
                    }
                    label_list += context.detection_details->keypoint_labels[i];
                    if (label_list.size() > 72 &&
                        i + 1 <
                            context.detection_details->keypoint_labels.size()) {
                        label_list += "...";
                        break;
                    }
                }
                if (!label_list.empty()) {
                    ImGui::TextWrapped("  Labels: %s", label_list.c_str());
                }
            }
            if (context.zarr_loader.activeDatasetHasSyntheticDetections()) {
                ImGui::TextWrapped(
                    "Synthetic detections are present; interpolated boxes draw with hollow keypoint markers.");
            }
        }

        if (context.zarr_loader.hasHeadingData()) {
            if (context.zarr_loader.hasKeypointData()) {
                ImGui::Spacing();
            }
            ImGui::Text("Heading Overlay:");
            ImGui::Checkbox("Show heading arrows", &result.show_heading_arrows);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Visualize swim bladder headings from the keypoints run.");
            }
            if (!context.zarr_loader.getKeypointsRunName().empty() &&
                !context.zarr_loader.hasKeypointData()) {
                ImGui::Text("  Keypoints run: %s (%s)",
                            context.zarr_loader.getKeypointsRunName().c_str(),
                            context.zarr_loader.isRefinedKeypoints()
                                ? "refined"
                                : "raw");
            }
            if (context.zarr_loader.activeDatasetHasSyntheticDetections()) {
                ImGui::TextWrapped(
                    "Synthetic detections are present; arrows render only for real boxes.");
            }
        }
    }

    if (context.zarr_loader.hasInterpolation()) {
        ImGui::Separator();
        ImGui::Text("Interpolation Status:");
        ImGui::Text("  Current frame interpolated: %s",
                    context.frame_is_interpolated ? "Yes" : "No");
        ImGui::Text("  Dataset uses interpolation: %s",
                    context.zarr_loader.activeDatasetHasSyntheticDetections()
                        ? "Yes"
                        : "No");
        ImGui::Text("  Method: %s",
                    context.zarr_loader.getInterpolationMethod().c_str());
    }

    if (context.zarr_loader.hasEyeMasks()) {
        ImGui::Separator();
        ImGui::Text("Eye Mask Overlay:");
        ImGui::Checkbox("Show refined eye masks", &result.show_eye_masks);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Visualize refined eye masks as semi-transparent overlays.");
        }
        if (!context.zarr_loader.getEyeMaskRunName().empty()) {
            ImGui::Text("  Eye mask run: %s",
                        context.zarr_loader.getEyeMaskRunName().c_str());
        }
        if (context.zarr_loader.activeDatasetHasSyntheticDetections()) {
            ImGui::TextWrapped(
                "Synthetic detections are present; masks are skipped for interpolated boxes.");
        }
    }
}

}  // namespace

FrameDebugWindowResult drawFrameDebugWindow(const FrameDebugWindowContext& context,
                                            FrameDebugWindowState& state) {
    FrameDebugWindowResult result;
    result.review_frame_filters = context.review_frame_filters;
    result.show_keypoint_markers = context.show_keypoint_markers;
    result.show_heading_arrows = context.show_heading_arrows;
    result.show_eye_masks = context.show_eye_masks;

    if (!ImGui::Begin("Frame Debug")) {
        ImGui::End();
        return result;
    }

    drawFrameOverviewSection(context);
    if (context.zarr_loaded) {
        drawDatasetSelectionSection(context, result);
        drawReviewStatusSection(context);
        drawStimulusAlignmentSection(context);
        drawDetectionSummarySection(context);
        drawReviewNavigationSection(context, result);
        drawDecodeDebugSection(context, result);
        drawBBoxEditSection(context, state, result);
        drawOverlaySection(context, result);
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                           "[Zarr] Detections:    Not loaded");
    }

    ImGui::End();
    return result;
}
