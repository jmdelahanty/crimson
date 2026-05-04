#include "subject_mask_edit_session.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace {

struct BrushSpan {
    int dy = 0;
    int x_min = 0;
    int x_max = 0;
};

std::vector<uint8_t> binarize(const std::vector<uint8_t>& mask) {
    std::vector<uint8_t> out(mask.size(), 0);
    std::transform(mask.begin(), mask.end(), out.begin(),
                   [](uint8_t value) -> uint8_t {
                       return value != 0 ? 1 : 0;
                   });
    return out;
}

bool validateTargetAndMask(const SubjectMaskEditTarget& target,
                           const std::vector<uint8_t>& mask,
                           std::string* error_message) {
    if (target.zarr_path.empty()) {
        if (error_message != nullptr) {
            *error_message = "No Zarr archive path available.";
        }
        return false;
    }
    if (target.refined_run.empty()) {
        if (error_message != nullptr) {
            *error_message = "No refined subject-mask run available.";
        }
        return false;
    }
    if (target.roi_index < 0) {
        if (error_message != nullptr) {
            *error_message = "No valid ROI row selected.";
        }
        return false;
    }
    if (target.component_name.empty()) {
        if (error_message != nullptr) {
            *error_message = "No subject-mask component selected.";
        }
        return false;
    }
    if (target.rows == 0 || target.cols == 0) {
        if (error_message != nullptr) {
            *error_message = "Selected subject-mask component has empty dimensions.";
        }
        return false;
    }
    const size_t expected_size = target.rows * target.cols;
    if (mask.size() != expected_size) {
        if (error_message != nullptr) {
            std::ostringstream oss;
            oss << "Selected subject-mask payload size mismatch: expected "
                << expected_size << " bytes, got " << mask.size() << ".";
            *error_message = oss.str();
        }
        return false;
    }
    return true;
}

const std::vector<BrushSpan>& diskFootprint(int radius_px) {
    static std::unordered_map<int, std::vector<BrushSpan>> cache;
    const int radius = std::max(0, radius_px);
    auto found = cache.find(radius);
    if (found != cache.end()) {
        return found->second;
    }

    std::vector<BrushSpan> spans;
    spans.reserve(static_cast<size_t>(radius * 2 + 1));
    const int radius_sq = radius * radius;
    for (int dy = -radius; dy <= radius; ++dy) {
        int x_extent = 0;
        while ((x_extent + 1) * (x_extent + 1) + dy * dy <= radius_sq) {
            ++x_extent;
        }
        spans.push_back(BrushSpan{dy, -x_extent, x_extent});
    }
    auto inserted = cache.emplace(radius, std::move(spans));
    return inserted.first->second;
}

bool stampDiskInto(std::vector<uint8_t>& mask,
                   size_t rows,
                   size_t cols,
                   int center_row,
                   int center_col,
                   int radius_px,
                   uint8_t binary_value) {
    if (rows == 0 || cols == 0) {
        return false;
    }
    const int row_count = static_cast<int>(rows);
    const int col_count = static_cast<int>(cols);
    bool changed = false;
    for (const BrushSpan& span : diskFootprint(radius_px)) {
        const int y = center_row + span.dy;
        if (y < 0 || y >= row_count) {
            continue;
        }
        const int x0 = std::max(0, center_col + span.x_min);
        const int x1 = std::min(col_count - 1, center_col + span.x_max);
        if (x0 > x1) {
            continue;
        }
        const size_t row_offset = static_cast<size_t>(y) * cols;
        for (int x = x0; x <= x1; ++x) {
            uint8_t& pixel = mask[row_offset + static_cast<size_t>(x)];
            if (pixel != binary_value) {
                pixel = binary_value;
                changed = true;
            }
        }
    }
    return changed;
}

bool fillPolygonInto(std::vector<uint8_t>& mask,
                     size_t rows,
                     size_t cols,
                     const std::vector<SubjectMaskRoiPoint>& points,
                     uint8_t binary_value) {
    if (rows == 0 || cols == 0 || points.size() < 3) {
        return false;
    }

    const int row_count = static_cast<int>(rows);
    const int col_count = static_cast<int>(cols);
    bool changed = false;
    std::vector<double> intersections;
    intersections.reserve(points.size());

    for (int row = 0; row < row_count; ++row) {
        intersections.clear();
        const double scan_y = static_cast<double>(row) + 0.5;
        for (size_t i = 0; i < points.size(); ++i) {
            const SubjectMaskRoiPoint& a = points[i];
            const SubjectMaskRoiPoint& b = points[(i + 1) % points.size()];
            if (a.row < 0 || a.col < 0 || b.row < 0 || b.col < 0) {
                continue;
            }
            const double y0 = static_cast<double>(a.row) + 0.5;
            const double y1 = static_cast<double>(b.row) + 0.5;
            const bool crosses =
                (y0 <= scan_y && y1 > scan_y) ||
                (y1 <= scan_y && y0 > scan_y);
            if (!crosses || std::fabs(y1 - y0) < 1e-9) {
                continue;
            }
            const double x0 = static_cast<double>(a.col) + 0.5;
            const double x1 = static_cast<double>(b.col) + 0.5;
            const double t = (scan_y - y0) / (y1 - y0);
            intersections.push_back(x0 + (x1 - x0) * t);
        }

        if (intersections.size() < 2) {
            continue;
        }
        std::sort(intersections.begin(), intersections.end());
        for (size_t i = 0; i + 1 < intersections.size(); i += 2) {
            const double x_start = intersections[i];
            const double x_end = intersections[i + 1];
            int col_start = static_cast<int>(std::ceil(x_start - 0.5));
            int col_end = static_cast<int>(std::floor(x_end - 0.5));
            col_start = std::max(0, col_start);
            col_end = std::min(col_count - 1, col_end);
            if (col_start > col_end) {
                continue;
            }
            const size_t row_offset = static_cast<size_t>(row) * cols;
            for (int col = col_start; col <= col_end; ++col) {
                uint8_t& pixel =
                    mask[row_offset + static_cast<size_t>(col)];
                if (pixel != binary_value) {
                    pixel = binary_value;
                    changed = true;
                }
            }
        }
    }

    return changed;
}

}  // namespace

bool SubjectMaskEditSession::start(
    const SubjectMaskEditTarget& target,
    const std::vector<uint8_t>& original_mask,
    std::string* error_message) {
    if (!validateTargetAndMask(target, original_mask, error_message)) {
        return false;
    }

    target_ = target;
    original_mask_ = binarize(original_mask);
    preview_mask_ = original_mask_;
    dirty_ = false;
    active_ = true;
    ++preview_revision_;
    return true;
}

bool SubjectMaskEditSession::startFromLoadedRow(
    const ZarrDetectionLoader& loader,
    size_t roi_index,
    const std::string& component_name,
    std::string* error_message) {
    ZarrDetectionLoader::RefinedSubjectMaskComponentRow row;
    if (!loader.readRefinedSubjectMaskComponentRow(
            roi_index, component_name, row, error_message)) {
        return false;
    }

    SubjectMaskEditTarget target;
    target.zarr_path = loader.getArchivePath();
    target.refined_run = row.run_name;
    target.roi_index = static_cast<int32_t>(row.roi_index);
    target.component_name = row.component_name;
    target.channel_index = row.channel_index;
    target.rows = row.rows;
    target.cols = row.cols;
    return start(target, row.mask, error_message);
}

void SubjectMaskEditSession::clear() {
    active_ = false;
    dirty_ = false;
    target_ = SubjectMaskEditTarget{};
    original_mask_.clear();
    preview_mask_.clear();
    ++preview_revision_;
}

bool SubjectMaskEditSession::resetPreview() {
    if (!active_) {
        return false;
    }
    if (preview_mask_ != original_mask_) {
        ++preview_revision_;
    }
    preview_mask_ = original_mask_;
    dirty_ = false;
    return true;
}

bool SubjectMaskEditSession::replacePreviewMask(
    const std::vector<uint8_t>& mask,
    std::string* error_message) {
    if (!active_) {
        if (error_message != nullptr) {
            *error_message = "No active subject-mask edit session.";
        }
        return false;
    }
    if (!validateTargetAndMask(target_, mask, error_message)) {
        return false;
    }
    preview_mask_ = binarize(mask);
    dirty_ = preview_mask_ != original_mask_;
    ++preview_revision_;
    return true;
}

bool SubjectMaskEditSession::setPixel(size_t row, size_t col, uint8_t value) {
    if (!active_ || row >= target_.rows || col >= target_.cols) {
        return false;
    }
    const size_t index = row * target_.cols + col;
    const uint8_t binary_value = value != 0 ? 1 : 0;
    if (preview_mask_[index] == binary_value) {
        return false;
    }
    preview_mask_[index] = binary_value;
    dirty_ = preview_mask_ != original_mask_;
    ++preview_revision_;
    return true;
}

bool SubjectMaskEditSession::stampDisk(int center_row,
                                       int center_col,
                                       int radius_px,
                                       uint8_t value) {
    if (!active_ || target_.rows == 0 || target_.cols == 0) {
        return false;
    }
    const uint8_t binary_value = value != 0 ? 1 : 0;
    const bool changed = stampDiskInto(preview_mask_,
                                       target_.rows,
                                       target_.cols,
                                       center_row,
                                       center_col,
                                       radius_px,
                                       binary_value);
    if (changed) {
        dirty_ = preview_mask_ != original_mask_;
        ++preview_revision_;
    }
    return changed;
}

bool SubjectMaskEditSession::stampLine(int row0,
                                       int col0,
                                       int row1,
                                       int col1,
                                       int radius_px,
                                       uint8_t value) {
    if (!active_) {
        return false;
    }
    const int dy = row1 - row0;
    const int dx = col1 - col0;
    const int steps = std::max(std::abs(dx), std::abs(dy));
    if (steps <= 0) {
        return stampDisk(row1, col1, radius_px, value);
    }

    const uint8_t binary_value = value != 0 ? 1 : 0;
    bool changed = false;
    for (int step = 0; step <= steps; ++step) {
        const double t = static_cast<double>(step) / static_cast<double>(steps);
        const int row = static_cast<int>(
            std::lround(static_cast<double>(row0) +
                        static_cast<double>(dy) * t));
        const int col = static_cast<int>(
            std::lround(static_cast<double>(col0) +
                        static_cast<double>(dx) * t));
        changed = stampDiskInto(preview_mask_,
                                target_.rows,
                                target_.cols,
                                row,
                                col,
                                radius_px,
                                binary_value) ||
                  changed;
    }
    if (changed) {
        dirty_ = preview_mask_ != original_mask_;
        ++preview_revision_;
    }
    return changed;
}

bool SubjectMaskEditSession::fillPolygon(
    const std::vector<SubjectMaskRoiPoint>& points,
    uint8_t value) {
    if (!active_) {
        return false;
    }
    const uint8_t binary_value = value != 0 ? 1 : 0;
    bool changed = fillPolygonInto(preview_mask_,
                                   target_.rows,
                                   target_.cols,
                                   points,
                                   binary_value);
    if (changed) {
        for (size_t i = 0; i < points.size(); ++i) {
            const SubjectMaskRoiPoint& a = points[i];
            const SubjectMaskRoiPoint& b = points[(i + 1) % points.size()];
            changed = stampDiskInto(preview_mask_,
                                    target_.rows,
                                    target_.cols,
                                    a.row,
                                    a.col,
                                    0,
                                    binary_value) ||
                      changed;
            changed = stampDiskInto(preview_mask_,
                                    target_.rows,
                                    target_.cols,
                                    b.row,
                                    b.col,
                                    0,
                                    binary_value) ||
                      changed;
        }
        dirty_ = preview_mask_ != original_mask_;
        ++preview_revision_;
    }
    return changed;
}

SubjectMaskWritebackRequest SubjectMaskEditSession::buildWritebackRequest(
    std::string reason) const {
    SubjectMaskWritebackRequest request;
    if (!active_) {
        return request;
    }
    request.zarr_path = target_.zarr_path;
    request.refined_run = target_.refined_run;
    request.component_name = target_.component_name;
    request.roi_index = target_.roi_index;
    request.rows = target_.rows;
    request.cols = target_.cols;
    request.binary_mask = preview_mask_;
    request.reason = std::move(reason);
    request.validate = true;
    return request;
}
