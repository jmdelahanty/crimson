#pragma once

#include "subject_mask_writeback_client.h"
#include "zarr_loader.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SubjectMaskEditTarget {
    std::string zarr_path;
    std::string refined_run;
    int32_t roi_index = -1;
    std::string component_name;
    size_t channel_index = 0;
    size_t rows = 0;
    size_t cols = 0;
};

struct SubjectMaskRoiPoint {
    int row = -1;
    int col = -1;
};

enum class SubjectMaskPreviewTool {
    Brush = 0,
    Lasso = 1,
    Polygon = 2,
};

struct SubjectMaskBrushState {
    bool enabled = false;
    SubjectMaskPreviewTool tool = SubjectMaskPreviewTool::Brush;
    bool erase = false;
    int radius_px = 6;
    bool brush_hover_valid = false;
    SubjectMaskRoiPoint brush_hover;
    bool stroke_active = false;
    int last_row = -1;
    int last_col = -1;
    bool lasso_active = false;
    std::vector<SubjectMaskRoiPoint> lasso_points;
    std::vector<SubjectMaskRoiPoint> polygon_points;
    bool polygon_hover_valid = false;
    SubjectMaskRoiPoint polygon_hover;
};

class SubjectMaskEditSession {
public:
    bool start(const SubjectMaskEditTarget& target,
               const std::vector<uint8_t>& original_mask,
               std::string* error_message = nullptr);
    bool startFromLoadedRow(const ZarrDetectionLoader& loader,
                            size_t roi_index,
                            const std::string& component_name,
                            std::string* error_message = nullptr);
    void clear();
    bool resetPreview();
    bool replacePreviewMask(const std::vector<uint8_t>& mask,
                            std::string* error_message = nullptr);
    bool setPixel(size_t row, size_t col, uint8_t value);
    bool stampDisk(int center_row,
                   int center_col,
                   int radius_px,
                   uint8_t value);
    bool stampLine(int row0,
                   int col0,
                   int row1,
                   int col1,
                   int radius_px,
                   uint8_t value);
    bool fillPolygon(const std::vector<SubjectMaskRoiPoint>& points,
                     uint8_t value);

    bool active() const { return active_; }
    bool dirty() const { return dirty_; }
    const SubjectMaskEditTarget& target() const { return target_; }
    const std::vector<uint8_t>& originalMask() const { return original_mask_; }
    const std::vector<uint8_t>& previewMask() const { return preview_mask_; }
    uint64_t previewRevision() const { return preview_revision_; }

    SubjectMaskWritebackRequest buildWritebackRequest(
        std::string reason = "crimson_refined_subject_mask_edit") const;

private:
    bool active_ = false;
    bool dirty_ = false;
    SubjectMaskEditTarget target_;
    std::vector<uint8_t> original_mask_;
    std::vector<uint8_t> preview_mask_;
    uint64_t preview_revision_ = 0;
};
