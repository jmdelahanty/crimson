#include "zarr_loader_internal.h"

#include <iostream>

namespace {

constexpr float kRadiansToDegrees = 57.29577951308232f;

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

bool valueIsNonFiniteAt(const std::vector<float>& values, size_t row) {
    return row < values.size() && !std::isfinite(values[row]);
}

}  // namespace

bool ZarrDetectionLoader::loadTailKinematicsData(
    const ts::kvstore::KvStore& store) {
    data_.tail_kinematics = ZarrDetectionData::TailKinematicsData{};
    if (data_.layout != ZarrLayoutType::kPaletteRuns) {
        return false;
    }

    std::string run_name = requested_tail_kinematics_run_name_;
    if (run_name.empty()) {
        if (auto group_attrs =
                readAttrsAny(store, "analysis/tail_kinematics_runs")) {
            run_name = extractLatestRunName(*group_attrs);
        }
    }
    if (run_name.empty() && !root_path_.empty()) {
        auto fs_candidates = collect_runs_fs(root_path_,
                                             "analysis/tail_kinematics_runs",
                                             {"valid"});
        if (!fs_candidates.empty()) {
            run_name = fs_candidates.back();
        }
    }
    if (run_name.empty()) {
        return false;
    }

    const std::string run_base =
        "analysis/tail_kinematics_runs/" + run_name + "/";
    auto run_attrs = readAttrsAny(store, run_base);
    if (!run_attrs.has_value()) {
        if (!requested_tail_kinematics_run_name_.empty()) {
            std::cout << "  [TAIL_KINEMATICS_WARNING] Requested tail kinematics run '"
                      << requested_tail_kinematics_run_name_
                      << "' was not found at " << run_base << std::endl;
        }
        return false;
    }

    auto& tail = data_.tail_kinematics;
    tail.run_name = run_name;
    tail.schema_id = jsonStringAttr(*run_attrs, "schema_id");
    tail.schema_version = jsonIntAttr(*run_attrs, "schema_version");
    tail.method = jsonStringAttr(*run_attrs, "method");
    tail.method_version = jsonIntAttr(*run_attrs, "method_version");
    tail.row_axis = jsonStringAttr(*run_attrs, "row_axis");
    tail.source_subject_shape_run =
        jsonStringAttr(*run_attrs, "source_subject_shape_run");
    tail.source_refined_subject_masks_run =
        jsonStringAttr(*run_attrs, "source_refined_subject_masks_run");
    const int sample_count_attr =
        jsonIntAttr(*run_attrs, "tail_angle_sample_count");
    if (sample_count_attr > 0) {
        tail.sample_count = static_cast<size_t>(sample_count_attr);
    }

    auto ensureRowCount = [&](size_t observed,
                              const std::string& path) -> bool {
        if (observed == 0) {
            return false;
        }
        if (tail.row_count == 0) {
            tail.row_count = observed;
            return true;
        }
        if (observed != tail.row_count) {
            appendWarning(tail.warning,
                          "Tail-kinematics array '" + path +
                              "' has row count " + std::to_string(observed) +
                              " but expected " +
                              std::to_string(tail.row_count) + ".");
            return false;
        }
        return true;
    };

    auto readFloatRows = [&](const std::string& rel_path,
                             std::vector<float>& out) -> bool {
        if (!readFloatArray(store, run_base + rel_path, out)) {
            return false;
        }
        if (!ensureRowCount(out.size(), rel_path)) {
            out.clear();
            return false;
        }
        return true;
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

    auto readFloat2D = [&](const std::string& rel_path,
                           std::vector<float>& out,
                           size_t& cols_out) -> bool {
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
            if (shape_dims.size() != 2 || shape_dims[1] <= 0) {
                return false;
            }
            const size_t rows = static_cast<size_t>(shape_dims[0]);
            if (!ensureRowCount(rows, rel_path)) {
                return false;
            }
            cols_out = static_cast<size_t>(shape_dims[1]);
            out.assign(rows * cols_out,
                       std::numeric_limits<float>::quiet_NaN());
            for (size_t row = 0; row < rows; ++row) {
                for (size_t col = 0; col < cols_out; ++col) {
                    out[row * cols_out + col] =
                        static_cast<float>(array(static_cast<ts::Index>(row),
                                                 static_cast<ts::Index>(col)));
                }
            }
            return true;
        };
        return read_as(float{}) || read_as(double{});
    };

    auto readFloat3DXY = [&](const std::string& rel_path,
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

    readInt32Array(store, run_base + "frame_index", tail.frame_index);
    if (tail.frame_index.empty()) {
        readInt32Array(store,
                       run_base + "row_index/frame_indices",
                       tail.frame_index);
        if (!tail.frame_index.empty()) {
            appendWarning(
                tail.warning,
                "Tail-kinematics frame_index missing; using row_index/frame_indices for seeking.");
        }
    }
    if (!tail.frame_index.empty()) {
        ensureRowCount(tail.frame_index.size(), "frame_index");
    }
    readBoolRows("valid", tail.valid);
    readReasonRows("failure_reason_bytes", tail.failure_reason);
    readFloatArray(store, run_base + "tail_angle_sample_s",
                   tail.tail_angle_sample_s);
    readFloat3DXY("tail_angle_sample_xy",
                  tail.tail_angle_sample_xy,
                  tail.tail_angle_sample_xy_count);

    size_t angle_cols = 0;
    if (readFloat2D("tail_angle_deg", tail.tail_angle_deg, angle_cols)) {
        tail.sample_count = angle_cols;
    } else {
        std::vector<float> angle_rad;
        if (readFloat2D("tail_angle_rad", angle_rad, angle_cols)) {
            tail.tail_angle_deg.resize(angle_rad.size());
            for (size_t i = 0; i < angle_rad.size(); ++i) {
                tail.tail_angle_deg[i] = angle_rad[i] * kRadiansToDegrees;
            }
            tail.sample_count = angle_cols;
            appendWarning(
                tail.warning,
                "Tail-kinematics tail_angle_deg missing; computed degrees from radians.");
        }
    }

    if (!readFloatRows("tail_tip_angle_deg", tail.tail_tip_angle_deg)) {
        std::vector<float> rad;
        if (readFloatRows("tail_tip_angle_rad", rad)) {
            tail.tail_tip_angle_deg.resize(rad.size());
            for (size_t i = 0; i < rad.size(); ++i) {
                tail.tail_tip_angle_deg[i] = rad[i] * kRadiansToDegrees;
            }
            appendWarning(
                tail.warning,
                "Tail-kinematics tail_tip_angle_deg missing; computed degrees from radians.");
        }
    }
    if (!readFloatRows("max_abs_tail_angle_deg",
                       tail.max_abs_tail_angle_deg)) {
        std::vector<float> rad;
        if (readFloatRows("max_abs_tail_angle_rad", rad)) {
            tail.max_abs_tail_angle_deg.resize(rad.size());
            for (size_t i = 0; i < rad.size(); ++i) {
                tail.max_abs_tail_angle_deg[i] = rad[i] * kRadiansToDegrees;
            }
        }
    }
    if (!readFloatRows("tail_angle_rms_deg", tail.tail_angle_rms_deg)) {
        std::vector<float> rad;
        if (readFloatRows("tail_angle_rms_rad", rad)) {
            tail.tail_angle_rms_deg.resize(rad.size());
            for (size_t i = 0; i < rad.size(); ++i) {
                tail.tail_angle_rms_deg[i] = rad[i] * kRadiansToDegrees;
            }
        }
    }
    size_t lateral_cols = 0;
    readFloat2D("tail_lateral_deflection_px",
                tail.tail_lateral_deflection_px,
                lateral_cols);
    readFloatRows("tail_tip_lateral_deflection_px",
                  tail.tail_tip_lateral_deflection_px);
    size_t curvature_cols = 0;
    readFloat2D("tail_curvature_px_inv",
                tail.tail_curvature_px_inv,
                curvature_cols);
    readFloatRows("max_abs_tail_curvature_px_inv",
                  tail.max_abs_tail_curvature_px_inv);

    if (tail.row_count == 0) {
        data_.tail_kinematics = ZarrDetectionData::TailKinematicsData{};
        return false;
    }
    if (tail.frame_index.size() == tail.row_count) {
        tail.row_to_frame = tail.frame_index;
    } else {
        tail.row_to_frame.assign(tail.row_count, -1);
        appendWarning(
            tail.warning,
            "Tail-kinematics frame_index length mismatch or missing; QC seeking may be unavailable.");
    }
    if (tail.sample_count == 0) {
        tail.sample_count = std::max({angle_cols,
                                      lateral_cols,
                                      curvature_cols,
                                      tail.tail_angle_sample_s.size(),
                                      tail.tail_angle_sample_xy_count});
    }
    if (data_.subject_shape.loaded &&
        !tail.source_subject_shape_run.empty() &&
        tail.source_subject_shape_run != data_.subject_shape.run_name) {
        appendWarning(tail.warning,
                      "Tail-kinematics source subject-shape run differs from the loaded subject-shape run.");
    }

    tail.loaded = true;
    std::cout << "  Tail kinematics run '" << tail.run_name
              << "' loaded (rows " << tail.row_count << ", samples "
              << tail.sample_count << ", source subject shape '"
              << tail.source_subject_shape_run << "')" << std::endl;
    if (!tail.warning.empty()) {
        std::cout << "  [TAIL_KINEMATICS_WARNING] " << tail.warning
                  << std::endl;
    }
    return true;
}

ZarrDetectionLoader::TailKinematicsQcJumpResult
ZarrDetectionLoader::computeTailKinematicsQcJump(
    const TailKinematicsQcFilterOptions& filters,
    int current_frame_num,
    bool forward) const {
    TailKinematicsQcJumpResult result;
    const auto& tail = data_.tail_kinematics;
    if (!tail.loaded || tail.row_count == 0) {
        result.status = "Tail-kinematics data unavailable.";
        return result;
    }

    const std::string reason_filter =
        toLowerCopy(filters.reason_substring);
    const bool filter_enabled =
        filters.invalid_rows ||
        filters.nonfinite_tail_tip_angle ||
        filters.nonfinite_tail_tip_lateral_deflection ||
        !reason_filter.empty();
    if (!filter_enabled) {
        result.status = "Enable at least one tail-kinematics QC filter.";
        return result;
    }

    auto rowHasReason = [&](size_t row) -> bool {
        if (reason_filter.empty() || row >= tail.failure_reason.size()) {
            return false;
        }
        return toLowerCopy(tail.failure_reason[row]).find(reason_filter) !=
               std::string::npos;
    };

    auto rowMatches = [&](size_t row) -> bool {
        const bool invalid =
            row < tail.valid.size() && tail.valid[row] == 0;
        bool matched = false;
        matched = matched || (filters.invalid_rows && invalid);
        matched = matched ||
                  (filters.nonfinite_tail_tip_angle &&
                   valueIsNonFiniteAt(tail.tail_tip_angle_deg, row));
        matched = matched ||
                  (filters.nonfinite_tail_tip_lateral_deflection &&
                   valueIsNonFiniteAt(tail.tail_tip_lateral_deflection_px, row));
        matched = matched || rowHasReason(row);
        return matched;
    };

    std::vector<std::pair<int, size_t>> frame_rows;
    frame_rows.reserve(tail.row_count);
    for (size_t row = 0; row < tail.row_count; ++row) {
        if (!rowMatches(row)) {
            continue;
        }
        result.match_count++;
        if (row < tail.row_to_frame.size() && tail.row_to_frame[row] >= 0) {
            frame_rows.emplace_back(tail.row_to_frame[row], row);
        }
    }

    std::sort(frame_rows.begin(), frame_rows.end());
    frame_rows.erase(std::unique(frame_rows.begin(), frame_rows.end()),
                     frame_rows.end());
    if (frame_rows.empty()) {
        if (result.match_count == 0) {
            result.status = "No tail-kinematics rows match QC filters.";
        } else {
            result.status =
                "Tail-kinematics rows matched, but no video frame mapping is available.";
        }
        return result;
    }

    auto select_forward = [&]() -> std::pair<int, size_t> {
        auto it = std::upper_bound(
            frame_rows.begin(),
            frame_rows.end(),
            std::make_pair(current_frame_num, std::numeric_limits<size_t>::max()));
        if (it == frame_rows.end()) {
            it = frame_rows.begin();
        }
        return *it;
    };
    auto select_backward = [&]() -> std::pair<int, size_t> {
        auto it = std::lower_bound(
            frame_rows.begin(),
            frame_rows.end(),
            std::make_pair(current_frame_num, static_cast<size_t>(0)));
        if (it == frame_rows.begin()) {
            return frame_rows.back();
        }
        --it;
        return *it;
    };

    const auto selected = forward ? select_forward() : select_backward();
    result.target_frame = selected.first;
    result.target_row = selected.second;
    result.status = "Tail-kinematics QC rows: " +
                    std::to_string(result.match_count) +
                    " matching rows across " +
                    std::to_string(frame_rows.size()) + " frame/row entries.";
    return result;
}

std::optional<size_t> ZarrDetectionLoader::findTailKinematicsRowForFrame(
    int frame) const {
    const auto& tail = data_.tail_kinematics;
    if (!tail.loaded || frame < 0) {
        return std::nullopt;
    }
    for (size_t row = 0; row < tail.row_to_frame.size(); ++row) {
        if (tail.row_to_frame[row] == frame) {
            return row;
        }
    }
    return std::nullopt;
}

std::optional<int32_t> ZarrDetectionLoader::getTailKinematicsFrameForRow(
    size_t row) const {
    const auto& tail = data_.tail_kinematics;
    if (!tail.loaded || row >= tail.row_to_frame.size() ||
        tail.row_to_frame[row] < 0) {
        return std::nullopt;
    }
    return tail.row_to_frame[row];
}
