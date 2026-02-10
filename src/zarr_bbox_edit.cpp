#include "zarr_bbox_edit.h"

void ZarrBBoxEditState::cancelDraw() {
    draw_active = false;
    draw_frame = -1;
    draw_anchor_x = 0.0f;
    draw_anchor_y = 0.0f;
    draw_current_x = 0.0f;
    draw_current_y = 0.0f;
}

void ZarrBBoxEditState::clearSelection() {
    selected_frame = -1;
    selected_box = -1;
    drag_active = false;
    drag_mouse_button = -1;
    drag_offset_x = 0.0f;
    drag_offset_y = 0.0f;
}

void ZarrBBoxEditState::clearFrameEdits(int frame) {
    frame_overrides.erase(frame);
    frame_added_flags.erase(frame);
    frame_manual_flags.erase(frame);
    frame_source_indices.erase(frame);
    frame_source_detection_source.erase(frame);
    frame_source_reason.erase(frame);
    dirty_frames.erase(frame);
    if (selected_frame == frame) {
        clearSelection();
    }
    if (draw_frame == frame) {
        cancelDraw();
    }
}

void ZarrBBoxEditState::clearAll() {
    frame_overrides.clear();
    frame_added_flags.clear();
    frame_manual_flags.clear();
    frame_source_indices.clear();
    frame_source_detection_source.clear();
    frame_source_reason.clear();
    dirty_frames.clear();
    clearSelection();
    cancelDraw();
    draw_mode = false;
}

bool ZarrBBoxEditState::hasFrameOverride(int frame) const {
    return frame_overrides.find(frame) != frame_overrides.end();
}

bool ZarrBBoxEditState::isFrameDirty(int frame) const {
    return dirty_frames.find(frame) != dirty_frames.end();
}

size_t ZarrBBoxEditState::dirtyFrameCount() const {
    return dirty_frames.size();
}

std::vector<LoggedBoundingBox> ZarrBBoxEditState::resolveFrameBoxes(
    int frame,
    const std::vector<LoggedBoundingBox>& loaded_boxes) const {
    auto it = frame_overrides.find(frame);
    if (it == frame_overrides.end()) {
        return loaded_boxes;
    }
    return it->second;
}

std::vector<LoggedBoundingBox>& ZarrBBoxEditState::ensureFrameOverride(
    int frame,
    const std::vector<LoggedBoundingBox>& loaded_boxes,
    const ZarrDetectionLoader::FrameDetections* loaded_details) {
    auto [it, inserted] = frame_overrides.emplace(frame, std::vector<LoggedBoundingBox>{});
    if (inserted) {
        it->second = loaded_boxes;
        frame_added_flags[frame] = std::vector<uint8_t>(loaded_boxes.size(), 0);
        frame_manual_flags[frame] = std::vector<uint8_t>(loaded_boxes.size(), 0);
        auto& source_indices = frame_source_indices[frame];
        auto& source_detection_source = frame_source_detection_source[frame];
        auto& source_reason = frame_source_reason[frame];
        source_indices.assign(loaded_boxes.size(), -1);
        source_detection_source.assign(loaded_boxes.size(), 0);
        source_reason.assign(loaded_boxes.size(), std::string{});
        for (size_t i = 0; i < loaded_boxes.size(); ++i) {
            source_indices[i] = static_cast<int32_t>(i);
            if (loaded_details) {
                if (i < loaded_details->detection_source.size()) {
                    source_detection_source[i] =
                        loaded_details->detection_source[i];
                }
                if (i < loaded_details->detection_reason.size()) {
                    source_reason[i] = loaded_details->detection_reason[i];
                }
            }
        }
    }
    ensureAddedFlags(frame, it->second.size());
    ensureManualFlags(frame, it->second.size());
    ensureSourceMetadata(frame, it->second.size());
    return it->second;
}

std::vector<uint8_t>& ZarrBBoxEditState::ensureAddedFlags(int frame, size_t box_count) {
    auto& flags = frame_added_flags[frame];
    if (flags.size() < box_count) {
        flags.resize(box_count, 0);
    } else if (flags.size() > box_count) {
        flags.resize(box_count);
    }
    return flags;
}

std::vector<uint8_t>& ZarrBBoxEditState::ensureManualFlags(int frame, size_t box_count) {
    auto& flags = frame_manual_flags[frame];
    if (flags.size() < box_count) {
        flags.resize(box_count, 0);
    } else if (flags.size() > box_count) {
        flags.resize(box_count);
    }
    return flags;
}

void ZarrBBoxEditState::ensureSourceMetadata(int frame, size_t box_count) {
    auto& source_indices = frame_source_indices[frame];
    auto& source_detection_source = frame_source_detection_source[frame];
    auto& source_reason = frame_source_reason[frame];
    if (source_indices.size() < box_count) {
        source_indices.resize(box_count, -1);
    } else if (source_indices.size() > box_count) {
        source_indices.resize(box_count);
    }
    if (source_detection_source.size() < box_count) {
        source_detection_source.resize(box_count, 0);
    } else if (source_detection_source.size() > box_count) {
        source_detection_source.resize(box_count);
    }
    if (source_reason.size() < box_count) {
        source_reason.resize(box_count);
    } else if (source_reason.size() > box_count) {
        source_reason.resize(box_count);
    }
}

bool ZarrBBoxEditState::isAddedBox(int frame, int box_index) const {
    if (box_index < 0) {
        return false;
    }
    auto it = frame_added_flags.find(frame);
    if (it == frame_added_flags.end()) {
        return false;
    }
    if (static_cast<size_t>(box_index) >= it->second.size()) {
        return false;
    }
    return it->second[static_cast<size_t>(box_index)] != 0;
}

bool ZarrBBoxEditState::isManualBox(int frame, int box_index) const {
    if (box_index < 0) {
        return false;
    }
    auto it = frame_manual_flags.find(frame);
    if (it == frame_manual_flags.end()) {
        return false;
    }
    if (static_cast<size_t>(box_index) >= it->second.size()) {
        return false;
    }
    return it->second[static_cast<size_t>(box_index)] != 0;
}

int HitTestZarrBoxAtPlotPoint(const std::vector<LoggedBoundingBox>& boxes,
                              double plot_x,
                              double plot_y,
                              float image_height,
                              float tolerance_px) {
    int best_index = -1;
    double best_area = std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < boxes.size(); ++i) {
        const auto& box = boxes[i];
        const double x0 = static_cast<double>(box.x_min);
        const double x1 = static_cast<double>(box.x_min + box.width);
        const double y_top = static_cast<double>(image_height) - static_cast<double>(box.y_min);
        const double y_bottom =
            static_cast<double>(image_height) -
            static_cast<double>(box.y_min + box.height);
        const double y0 = std::min(y_top, y_bottom);
        const double y1 = std::max(y_top, y_bottom);

        if (plot_x < x0 - tolerance_px || plot_x > x1 + tolerance_px ||
            plot_y < y0 - tolerance_px || plot_y > y1 + tolerance_px) {
            continue;
        }

        const double area = std::max(
            1.0,
            static_cast<double>(std::fabs(box.width * box.height)));
        if (area < best_area) {
            best_area = area;
            best_index = static_cast<int>(i);
        }
    }
    return best_index;
}

bool IsPlotPointInsideZarrBox(const LoggedBoundingBox& box,
                              double plot_x,
                              double plot_y,
                              float image_height,
                              float tolerance_px) {
    const double x0 = static_cast<double>(box.x_min);
    const double x1 = static_cast<double>(box.x_min + box.width);
    const double y_top = static_cast<double>(image_height) - static_cast<double>(box.y_min);
    const double y_bottom =
        static_cast<double>(image_height) -
        static_cast<double>(box.y_min + box.height);
    const double y0 = std::min(y_top, y_bottom);
    const double y1 = std::max(y_top, y_bottom);
    return !(plot_x < x0 - tolerance_px || plot_x > x1 + tolerance_px ||
             plot_y < y0 - tolerance_px || plot_y > y1 + tolerance_px);
}
