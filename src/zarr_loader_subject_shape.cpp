#include "zarr_loader_internal.h"

#include <iostream>

namespace {

std::string jsonStringAttr(const nlohmann::json& attrs, const char* key) {
    if (attrs.contains(key) && attrs[key].is_string()) {
        return attrs[key].get<std::string>();
    }
    return {};
}

int jsonIntAttr(const nlohmann::json& attrs, const char* key) {
    if (attrs.contains(key) && attrs[key].is_number_integer()) {
        return attrs[key].get<int>();
    }
    if (attrs.contains(key) && attrs[key].is_number()) {
        return static_cast<int>(attrs[key].get<double>());
    }
    return -1;
}

void appendWarning(std::string& warning, const std::string& message) {
    if (message.empty()) {
        return;
    }
    if (!warning.empty()) {
        warning += " ";
    }
    warning += message;
}

std::array<float, 2> pointOrNan(
    const std::vector<std::array<float, 2>>& values,
    size_t row) {
    const float nan_value = std::numeric_limits<float>::quiet_NaN();
    if (row >= values.size()) {
        return {nan_value, nan_value};
    }
    return values[row];
}

bool boolAt(const std::vector<uint8_t>& values, size_t row) {
    return row < values.size() && values[row] != 0;
}

std::string stringAt(const std::vector<std::string>& values, size_t row) {
    if (row >= values.size()) {
        return {};
    }
    return values[row];
}

void copyPointSequence(const std::vector<float>& flat,
                       size_t points_per_row,
                       size_t row,
                       std::vector<std::array<float, 2>>& out) {
    out.clear();
    if (points_per_row == 0) {
        return;
    }
    const size_t base = row * points_per_row * 2;
    if (base + points_per_row * 2 > flat.size()) {
        return;
    }
    out.reserve(points_per_row);
    for (size_t i = 0; i < points_per_row; ++i) {
        const float x = flat[base + i * 2 + 0];
        const float y = flat[base + i * 2 + 1];
        if (std::isfinite(x) && std::isfinite(y)) {
            out.push_back({x, y});
        }
    }
}

}  // namespace

bool ZarrDetectionLoader::loadSubjectShapeData(
    const ts::kvstore::KvStore& store) {
    data_.subject_shape = ZarrDetectionData::SubjectShapeData{};
    if (data_.layout != ZarrLayoutType::kPaletteRuns) {
        return false;
    }

    std::string run_name = requested_subject_shape_run_name_;
    if (run_name.empty()) {
        if (auto group_attrs = readAttrsAny(store, "analysis/subject_shape_runs")) {
            run_name = extractLatestRunName(*group_attrs);
        }
    }
    if (run_name.empty() && !root_path_.empty()) {
        auto fs_candidates = collect_runs_fs(
            root_path_,
            "analysis/subject_shape_runs",
            {"body_frame/valid"});
        if (!fs_candidates.empty()) {
            run_name = fs_candidates.back();
        }
    }
    if (run_name.empty()) {
        return false;
    }

    const std::string run_base =
        "analysis/subject_shape_runs/" + run_name + "/";
    auto run_attrs = readAttrsAny(store, run_base);
    if (!run_attrs.has_value()) {
        if (!requested_subject_shape_run_name_.empty()) {
            std::cout << "  [SUBJECT_SHAPE_WARNING] Requested subject shape run '"
                      << requested_subject_shape_run_name_
                      << "' was not found at " << run_base << std::endl;
        }
        return false;
    }

    auto& shape = data_.subject_shape;
    shape.run_name = run_name;
    shape.schema_id = jsonStringAttr(*run_attrs, "schema_id");
    shape.schema_version = jsonIntAttr(*run_attrs, "schema_version");
    shape.method = jsonStringAttr(*run_attrs, "method");
    shape.method_version = jsonIntAttr(*run_attrs, "method_version");
    shape.source_refined_subject_masks_run =
        jsonStringAttr(*run_attrs, "source_refined_subject_masks_run");
    shape.head_endpoint_semantics =
        jsonStringAttr(*run_attrs, "head_endpoint_semantics");
    shape.row_axis = jsonStringAttr(*run_attrs, "row_axis");

    auto ensureRowCount = [&](size_t observed,
                              const std::string& path) -> bool {
        if (observed == 0) {
            return false;
        }
        if (shape.row_count == 0) {
            shape.row_count = observed;
            return true;
        }
        if (observed != shape.row_count) {
            appendWarning(shape.warning,
                          "Subject-shape array '" + path +
                              "' has row count " + std::to_string(observed) +
                              " but expected " +
                              std::to_string(shape.row_count) + ".");
            return false;
        }
        return true;
    };

    auto readVec2Array = [&](const std::string& rel_path,
                             std::vector<std::array<float, 2>>& out) -> bool {
        const std::string path = run_base + rel_path;
        auto read_as = [&](auto type_token) -> bool {
            using Source = decltype(type_token);
            auto open_result = openArrayAny<Source, 2>(store, path, context_);
            if (!open_result.ok()) {
                return false;
            }
            auto array_result = ts::Read(open_result.value()).result();
            if (!array_result.ok()) {
                return false;
            }
            auto array = array_result.value();
            auto shape_dims = array.shape();
            if (shape_dims.size() != 2 || shape_dims[1] < 2) {
                return false;
            }
            const size_t rows = static_cast<size_t>(shape_dims[0]);
            if (!ensureRowCount(rows, rel_path)) {
                return false;
            }
            out.resize(rows);
            for (size_t row = 0; row < rows; ++row) {
                out[row] = {
                    static_cast<float>(array(static_cast<ts::Index>(row),
                                             static_cast<ts::Index>(0))),
                    static_cast<float>(array(static_cast<ts::Index>(row),
                                             static_cast<ts::Index>(1)))};
            }
            return true;
        };
        return read_as(float{}) || read_as(double{});
    };

    auto readVec3Array = [&](const std::string& rel_path,
                             std::vector<float>& out,
                             size_t& points_per_row) -> bool {
        const std::string path = run_base + rel_path;
        auto read_as = [&](auto type_token) -> bool {
            using Source = decltype(type_token);
            auto open_result = openArrayAny<Source, 3>(store, path, context_);
            if (!open_result.ok()) {
                return false;
            }
            auto array_result = ts::Read(open_result.value()).result();
            if (!array_result.ok()) {
                return false;
            }
            auto array = array_result.value();
            auto shape_dims = array.shape();
            if (shape_dims.size() != 3 || shape_dims[2] < 2) {
                return false;
            }
            const size_t rows = static_cast<size_t>(shape_dims[0]);
            if (!ensureRowCount(rows, rel_path)) {
                return false;
            }
            points_per_row = static_cast<size_t>(shape_dims[1]);
            out.assign(rows * points_per_row * 2,
                       std::numeric_limits<float>::quiet_NaN());
            for (size_t row = 0; row < rows; ++row) {
                for (size_t point = 0; point < points_per_row; ++point) {
                    out[(row * points_per_row + point) * 2 + 0] =
                        static_cast<float>(array(static_cast<ts::Index>(row),
                                                 static_cast<ts::Index>(point),
                                                 static_cast<ts::Index>(0)));
                    out[(row * points_per_row + point) * 2 + 1] =
                        static_cast<float>(array(static_cast<ts::Index>(row),
                                                 static_cast<ts::Index>(point),
                                                 static_cast<ts::Index>(1)));
                }
            }
            return true;
        };
        return read_as(float{}) || read_as(double{});
    };

    auto readBoolRows = [&](const std::string& rel_path,
                            std::vector<uint8_t>& out) -> bool {
        if (!readBoolArray(store, run_base + rel_path, out)) {
            return false;
        }
        if (!ensureRowCount(out.size(), rel_path)) {
            out.clear();
            return false;
        }
        return true;
    };

    auto readReasonRows = [&](const std::string& rel_path,
                              std::vector<std::string>& out) -> bool {
        if (!readStringArray(store, run_base + rel_path, out)) {
            return false;
        }
        if (!ensureRowCount(out.size(), rel_path)) {
            out.clear();
            return false;
        }
        return true;
    };

    readInt32Array(store,
                   run_base + "row_index/frame_indices",
                   shape.frame_indices);
    const bool has_canonical_frame_indices = !shape.frame_indices.empty();
    if (shape.frame_indices.empty()) {
        readInt32Array(store, run_base + "frame_indices", shape.frame_indices);
    }
    if (!shape.frame_indices.empty()) {
        ensureRowCount(shape.frame_indices.size(), "frame_indices");
    }

    readVec2Array("body_frame/origin_xy", shape.body_origin_xy);
    readVec2Array("body_frame/forward_axis_xy", shape.body_forward_axis_xy);
    readVec2Array("body_frame/left_axis_xy", shape.body_left_axis_xy);
    readBoolRows("body_frame/valid", shape.body_frame_valid);
    readReasonRows("body_frame/failure_reason_bytes",
                   shape.body_frame_failure_reason);

    readVec2Array("components/subject_body/snout_tip_xy",
                  shape.snout_tip_xy);
    readBoolRows("components/subject_body/snout_tip_valid",
                 shape.snout_tip_valid);
    readReasonRows("components/subject_body/snout_tip_failure_reason_bytes",
                   shape.snout_tip_failure_reason);
    readVec2Array("components/subject_body/tail_base_xy",
                  shape.tail_base_xy);
    readBoolRows("components/subject_body/tail_base_valid",
                 shape.tail_base_valid);
    readVec2Array("components/subject_body/tail_tip_xy", shape.tail_tip_xy);
    readBoolRows("components/subject_body/centerline_valid",
                 shape.centerline_valid);
    readBoolRows("components/subject_body/centerline_reaches_snout",
                 shape.centerline_reaches_snout);
    readReasonRows("components/subject_body/centerline_failure_reason_bytes",
                   shape.centerline_failure_reason);
    readVec3Array("components/subject_body/centerline_xy",
                  shape.centerline_xy,
                  shape.centerline_points);
    readBoolRows("components/subject_body/bspline_valid",
                 shape.bspline_valid);
    readReasonRows("components/subject_body/bspline_failure_reason_bytes",
                   shape.bspline_failure_reason);
    readVec3Array("components/subject_body/bspline_sample_xy",
                  shape.bspline_sample_xy,
                  shape.bspline_sample_points);
    readVec3Array("components/subject_body/bspline_control_points_xy",
                  shape.bspline_control_points_xy,
                  shape.bspline_control_points);
    readBoolRows("components/subject_body/tail_sample_valid",
                 shape.tail_sample_valid);
    readReasonRows("components/subject_body/tail_sample_failure_reason_bytes",
                   shape.tail_sample_failure_reason);
    readBoolRows("components/subject_body/source_mask_qc_severe_failure",
                 shape.source_mask_qc_severe_failure);
    readReasonRows("components/subject_body/source_mask_qc_reason_bytes",
                   shape.source_mask_qc_reason);
    readVec3Array("components/subject_body/tail_sample_xy",
                  shape.tail_sample_xy,
                  shape.tail_sample_count);
    readVec3Array("components/subject_body/tail_normal_xy",
                  shape.tail_normal_xy,
                  shape.tail_normal_count);

    readVec2Array("components/swim_bladder/caudal_contour_point_xy",
                  shape.caudal_contour_point_xy);
    readBoolRows("components/swim_bladder/caudal_contour_valid",
                 shape.caudal_contour_valid);

    if (shape.row_count == 0) {
        data_.subject_shape = ZarrDetectionData::SubjectShapeData{};
        return false;
    }

    shape.row_to_frame.assign(shape.row_count, -1);
    if (shape.frame_indices.size() == shape.row_count) {
        shape.row_to_frame = shape.frame_indices;
        if (!has_canonical_frame_indices) {
            appendWarning(
                shape.warning,
                "Subject-shape row_index/frame_indices missing; using legacy root frame_indices for QC seeking.");
        }
    } else {
        if (shape.frame_indices.empty()) {
            appendWarning(
                shape.warning,
                "Subject-shape row_index/frame_indices missing; using ROI-to-detection frame mapping for QC seeking.");
        } else {
            appendWarning(
                shape.warning,
                "Subject-shape row_index/frame_indices length mismatch; using ROI-to-detection frame mapping for QC seeking.");
        }
        for (size_t det = 0; det < data_.mask_roi_indices.size(); ++det) {
            const int32_t roi_index = data_.mask_roi_indices[det];
            if (roi_index < 0 ||
                static_cast<size_t>(roi_index) >= shape.row_to_frame.size() ||
                shape.row_to_frame[static_cast<size_t>(roi_index)] >= 0) {
                continue;
            }
            if (det < data_.frame_indices.size()) {
                shape.row_to_frame[static_cast<size_t>(roi_index)] =
                    data_.frame_indices[det];
            }
        }
    }

    shape.loaded = true;
    std::cout << "  Subject shape run '" << shape.run_name << "' loaded (rows "
              << shape.row_count << ", source refined masks '"
              << shape.source_refined_subject_masks_run << "', centerline points "
              << shape.centerline_points << ", B-spline samples "
              << shape.bspline_sample_points << ")" << std::endl;
    if (!shape.warning.empty()) {
        std::cout << "  [SUBJECT_SHAPE_WARNING] " << shape.warning
                  << std::endl;
    }
    return true;
}

bool ZarrDetectionLoader::populateSubjectShapeEntry(
    size_t roi_index,
    FrameDetections::SubjectShape& out_shape) const {
    const auto& shape = data_.subject_shape;
    if (!shape.loaded || roi_index >= shape.row_count) {
        return false;
    }

    out_shape.valid = true;
    out_shape.roi_index = static_cast<int32_t>(roi_index);
    out_shape.body_frame_valid = boolAt(shape.body_frame_valid, roi_index);
    out_shape.body_origin_xy = pointOrNan(shape.body_origin_xy, roi_index);
    out_shape.body_forward_axis_xy =
        pointOrNan(shape.body_forward_axis_xy, roi_index);
    out_shape.body_left_axis_xy =
        pointOrNan(shape.body_left_axis_xy, roi_index);
    out_shape.body_frame_failure_reason =
        stringAt(shape.body_frame_failure_reason, roi_index);

    out_shape.snout_tip_valid = boolAt(shape.snout_tip_valid, roi_index);
    out_shape.snout_tip_xy = pointOrNan(shape.snout_tip_xy, roi_index);
    out_shape.snout_tip_failure_reason =
        stringAt(shape.snout_tip_failure_reason, roi_index);
    out_shape.tail_base_valid = boolAt(shape.tail_base_valid, roi_index);
    out_shape.tail_base_xy = pointOrNan(shape.tail_base_xy, roi_index);
    out_shape.tail_tip_xy = pointOrNan(shape.tail_tip_xy, roi_index);
    out_shape.centerline_valid = boolAt(shape.centerline_valid, roi_index);
    out_shape.centerline_reaches_snout =
        boolAt(shape.centerline_reaches_snout, roi_index);
    out_shape.centerline_failure_reason =
        stringAt(shape.centerline_failure_reason, roi_index);
    out_shape.bspline_valid = boolAt(shape.bspline_valid, roi_index);
    out_shape.bspline_failure_reason =
        stringAt(shape.bspline_failure_reason, roi_index);
    out_shape.tail_sample_valid = boolAt(shape.tail_sample_valid, roi_index);
    out_shape.tail_sample_failure_reason =
        stringAt(shape.tail_sample_failure_reason, roi_index);
    out_shape.caudal_contour_valid =
        boolAt(shape.caudal_contour_valid, roi_index);
    out_shape.caudal_contour_point_xy =
        pointOrNan(shape.caudal_contour_point_xy, roi_index);

    copyPointSequence(shape.centerline_xy,
                      shape.centerline_points,
                      roi_index,
                      out_shape.centerline_xy);
    copyPointSequence(shape.bspline_sample_xy,
                      shape.bspline_sample_points,
                      roi_index,
                      out_shape.bspline_sample_xy);
    copyPointSequence(shape.bspline_control_points_xy,
                      shape.bspline_control_points,
                      roi_index,
                      out_shape.bspline_control_points_xy);
    copyPointSequence(shape.tail_sample_xy,
                      shape.tail_sample_count,
                      roi_index,
                      out_shape.tail_sample_xy);
    copyPointSequence(shape.tail_normal_xy,
                      shape.tail_normal_count,
                      roi_index,
                      out_shape.tail_normal_xy);
    return true;
}

ZarrDetectionLoader::SubjectShapeQcJumpResult
ZarrDetectionLoader::computeSubjectShapeQcJump(
    const SubjectShapeQcFilterOptions& filters,
    int current_frame_num,
    bool forward) const {
    SubjectShapeQcJumpResult result;
    const auto& shape = data_.subject_shape;
    if (!shape.loaded || shape.row_count == 0) {
        result.status = "Subject-shape data unavailable.";
        return result;
    }

    const std::string reason_filter =
        toLowerCopy(filters.reason_substring);
    const bool filter_enabled =
        filters.any_invalid ||
        filters.source_mask_qc_failure ||
        filters.body_frame_invalid ||
        filters.snout_invalid ||
        filters.centerline_invalid ||
        filters.centerline_misses_snout ||
        filters.bspline_invalid ||
        filters.tail_base_invalid ||
        filters.tail_sample_invalid ||
        !reason_filter.empty();
    if (!filter_enabled) {
        result.status = "Enable at least one subject-shape QC filter.";
        return result;
    }

    auto rowHasReason = [&](size_t row) -> bool {
        if (reason_filter.empty()) {
            return false;
        }
        const std::array<const std::vector<std::string>*, 6> reason_arrays = {
            &shape.body_frame_failure_reason,
            &shape.source_mask_qc_reason,
            &shape.snout_tip_failure_reason,
            &shape.centerline_failure_reason,
            &shape.bspline_failure_reason,
            &shape.tail_sample_failure_reason};
        for (const auto* reasons : reason_arrays) {
            if (row >= reasons->size()) {
                continue;
            }
            const std::string lowered = toLowerCopy((*reasons)[row]);
            if (lowered.find(reason_filter) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    auto rowMatches = [&](size_t row) -> bool {
        const bool body_invalid =
            row < shape.body_frame_valid.size() &&
            shape.body_frame_valid[row] == 0;
        const bool source_mask_qc_failed =
            row < shape.source_mask_qc_severe_failure.size() &&
            shape.source_mask_qc_severe_failure[row] != 0;
        const bool snout_invalid =
            row < shape.snout_tip_valid.size() &&
            shape.snout_tip_valid[row] == 0;
        const bool centerline_invalid =
            row < shape.centerline_valid.size() &&
            shape.centerline_valid[row] == 0;
        const bool centerline_misses_snout =
            row < shape.centerline_reaches_snout.size() &&
            shape.centerline_reaches_snout[row] == 0;
        const bool bspline_invalid =
            row < shape.bspline_valid.size() &&
            shape.bspline_valid[row] == 0;
        const bool tail_base_invalid =
            row < shape.tail_base_valid.size() &&
            shape.tail_base_valid[row] == 0;
        const bool tail_sample_invalid =
            row < shape.tail_sample_valid.size() &&
            shape.tail_sample_valid[row] == 0;

        bool matched = false;
        matched = matched ||
                  (filters.source_mask_qc_failure &&
                   source_mask_qc_failed);
        matched = matched || (filters.body_frame_invalid && body_invalid);
        matched = matched || (filters.snout_invalid && snout_invalid);
        matched = matched || (filters.centerline_invalid && centerline_invalid);
        matched = matched ||
                  (filters.centerline_misses_snout &&
                   centerline_misses_snout);
        matched = matched || (filters.bspline_invalid && bspline_invalid);
        matched = matched || (filters.tail_base_invalid && tail_base_invalid);
        matched = matched || (filters.tail_sample_invalid && tail_sample_invalid);
        matched = matched || rowHasReason(row);
        if (filters.any_invalid) {
            matched = matched || source_mask_qc_failed || body_invalid ||
                      snout_invalid || centerline_invalid ||
                      centerline_misses_snout || bspline_invalid ||
                      tail_base_invalid || tail_sample_invalid;
        }
        return matched;
    };

    std::vector<int> frames;
    frames.reserve(shape.row_count);
    for (size_t row = 0; row < shape.row_count; ++row) {
        if (!rowMatches(row)) {
            continue;
        }
        result.match_count++;
        if (row < shape.row_to_frame.size() && shape.row_to_frame[row] >= 0) {
            frames.push_back(shape.row_to_frame[row]);
        }
    }

    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    if (frames.empty()) {
        if (result.match_count == 0) {
            result.status = "No subject-shape rows match QC filters.";
        } else {
            result.status =
                "Subject-shape rows matched, but no video frame mapping is available.";
        }
        return result;
    }

    int target_frame = current_frame_num;
    if (forward) {
        auto it = std::upper_bound(frames.begin(), frames.end(),
                                   current_frame_num);
        if (it == frames.end()) {
            it = frames.begin();
        }
        target_frame = *it;
    } else {
        auto it = std::lower_bound(frames.begin(), frames.end(),
                                   current_frame_num);
        if (it == frames.begin()) {
            target_frame = frames.back();
        } else {
            --it;
            target_frame = *it;
        }
    }

    result.target_frame = target_frame;
    result.status = "Subject-shape QC rows: " +
                    std::to_string(result.match_count) +
                    " matching rows across " +
                    std::to_string(frames.size()) + " frames.";
    return result;
}
