#include "gui/crop_preview_window.h"

#include "imgui.h"
#include "opencv2/imgproc.hpp"
#include "render.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

namespace {

constexpr auto kCropPreviewPlaybackRefreshInterval =
    std::chrono::milliseconds(100);

struct ResolvedCropPreviewSelection {
    int32_t crop_roi_index = -1;
    std::string crop_roi_source;
    std::optional<RefinedKeypointSelection> selected_keypoint_selection;
};

void clearCropPreviewState(CropPreviewWindowState& state) {
    state.last_roi_index = -1;
    state.displayed_crop_roi_index = -1;
    state.displayed_crop_source_frame = -1;
    state.displayed_crop_source_label.clear();
    state.crop_kp_positions.clear();
    state.crop_kp_labels.clear();
    state.crop_kp_edges.clear();
    state.rotated_kp_positions.clear();
    state.rotated_kp_labels.clear();
    state.rotated_kp_edges.clear();
    state.rotated_valid = false;
    state.stored_heading_valid = false;
    state.arrow_origin_valid = false;
    resetCropKeypointEditorState(state.editor_state);
}

ResolvedCropPreviewSelection resolveCropPreviewSelection(
    const CropPreviewWindowContext& context) {
    ResolvedCropPreviewSelection resolved;

    const auto& movement_frames = context.zarr_loader.getMovementFrameIndices();
    const auto& detection_indices =
        context.zarr_loader.getMovementDetectionIndices();

    if (context.selected_frame == context.current_frame_num &&
        context.selected_box >= 0) {
        auto selection =
            context.refined_keypoint_repo.resolveFrameDetectionSelection(
                static_cast<size_t>(context.current_frame_num),
                static_cast<size_t>(context.selected_box),
                false);
        if (selection.valid) {
            resolved.crop_roi_index = selection.roi_index;
            resolved.crop_roi_source =
                selection.editable ? "selected refined keypoint detection"
                                   : "selected keypoint detection";
            resolved.selected_keypoint_selection = selection;
            return resolved;
        }
    }

    if (!movement_frames.empty() &&
        movement_frames.size() == detection_indices.size()) {
        auto it = std::lower_bound(movement_frames.begin(),
                                   movement_frames.end(),
                                   context.current_frame_num);
        if (it != movement_frames.end() && *it == context.current_frame_num) {
            const size_t idx =
                static_cast<size_t>(std::distance(movement_frames.begin(), it));
            if (idx < detection_indices.size()) {
                resolved.crop_roi_index = detection_indices[idx];
                resolved.crop_roi_source = "movement ROI";
                return resolved;
            }
        }
    }

    const auto& crop_frames = context.crop_image_provider.getCropFrameIndices();
    auto crop_it =
        std::find(crop_frames.begin(), crop_frames.end(), context.current_frame_num);
    if (crop_it != crop_frames.end()) {
        resolved.crop_roi_index =
            static_cast<int32_t>(std::distance(crop_frames.begin(), crop_it));
        resolved.crop_roi_source = "crop frame fallback";
    }

    return resolved;
}

void buildRotatedCropPreview(const CropPreviewWindowContext& context,
                             CropPreviewWindowState& state,
                             const CropImageView& crop_view,
                             int32_t crop_roi_index,
                             const std::optional<RefinedKeypointSelection>&
                                 selected_keypoint_selection) {
    state.rotated_valid = false;
    state.stored_heading_valid = false;
    state.crop_kp_positions.clear();
    state.crop_kp_labels.clear();
    state.crop_kp_edges.clear();
    state.rotated_kp_positions.clear();
    state.rotated_kp_labels.clear();
    state.rotated_kp_edges.clear();
    state.arrow_origin_valid = false;

    if (!context.zarr_loader.hasKeypointData()) {
        return;
    }

    auto det = context.zarr_loader.getRawDetections(
        static_cast<size_t>(context.current_frame_num), false, true);
    size_t matched = SIZE_MAX;
    std::optional<RefinedKeypointSelection> matched_keypoint_selection;
    if (selected_keypoint_selection.has_value() &&
        selected_keypoint_selection->roi_index == crop_roi_index &&
        selected_keypoint_selection->detection_index < det.boxes.size()) {
        matched = selected_keypoint_selection->detection_index;
        matched_keypoint_selection = selected_keypoint_selection;
    }
    if (matched == SIZE_MAX) {
        for (size_t di = 0; di < det.eye_masks.size(); ++di) {
            if (det.eye_masks[di].roi_index == crop_roi_index) {
                matched = di;
                matched_keypoint_selection =
                    context.refined_keypoint_repo.resolveFrameDetectionSelection(
                        static_cast<size_t>(context.current_frame_num), di, false);
                break;
            }
        }
    }
    if (matched == SIZE_MAX && det.has_keypoints) {
        const size_t keypoint_detection_count =
            std::min(det.keypoints_pixels.size(), det.boxes.size());
        for (size_t di = 0; di < keypoint_detection_count; ++di) {
            auto candidate =
                context.refined_keypoint_repo.resolveFrameDetectionSelection(
                    static_cast<size_t>(context.current_frame_num), di, false);
            if (candidate.valid && candidate.roi_index == crop_roi_index) {
                matched = di;
                matched_keypoint_selection = std::move(candidate);
                break;
            }
        }
    }

    if (matched == SIZE_MAX || matched >= det.headings_deg.size() ||
        matched >= det.heading_valid.size() || !det.heading_valid[matched]) {
        return;
    }

    state.stored_heading_deg = det.headings_deg[matched];
    state.stored_heading_valid = true;

    const float angle = -state.stored_heading_deg;
    const int width = static_cast<int>(crop_view.width);
    const int height = static_cast<int>(crop_view.height);
    cv::Point2f center(width / 2.0f, height / 2.0f);
    cv::Mat rot_mat = cv::getRotationMatrix2D(center, angle, 1.0);
    cv::Rect2f bbox =
        cv::RotatedRect(center,
                        cv::Size2f(static_cast<float>(width),
                                   static_cast<float>(height)),
                        angle)
            .boundingRect2f();
    rot_mat.at<double>(0, 2) += bbox.width / 2.0 - center.x;
    rot_mat.at<double>(1, 2) += bbox.height / 2.0 - center.y;
    int new_width = static_cast<int>(std::ceil(bbox.width));
    int new_height = static_cast<int>(std::ceil(bbox.height));

    cv::Mat src(height, width, CV_8UC4, state.crop_rgba_buffer.data());
    cv::Mat dst;
    cv::warpAffine(src,
                   dst,
                   rot_mat,
                   cv::Size(new_width, new_height),
                   cv::INTER_LINEAR,
                   cv::BORDER_CONSTANT,
                   cv::Scalar(0, 0, 0, 0));

    const float radius = std::min(width, height) / 2.0f;
    cv::Point2f new_center(new_width / 2.0f, new_height / 2.0f);
    for (int row = 0; row < new_height; ++row) {
        uint8_t* ptr = dst.ptr<uint8_t>(row);
        for (int col = 0; col < new_width; ++col) {
            const float dx = col + 0.5f - new_center.x;
            const float dy = row + 0.5f - new_center.y;
            if (dx * dx + dy * dy > radius * radius) {
                ptr[col * 4 + 0] = 0;
                ptr[col * 4 + 1] = 0;
                ptr[col * 4 + 2] = 0;
                ptr[col * 4 + 3] = 0;
            }
        }
    }

    const int crop_side = std::min(width, height);
    const int crop_x = new_width / 2 - crop_side / 2;
    const int crop_y = new_height / 2 - crop_side / 2;
    dst = dst(cv::Rect(crop_x, crop_y, crop_side, crop_side)).clone();
    new_width = crop_side;
    new_height = crop_side;

    state.rotated_rgba_buffer.assign(dst.data, dst.data + dst.total() * 4);
    state.rotated_width = static_cast<unsigned int>(new_width);
    state.rotated_height = static_cast<unsigned int>(new_height);
    if (state.rotated_crop_texture == 0) {
        create_texture(&state.rotated_crop_texture);
    }
    upload_texture(&state.rotated_crop_texture,
                   state.rotated_rgba_buffer.data(),
                   state.rotated_width,
                   state.rotated_height);
    state.rotated_valid = true;

    state.rotated_kp_edges = det.skeleton_edges;
    state.crop_kp_edges = det.skeleton_edges;
    if (!det.has_keypoints || matched >= det.keypoints_pixels.size()) {
        return;
    }

    const auto& keypoints = det.keypoints_pixels[matched];
    float offset_x = NAN;
    float offset_y = NAN;
    if (matched < det.eye_masks.size() &&
        std::isfinite(det.eye_masks[matched].offset_x) &&
        std::isfinite(det.eye_masks[matched].offset_y)) {
        offset_x = det.eye_masks[matched].offset_x;
        offset_y = det.eye_masks[matched].offset_y;
    } else if (matched_keypoint_selection.has_value() &&
               matched_keypoint_selection->roi_metadata.has_crop_metadata) {
        offset_x = matched_keypoint_selection->roi_metadata.offset_x;
        offset_y = matched_keypoint_selection->roi_metadata.offset_y;
    }
    if (!std::isfinite(offset_x) || !std::isfinite(offset_y)) {
        return;
    }

    const double r0 = rot_mat.at<double>(0, 0);
    const double r1 = rot_mat.at<double>(0, 1);
    const double r2 = rot_mat.at<double>(0, 2);
    const double r3 = rot_mat.at<double>(1, 0);
    const double r4 = rot_mat.at<double>(1, 1);
    const double r5 = rot_mat.at<double>(1, 2);

    float left_x = NAN;
    float left_y = NAN;
    float right_x = NAN;
    float right_y = NAN;
    float left_rx = NAN;
    float left_ry = NAN;
    float right_rx = NAN;
    float right_ry = NAN;

    for (size_t ki = 0; ki < keypoints.size(); ++ki) {
        if (!std::isfinite(keypoints[ki][0]) || !std::isfinite(keypoints[ki][1])) {
            state.crop_kp_positions.push_back({NAN, NAN});
            state.rotated_kp_positions.push_back({NAN, NAN});
        } else {
            const float px = keypoints[ki][0] - offset_x;
            const float py = keypoints[ki][1] - offset_y;
            state.crop_kp_positions.push_back({px, py});
            const float rx = static_cast<float>(r0 * px + r1 * py + r2) - crop_x;
            const float ry = static_cast<float>(r3 * px + r4 * py + r5) - crop_y;
            state.rotated_kp_positions.push_back({rx, ry});
        }

        const std::string label =
            ki < det.keypoint_labels.size() ? det.keypoint_labels[ki] : "";
        state.crop_kp_labels.push_back(label);
        state.rotated_kp_labels.push_back(label);

        const bool is_left = label.find("left") != std::string::npos;
        const bool is_right = label.find("right") != std::string::npos;
        if ((is_left || is_right) &&
            std::isfinite(keypoints[ki][0]) && std::isfinite(keypoints[ki][1])) {
            const float px = keypoints[ki][0] - offset_x;
            const float py = keypoints[ki][1] - offset_y;
            if (is_left) {
                left_x = px;
                left_y = py;
                if (ki < state.rotated_kp_positions.size()) {
                    left_rx = state.rotated_kp_positions[ki][0];
                    left_ry = state.rotated_kp_positions[ki][1];
                }
            }
            if (is_right) {
                right_x = px;
                right_y = py;
                if (ki < state.rotated_kp_positions.size()) {
                    right_rx = state.rotated_kp_positions[ki][0];
                    right_ry = state.rotated_kp_positions[ki][1];
                }
            }
        }
    }

    if (std::isfinite(left_x) && std::isfinite(right_x)) {
        state.arrow_origin_crop = {(left_x + right_x) / 2.0f,
                                   (left_y + right_y) / 2.0f};
        state.arrow_origin_rotated = {(left_rx + right_rx) / 2.0f,
                                      (left_ry + right_ry) / 2.0f};
        state.arrow_origin_valid = true;
    }
}

bool refreshCropPreview(const CropPreviewWindowContext& context,
                        CropPreviewWindowState& state,
                        int32_t crop_roi_index,
                        const std::string& crop_roi_source,
                        const std::optional<RefinedKeypointSelection>&
                            selected_keypoint_selection) {
    const auto now_steady = std::chrono::steady_clock::now();
    const bool playback_refresh_due =
        !context.play_video ||
        state.last_crop_preview_refresh_time ==
            std::chrono::steady_clock::time_point{} ||
        (now_steady - state.last_crop_preview_refresh_time) >=
            kCropPreviewPlaybackRefreshInterval;
    const bool frame_changed =
        context.current_frame_num != state.last_crop_preview_source_frame;
    const bool should_refresh_preview =
        !context.play_video || !frame_changed || playback_refresh_due;
    if (!should_refresh_preview) {
        return state.crop_texture != 0 && state.displayed_crop_roi_index >= 0 &&
               state.last_width > 0 && state.last_height > 0;
    }

    CropImageView crop_view;
    if (!context.crop_image_provider.getCropImageForIndex(crop_roi_index,
                                                          crop_view)) {
        return false;
    }

    bool needs_upload = crop_roi_index != state.last_roi_index ||
                        crop_view.width != state.last_width ||
                        crop_view.height != state.last_height ||
                        crop_view.channels != state.last_channels ||
                        frame_changed;
    if (state.crop_texture == 0) {
        create_texture(&state.crop_texture);
        needs_upload = true;
    }

    if (needs_upload) {
        const size_t pixel_count = crop_view.width * crop_view.height;
        state.crop_rgba_buffer.resize(pixel_count * 4);
        const uint8_t* src = crop_view.data;
        uint8_t* dst = state.crop_rgba_buffer.data();
        if (crop_view.channels == 4) {
            std::memcpy(dst, src, pixel_count * 4);
        } else if (crop_view.channels == 3) {
            for (size_t p = 0; p < pixel_count; ++p) {
                dst[4 * p + 0] = src[3 * p + 0];
                dst[4 * p + 1] = src[3 * p + 1];
                dst[4 * p + 2] = src[3 * p + 2];
                dst[4 * p + 3] = 255;
            }
        } else {
            for (size_t p = 0; p < pixel_count; ++p) {
                const uint8_t v = src[p];
                dst[4 * p + 0] = v;
                dst[4 * p + 1] = v;
                dst[4 * p + 2] = v;
                dst[4 * p + 3] = 255;
            }
        }

        upload_texture(&state.crop_texture,
                       state.crop_rgba_buffer.data(),
                       static_cast<unsigned int>(crop_view.width),
                       static_cast<unsigned int>(crop_view.height));
        state.last_roi_index = crop_roi_index;
        state.last_width = crop_view.width;
        state.last_height = crop_view.height;
        state.last_channels = crop_view.channels;
        state.displayed_crop_roi_index = crop_roi_index;
        state.displayed_crop_source_frame = context.current_frame_num;
        state.displayed_crop_source_label = crop_roi_source;
        state.last_crop_preview_source_frame = context.current_frame_num;
        state.last_crop_preview_refresh_time = now_steady;

        buildRotatedCropPreview(
            context, state, crop_view, crop_roi_index, selected_keypoint_selection);
    }

    return state.crop_texture != 0 && state.displayed_crop_roi_index >= 0 &&
           state.last_width > 0 && state.last_height > 0;
}

}  // namespace

CropPreviewWindowResult drawCropPreviewWindow(const CropPreviewWindowContext& context,
                                             CropPreviewWindowState& state) {
    CropPreviewWindowResult result;

    ImGui::SetNextWindowSize(ImVec2(300.0f, 300.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(120.0f, 120.0f),
                                        ImVec2(420.0f, 700.0f));
    if (!ImGui::Begin("Crop Preview")) {
        ImGui::End();
        return result;
    }

    const auto resolved = resolveCropPreviewSelection(context);
    result.selected_keypoint_selection = resolved.selected_keypoint_selection;
    if (resolved.crop_roi_index < 0) {
        ImGui::TextUnformatted("No crop available for current frame.");
        clearCropPreviewState(state);
        ImGui::End();
        return result;
    }

    if (!refreshCropPreview(context,
                            state,
                            resolved.crop_roi_index,
                            resolved.crop_roi_source,
                            resolved.selected_keypoint_selection)) {
        ImGui::TextUnformatted("No crop available for current frame.");
        clearCropPreviewState(state);
        ImGui::End();
        return result;
    }

    CropKeypointEditorContext editor_context;
    editor_context.selection = resolved.selected_keypoint_selection.has_value()
                                   ? &*resolved.selected_keypoint_selection
                                   : nullptr;
    editor_context.displayed_crop_roi_index = state.displayed_crop_roi_index;
    editor_context.displayed_crop_source_frame = state.displayed_crop_source_frame;
    editor_context.displayed_crop_source_label = &state.displayed_crop_source_label;
    editor_context.play_video = context.play_video;
    editor_context.rotated_valid = state.rotated_valid;
    editor_context.stored_heading_valid = state.stored_heading_valid;
    editor_context.stored_heading_deg = state.stored_heading_deg;
    editor_context.source_positions = &state.crop_kp_positions;
    editor_context.labels = &state.crop_kp_labels;
    editor_context.edges = &state.crop_kp_edges;
    editor_context.base_arrow_origin = state.arrow_origin_crop;
    editor_context.base_arrow_origin_valid = state.arrow_origin_valid;
    editor_context.status_message = context.manual_write_status;

    CropKeypointPreviewPanelContext preview_context;
    preview_context.crop_texture_id = state.crop_texture;
    preview_context.crop_width = state.last_width;
    preview_context.crop_height = state.last_height;
    preview_context.displayed_crop_roi_index = state.displayed_crop_roi_index;
    preview_context.displayed_crop_source_frame = state.displayed_crop_source_frame;
    preview_context.current_frame_num = context.current_frame_num;
    preview_context.displayed_crop_source_label = &state.displayed_crop_source_label;
    preview_context.play_video = context.play_video;
    preview_context.editor_context = editor_context;
    preview_context.rotated.valid = state.rotated_valid;
    preview_context.rotated.texture_id = state.rotated_crop_texture;
    preview_context.rotated.width = state.rotated_width;
    preview_context.rotated.height = state.rotated_height;
    preview_context.rotated.positions = &state.rotated_kp_positions;
    preview_context.rotated.labels = &state.rotated_kp_labels;
    preview_context.rotated.edges = &state.rotated_kp_edges;
    preview_context.rotated.arrow_origin = state.arrow_origin_rotated;
    preview_context.rotated.arrow_origin_valid = state.arrow_origin_valid;

    const auto preview_result = drawCropKeypointPreviewPanel(
        preview_context, state.preview_ui_state, state.editor_state);
    result.editor_action = preview_result.editor_action;

    ImGui::End();
    return result;
}
