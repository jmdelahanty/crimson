#pragma once

#include "h5_loader.h"
#include "zarr_loader.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ZarrBBoxEditState {
    bool enabled = true;
    bool allow_edit_while_playing = false;
    std::unordered_map<int, std::vector<LoggedBoundingBox>> frame_overrides;
    std::unordered_map<int, std::vector<uint8_t>> frame_added_flags;
    std::unordered_map<int, std::vector<uint8_t>> frame_manual_flags;
    std::unordered_map<int, std::vector<int32_t>> frame_source_indices;
    std::unordered_map<int, std::vector<uint8_t>> frame_source_detection_source;
    std::unordered_map<int, std::vector<std::string>> frame_source_reason;
    std::unordered_set<int> dirty_frames;
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

    void cancelDraw();
    void clearSelection();
    void clearFrameEdits(int frame);
    void clearAll();
    bool hasFrameOverride(int frame) const;
    bool isFrameDirty(int frame) const;
    size_t dirtyFrameCount() const;

    std::vector<LoggedBoundingBox> resolveFrameBoxes(
        int frame,
        const std::vector<LoggedBoundingBox>& loaded_boxes) const;

    std::vector<LoggedBoundingBox>& ensureFrameOverride(
        int frame,
        const std::vector<LoggedBoundingBox>& loaded_boxes,
        const ZarrDetectionLoader::FrameDetections* loaded_details = nullptr);

    std::vector<uint8_t>& ensureAddedFlags(int frame, size_t box_count);
    std::vector<uint8_t>& ensureManualFlags(int frame, size_t box_count);
    void ensureSourceMetadata(int frame, size_t box_count);
    bool isAddedBox(int frame, int box_index) const;
    bool isManualBox(int frame, int box_index) const;
};

int HitTestZarrBoxAtPlotPoint(const std::vector<LoggedBoundingBox>& boxes,
                              double plot_x,
                              double plot_y,
                              float image_height,
                              float tolerance_px);

bool IsPlotPointInsideZarrBox(const LoggedBoundingBox& box,
                              double plot_x,
                              double plot_y,
                              float image_height,
                              float tolerance_px);
