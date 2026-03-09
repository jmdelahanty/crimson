#include "zarr_loader_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
using json = nlohmann::json;

constexpr double kPi = 3.14159265358979323846;

uint64_t doubleToBits(double value) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool isNanBits(uint64_t bits) {
    return (bits & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL &&
           (bits & 0x000fffffffffffffULL) != 0;
}

double makeNanNumber() {
    constexpr uint64_t kQuietNanBits = 0x7ff8000000000000ULL;
    double value = 0.0;
    std::memcpy(&value, &kQuietNanBits, sizeof(value));
    return value;
}

float makeNanFloat() {
    constexpr uint32_t kQuietNanBits = 0x7fc00000U;
    float value = 0.0f;
    std::memcpy(&value, &kQuietNanBits, sizeof(value));
    return value;
}

bool isFiniteNumber(double value) {
    return (doubleToBits(value) & 0x7ff0000000000000ULL) !=
           0x7ff0000000000000ULL;
}

bool isNanNumber(double value) {
    return isNanBits(doubleToBits(value));
}

bool isFinitePair(const std::array<double, 2>& point) {
    return isFiniteNumber(point[0]) && isFiniteNumber(point[1]);
}

bool anyFiniteKeypoint(const std::vector<std::array<double, 2>>& keypoints) {
    for (const auto& point : keypoints) {
        if (isFinitePair(point)) {
            return true;
        }
    }
    return false;
}

bool nanAwareEqual(double a, double b) {
    const uint64_t bits_a = doubleToBits(a);
    const uint64_t bits_b = doubleToBits(b);
    if (isNanBits(bits_a) && isNanBits(bits_b)) {
        return true;
    }
    constexpr uint64_t kSignlessMask = 0x7fffffffffffffffULL;
    if ((bits_a & kSignlessMask) == 0 && (bits_b & kSignlessMask) == 0) {
        return true;
    }
    return bits_a == bits_b;
}

bool nanAwareEqualPoint(const std::array<double, 2>& a,
                        const std::array<double, 2>& b) {
    return nanAwareEqual(a[0], b[0]) && nanAwareEqual(a[1], b[1]);
}

bool nanAwareEqualPointRows(const std::vector<std::array<double, 2>>& a,
                            const std::vector<std::array<double, 2>>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (!nanAwareEqualPoint(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

bool nanAwareEqualRows(const std::vector<double>& a,
                       const std::vector<double>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (!nanAwareEqual(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

template <typename T>
T castValueForWrite(double value) {
    if constexpr (std::is_same_v<T, double>) {
        return isNanNumber(value) ? makeNanNumber() : value;
    } else if constexpr (std::is_same_v<T, float>) {
        return isNanNumber(value) ? makeNanFloat() : static_cast<float>(value);
    } else {
        return static_cast<T>(value);
    }
}

std::string toLowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return text;
}

std::vector<std::string> splitReasonTags(const std::string& reason) {
    std::vector<std::string> tags;
    std::string token;
    std::stringstream ss(reason);
    while (std::getline(ss, token, '|')) {
        if (!token.empty()) {
            tags.push_back(token);
        }
    }
    return tags;
}

std::string joinReasonTags(const std::vector<std::string>& tags) {
    std::string out;
    for (size_t i = 0; i < tags.size(); ++i) {
        if (tags[i].empty()) {
            continue;
        }
        if (!out.empty()) {
            out.push_back('|');
        }
        out += tags[i];
    }
    return out;
}

enum class RowReasonAction {
    kManualCorrection = 0,
    kFishPresentNoKeypoints = 1,
    kDetectionIssue = 2,
};

std::string buildReasonLabel(const std::string& existing_reason,
                             RowReasonAction action,
                             bool geometry_valid) {
    static const std::unordered_set<std::string> kDropTags = {
        "detection_failed",
        "low_confidence",
        "confidence_missing",
        "fish_present_no_keypoints",
        "detection_issue",
        "manual_correction",
        "geometry_issue",
    };

    std::vector<std::string> merged;
    std::unordered_set<std::string> seen;

    for (const auto& tag : splitReasonTags(existing_reason)) {
        if (kDropTags.find(tag) != kDropTags.end()) {
            continue;
        }
        if (seen.insert(tag).second) {
            merged.push_back(tag);
        }
    }

    auto append_unique = [&](const std::string& tag) {
        if (tag.empty()) {
            return;
        }
        if (seen.insert(tag).second) {
            merged.push_back(tag);
        }
    };

    if (action == RowReasonAction::kManualCorrection) {
        append_unique("manual_correction");
        if (!geometry_valid) {
            append_unique("geometry_issue");
        }
    } else if (action == RowReasonAction::kFishPresentNoKeypoints) {
        append_unique("fish_present_no_keypoints");
    } else if (action == RowReasonAction::kDetectionIssue) {
        append_unique("detection_issue");
    }

    return joinReasonTags(merged);
}

struct TriangleMetrics {
    double area = makeNanNumber();
    std::array<double, 3> angles = {
        makeNanNumber(),
        makeNanNumber(),
        makeNanNumber()};
    double min_angle = makeNanNumber();
    double max_angle = makeNanNumber();
};

TriangleMetrics computeTriangleMetrics(
    const std::vector<std::array<double, 2>>& keypoints_roi) {
    TriangleMetrics metrics;
    if (keypoints_roi.size() < 3) {
        return metrics;
    }

    const auto& p0 = keypoints_roi[0];
    const auto& p1 = keypoints_roi[1];
    const auto& p2 = keypoints_roi[2];

    if (!isFinitePair(p0) || !isFinitePair(p1) || !isFinitePair(p2)) {
        return metrics;
    }

    auto edge_length = [](const std::array<double, 2>& a,
                          const std::array<double, 2>& b) -> double {
        const double dx = a[0] - b[0];
        const double dy = a[1] - b[1];
        return std::sqrt(dx * dx + dy * dy);
    };

    const double a = edge_length(p1, p2);  // opposite p0
    const double b = edge_length(p0, p2);  // opposite p1
    const double c = edge_length(p0, p1);  // opposite p2
    if (!(a > 0.0) || !(b > 0.0) || !(c > 0.0) || !isFiniteNumber(a) ||
        !isFiniteNumber(b) || !isFiniteNumber(c)) {
        return metrics;
    }

    const double area2 =
        (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p1[1] - p0[1]) * (p2[0] - p0[0]);
    metrics.area = 0.5 * std::abs(area2);

    auto safe_angle = [](double num, double den) -> double {
        if (!(den > 0.0) || !isFiniteNumber(num) || !isFiniteNumber(den)) {
            return makeNanNumber();
        }
        double v = num / den;
        v = std::max(-1.0, std::min(1.0, v));
        return std::acos(v) * (180.0 / kPi);
    };

    metrics.angles[0] = safe_angle(b * b + c * c - a * a, 2.0 * b * c);
    metrics.angles[1] = safe_angle(a * a + c * c - b * b, 2.0 * a * c);
    metrics.angles[2] = safe_angle(a * a + b * b - c * c, 2.0 * a * b);

    metrics.min_angle = metrics.angles[0];
    metrics.max_angle = metrics.angles[0];
    for (size_t i = 1; i < metrics.angles.size(); ++i) {
        metrics.min_angle = std::min(metrics.min_angle, metrics.angles[i]);
        metrics.max_angle = std::max(metrics.max_angle, metrics.angles[i]);
    }
    return metrics;
}

double computeHeadingFromPoints(
    const std::vector<std::array<double, 2>>& keypoints_roi) {
    if (keypoints_roi.size() < 3) {
        return makeNanNumber();
    }

    const auto& bladder = keypoints_roi[0];
    const auto& eye_left = keypoints_roi[1];
    const auto& eye_right = keypoints_roi[2];
    if (!isFinitePair(bladder) || !isFinitePair(eye_left) ||
        !isFinitePair(eye_right)) {
        return makeNanNumber();
    }

    const double eye_mid_x = 0.5 * (eye_left[0] + eye_right[0]);
    const double eye_mid_y = 0.5 * (eye_left[1] + eye_right[1]);
    const double dx = eye_mid_x - bladder[0];
    const double dy = eye_mid_y - bladder[1];
    const double norm_sq = dx * dx + dy * dy;
    if (!(norm_sq > 0.0) || !isFiniteNumber(norm_sq)) {
        return makeNanNumber();
    }

    return std::atan2(dy, dx) * (180.0 / kPi);
}

template <typename T, int Rank>
ts::Result<ts::TensorStore<T, Rank>> openArrayForUpdate(
    const ts::kvstore::KvStore& store,
    const std::string& path,
    const ts::Context& context) {
    auto store_spec = store.spec();
    if (!store_spec.ok()) {
        return store_spec.status();
    }

    auto kv_json_or = store_spec.value().ToJson();
    if (!kv_json_or.ok()) {
        return kv_json_or.status();
    }

    json spec = {
        {"driver", "zarr3"},
        {"kvstore", kv_json_or.value()},
        {"path", path},
    };

    return ts::Open<T, Rank>(spec,
                             ts::OpenMode::open,
                             ts::ReadWriteMode::read_write,
                             context)
        .result();
}

template <typename T>
bool writeScalarRow1D(ts::TensorStore<T, 1>& store,
                      int32_t roi_index,
                      double value,
                      const std::string& array_name,
                      std::string* error_message) {
    const ts::Index shape[1] = {1};
    auto source = ts::AllocateArray<T>(shape);
    source.data()[0] = castValueForWrite<T>(value);

    auto write_result =
        ts::Write(source,
                  store | ts::Dims(0).IndexSlice(static_cast<ts::Index>(roi_index)))
            .commit_future.result();
    if (!write_result.ok()) {
        if (error_message) {
            *error_message = "Failed to write " + array_name + " row " +
                             std::to_string(roi_index) + ": " +
                             write_result.status().ToString();
        }
        return false;
    }
    return true;
}

template <typename T>
bool readScalarRow1D(ts::TensorStore<T, 1>& store,
                     int32_t roi_index,
                     double& value_out,
                     const std::string& array_name,
                     std::string* error_message) {
    auto read_result =
        ts::Read(store | ts::Dims(0).IndexSlice(static_cast<ts::Index>(roi_index)))
            .result();
    if (!read_result.ok()) {
        if (error_message) {
            *error_message = "Failed to read " + array_name + " row " +
                             std::to_string(roi_index) + ": " +
                             read_result.status().ToString();
        }
        return false;
    }

    auto array = read_result.value();
    if (array.rank() != 0) {
        if (error_message) {
            *error_message = "Unexpected rank when reading " + array_name +
                             " row " + std::to_string(roi_index) + ".";
        }
        return false;
    }
    value_out = static_cast<double>(array());
    return true;
}

template <typename T>
bool writeRow2D(ts::TensorStore<T, 2>& store,
                int32_t roi_index,
                const std::vector<double>& row,
                const std::string& array_name,
                std::string* error_message) {
    const ts::Index shape[1] = {static_cast<ts::Index>(row.size())};
    auto source = ts::AllocateArray<T>(shape);
    T* dst = source.data();
    for (double value : row) {
        *dst++ = castValueForWrite<T>(value);
    }

    auto write_result =
        ts::Write(source,
                  store | ts::Dims(0).IndexSlice(static_cast<ts::Index>(roi_index)))
            .commit_future.result();
    if (!write_result.ok()) {
        if (error_message) {
            *error_message = "Failed to write " + array_name + " row " +
                             std::to_string(roi_index) + ": " +
                             write_result.status().ToString();
        }
        return false;
    }
    return true;
}

template <typename T>
bool readRow2D(ts::TensorStore<T, 2>& store,
               int32_t roi_index,
               size_t expected_cols,
               std::vector<double>& row_out,
               const std::string& array_name,
               std::string* error_message) {
    auto read_result =
        ts::Read(store | ts::Dims(0).IndexSlice(static_cast<ts::Index>(roi_index)))
            .result();
    if (!read_result.ok()) {
        if (error_message) {
            *error_message = "Failed to read " + array_name + " row " +
                             std::to_string(roi_index) + ": " +
                             read_result.status().ToString();
        }
        return false;
    }

    auto array = read_result.value();
    if (array.rank() != 1 ||
        static_cast<size_t>(array.shape()[0]) != expected_cols) {
        if (error_message) {
            *error_message = "Unexpected shape when reading " + array_name +
                             " row " + std::to_string(roi_index) + ".";
        }
        return false;
    }

    row_out.resize(expected_cols);
    const T* src = static_cast<const T*>(array.data());
    for (size_t i = 0; i < expected_cols; ++i) {
        row_out[i] = static_cast<double>(src[i]);
    }
    return true;
}

template <typename T>
bool writeKeypointRow3D(ts::TensorStore<T, 3>& store,
                        int32_t roi_index,
                        const std::vector<std::array<double, 2>>& row,
                        const std::string& array_name,
                        std::string* error_message) {
    const ts::Index shape[2] = {static_cast<ts::Index>(row.size()), 2};
    auto source = ts::AllocateArray<T>(shape);

    T* dst = source.data();
    for (const auto& point : row) {
        *dst++ = castValueForWrite<T>(point[0]);
        *dst++ = castValueForWrite<T>(point[1]);
    }

    auto write_result =
        ts::Write(source,
                  store | ts::Dims(0).IndexSlice(static_cast<ts::Index>(roi_index)))
            .commit_future.result();
    if (!write_result.ok()) {
        if (error_message) {
            *error_message = "Failed to write " + array_name + " row " +
                             std::to_string(roi_index) + ": " +
                             write_result.status().ToString();
        }
        return false;
    }
    return true;
}

template <typename T>
bool readKeypointRow3D(ts::TensorStore<T, 3>& store,
                       int32_t roi_index,
                       size_t expected_keypoints,
                       std::vector<std::array<double, 2>>& row_out,
                       const std::string& array_name,
                       std::string* error_message) {
    auto read_result =
        ts::Read(store | ts::Dims(0).IndexSlice(static_cast<ts::Index>(roi_index)))
            .result();
    if (!read_result.ok()) {
        if (error_message) {
            *error_message = "Failed to read " + array_name + " row " +
                             std::to_string(roi_index) + ": " +
                             read_result.status().ToString();
        }
        return false;
    }

    auto array = read_result.value();
    if (array.rank() != 2 || static_cast<size_t>(array.shape()[0]) != expected_keypoints ||
        array.shape()[1] < 2) {
        if (error_message) {
            *error_message = "Unexpected shape when reading " + array_name +
                             " row " + std::to_string(roi_index) + ".";
        }
        return false;
    }

    row_out.assign(expected_keypoints,
                   {std::numeric_limits<double>::quiet_NaN(),
                    std::numeric_limits<double>::quiet_NaN()});
    const size_t cols = static_cast<size_t>(array.shape()[1]);
    const T* src = static_cast<const T*>(array.data());
    for (size_t kp = 0; kp < expected_keypoints; ++kp) {
        row_out[kp][0] = static_cast<double>(src[kp * cols + 0]);
        row_out[kp][1] = static_cast<double>(src[kp * cols + 1]);
    }
    return true;
}

bool readBoolArrayFlexible(const ts::kvstore::KvStore& store,
                           const std::string& path,
                           const ts::Context& context,
                           std::vector<uint8_t>& out) {
    auto bool_store = openArrayAny<bool, 1>(store, path, context);
    if (bool_store.ok()) {
        auto read_result = ts::Read(bool_store.value()).result();
        if (read_result.ok()) {
            auto arr = read_result.value();
            if (arr.rank() == 1) {
                const size_t n = static_cast<size_t>(arr.shape()[0]);
                out.resize(n);
                const bool* ptr = static_cast<const bool*>(arr.data());
                for (size_t i = 0; i < n; ++i) {
                    out[i] = ptr[i] ? 1 : 0;
                }
                return true;
            }
        }
    }

    auto u8_store = openArrayAny<uint8_t, 1>(store, path, context);
    if (u8_store.ok()) {
        auto read_result = ts::Read(u8_store.value()).result();
        if (read_result.ok()) {
            auto arr = read_result.value();
            if (arr.rank() == 1) {
                const size_t n = static_cast<size_t>(arr.shape()[0]);
                out.resize(n);
                const uint8_t* ptr = static_cast<const uint8_t*>(arr.data());
                for (size_t i = 0; i < n; ++i) {
                    out[i] = ptr[i] != 0 ? 1 : 0;
                }
                return true;
            }
        }
    }

    auto i8_store = openArrayAny<int8_t, 1>(store, path, context);
    if (i8_store.ok()) {
        auto read_result = ts::Read(i8_store.value()).result();
        if (read_result.ok()) {
            auto arr = read_result.value();
            if (arr.rank() == 1) {
                const size_t n = static_cast<size_t>(arr.shape()[0]);
                out.resize(n);
                const int8_t* ptr = static_cast<const int8_t*>(arr.data());
                for (size_t i = 0; i < n; ++i) {
                    out[i] = ptr[i] != 0 ? 1 : 0;
                }
                return true;
            }
        }
    }

    auto i32_store = openArrayAny<int32_t, 1>(store, path, context);
    if (i32_store.ok()) {
        auto read_result = ts::Read(i32_store.value()).result();
        if (read_result.ok()) {
            auto arr = read_result.value();
            if (arr.rank() == 1) {
                const size_t n = static_cast<size_t>(arr.shape()[0]);
                out.resize(n);
                const int32_t* ptr = static_cast<const int32_t*>(arr.data());
                for (size_t i = 0; i < n; ++i) {
                    out[i] = ptr[i] != 0 ? 1 : 0;
                }
                return true;
            }
        }
    }

    return false;
}

struct Thresholds {
    double confidence_threshold = 0.3;
    double min_triangle_angle = 10.0;
    double min_triangle_area = 100.0;
    bool has_max_triangle_area = false;
    double max_triangle_area = std::numeric_limits<double>::quiet_NaN();
};

Thresholds parseThresholds(const json& run_attrs) {
    Thresholds t;
    if (!run_attrs.is_object() || !run_attrs.contains("summary_statistics") ||
        !run_attrs["summary_statistics"].is_object()) {
        return t;
    }
    const auto& summary = run_attrs["summary_statistics"];
    if (!summary.contains("refine") || !summary["refine"].is_object()) {
        return t;
    }
    const auto& refine = summary["refine"];

    auto read_num = [&](const char* key, double& target) {
        if (refine.contains(key) && refine[key].is_number()) {
            target = refine[key].get<double>();
        }
    };

    read_num("confidence_threshold", t.confidence_threshold);
    read_num("min_triangle_angle", t.min_triangle_angle);
    read_num("min_triangle_area", t.min_triangle_area);
    if (refine.contains("max_triangle_area") && refine["max_triangle_area"].is_number()) {
        t.has_max_triangle_area = true;
        t.max_triangle_area = refine["max_triangle_area"].get<double>();
    }
    return t;
}

}  // namespace

bool ZarrDetectionLoader::writeManualRefinedKeypoints(
    const std::vector<ManualKeypointRoiWrite>& roi_writes,
    std::string& error_message,
    std::vector<int32_t>* changed_roi_indices) {
    error_message.clear();
    if (changed_roi_indices) {
        changed_roi_indices->clear();
    }

    if (root_path_.empty()) {
        error_message = "No loaded Zarr archive to write into.";
        return false;
    }
    if (roi_writes.empty()) {
        return true;
    }

    std::vector<ManualKeypointRoiWrite> deduped_writes;
    deduped_writes.reserve(roi_writes.size());
    std::unordered_map<int32_t, size_t> dedup_lookup;
    dedup_lookup.reserve(roi_writes.size());
    for (size_t i = 0; i < roi_writes.size(); ++i) {
        const auto& write = roi_writes[i];
        if (write.roi_index < 0) {
            error_message = "roi_writes contains a negative roi_index at row " +
                            std::to_string(i) + ".";
            return false;
        }
        if (write.mark_fish_present_no_keypoints && write.mark_detection_issue) {
            error_message =
                "roi_writes row " + std::to_string(i) +
                " sets both mark_fish_present_no_keypoints and mark_detection_issue.";
            return false;
        }

        auto found = dedup_lookup.find(write.roi_index);
        if (found == dedup_lookup.end()) {
            dedup_lookup.emplace(write.roi_index, deduped_writes.size());
            deduped_writes.push_back(write);
        } else {
            deduped_writes[found->second] = write;
        }
    }

    const std::string kvstore_path = normalizeKvstoreFileRootPath(root_path_);
    auto kv_spec = ts::kvstore::Spec::FromJson(
        {{"driver", "file"}, {"path", kvstore_path}});
    if (!kv_spec.ok()) {
        error_message =
            "Failed to create kvstore spec: " + kv_spec.status().ToString();
        return false;
    }

    auto store_result = ts::kvstore::Open(kv_spec.value(), context_).result();
    if (!store_result.ok()) {
        error_message =
            "Failed to open kvstore: " + store_result.status().ToString();
        return false;
    }
    const auto store = store_result.value();

    auto refined_group_attrs = readAttrsAny(store, "refined_keypoints_runs");
    if (!refined_group_attrs.has_value() || !refined_group_attrs->is_object()) {
        error_message = "refined_keypoints_runs group is missing.";
        return false;
    }

    const std::string refined_run = extractLatestRunName(*refined_group_attrs);
    if (refined_run.empty()) {
        error_message = "refined_keypoints_runs has no latest run pointer.";
        return false;
    }

    const std::string run_base = "refined_keypoints_runs/" + refined_run + "/";
    auto run_meta = readNodeMetaV3(store, run_base);
    if (!run_meta.has_value() || !run_meta->is_object()) {
        error_message = "Failed to read refined keypoint run metadata.";
        return false;
    }
    auto run_group_meta = normalizeGroupMetadataV3(*run_meta);
    auto& run_attrs = run_group_meta["attributes"];
    const Thresholds thresholds = parseThresholds(run_attrs);

    std::vector<int32_t> roi_frame_indices;
    if (!readInt32Array(store, run_base + "frame_indices", roi_frame_indices)) {
        error_message = "Failed to read refined keypoint frame_indices.";
        return false;
    }

    const size_t roi_count = roi_frame_indices.size();
    if (roi_count == 0) {
        error_message = "refined keypoint run has zero ROI rows.";
        return false;
    }

    auto keypoints_roi_double_or =
        openArrayForUpdate<double, 3>(store, run_base + "keypoints_roi", context_);
    auto keypoints_roi_float_or =
        openArrayForUpdate<float, 3>(store, run_base + "keypoints_roi", context_);
    bool keypoints_use_double = false;
    ts::TensorStore<double, 3> keypoints_roi_double;
    ts::TensorStore<float, 3> keypoints_roi_float;
    if (keypoints_roi_double_or.ok()) {
        keypoints_use_double = true;
        keypoints_roi_double = keypoints_roi_double_or.value();
    } else if (keypoints_roi_float_or.ok()) {
        keypoints_use_double = false;
        keypoints_roi_float = keypoints_roi_float_or.value();
    } else {
        error_message = "Failed to open refined keypoints_roi for update.";
        return false;
    }

    const auto roi_shape = keypoints_use_double
        ? keypoints_roi_double.domain().shape()
        : keypoints_roi_float.domain().shape();
    if (roi_shape.size() != 3 ||
        roi_shape[0] != static_cast<ts::Index>(roi_count) || roi_shape[2] < 2) {
        error_message = "Unexpected keypoints_roi shape in refined run.";
        return false;
    }
    const size_t keypoints_per_roi = static_cast<size_t>(roi_shape[1]);

    auto keypoints_img_double_or =
        openArrayForUpdate<double, 3>(store, run_base + "keypoints_img", context_);
    auto keypoints_img_float_or =
        openArrayForUpdate<float, 3>(store, run_base + "keypoints_img", context_);
    bool keypoints_img_use_double = false;
    ts::TensorStore<double, 3> keypoints_img_double;
    ts::TensorStore<float, 3> keypoints_img_float;
    if (keypoints_img_double_or.ok()) {
        keypoints_img_use_double = true;
        keypoints_img_double = keypoints_img_double_or.value();
    } else if (keypoints_img_float_or.ok()) {
        keypoints_img_use_double = false;
        keypoints_img_float = keypoints_img_float_or.value();
    } else {
        error_message = "Failed to open refined keypoints_img for update.";
        return false;
    }

    auto keypoints_norm_double_or =
        openArrayForUpdate<double, 3>(store, run_base + "keypoints_norm", context_);
    auto keypoints_norm_float_or =
        openArrayForUpdate<float, 3>(store, run_base + "keypoints_norm", context_);
    bool keypoints_norm_use_double = false;
    ts::TensorStore<double, 3> keypoints_norm_double;
    ts::TensorStore<float, 3> keypoints_norm_float;
    if (keypoints_norm_double_or.ok()) {
        keypoints_norm_use_double = true;
        keypoints_norm_double = keypoints_norm_double_or.value();
    } else if (keypoints_norm_float_or.ok()) {
        keypoints_norm_use_double = false;
        keypoints_norm_float = keypoints_norm_float_or.value();
    } else {
        error_message = "Failed to open refined keypoints_norm for update.";
        return false;
    }

    auto heading_double_or =
        openArrayForUpdate<double, 1>(store, run_base + "heading", context_);
    auto heading_float_or =
        openArrayForUpdate<float, 1>(store, run_base + "heading", context_);
    bool heading_use_double = false;
    ts::TensorStore<double, 1> heading_double;
    ts::TensorStore<float, 1> heading_float;
    if (heading_double_or.ok()) {
        heading_use_double = true;
        heading_double = heading_double_or.value();
    } else if (heading_float_or.ok()) {
        heading_use_double = false;
        heading_float = heading_float_or.value();
    } else {
        error_message = "Failed to open refined heading for update.";
        return false;
    }

    // Optional/required derivative arrays
    auto keypoint_conf_double_or = openArrayForUpdate<double, 2>(
        store, run_base + "keypoint_confidences", context_);
    auto keypoint_conf_float_or = openArrayForUpdate<float, 2>(
        store, run_base + "keypoint_confidences", context_);
    bool keypoint_conf_use_double = false;
    ts::TensorStore<double, 2> keypoint_conf_double;
    ts::TensorStore<float, 2> keypoint_conf_float;
    if (keypoint_conf_double_or.ok()) {
        keypoint_conf_use_double = true;
        keypoint_conf_double = keypoint_conf_double_or.value();
    } else if (keypoint_conf_float_or.ok()) {
        keypoint_conf_use_double = false;
        keypoint_conf_float = keypoint_conf_float_or.value();
    } else {
        error_message = "Failed to open keypoint_confidences for update.";
        return false;
    }

    auto confidence_double_or =
        openArrayForUpdate<double, 1>(store, run_base + "confidence", context_);
    auto confidence_float_or =
        openArrayForUpdate<float, 1>(store, run_base + "confidence", context_);
    bool confidence_use_double = false;
    ts::TensorStore<double, 1> confidence_double;
    ts::TensorStore<float, 1> confidence_float;
    if (confidence_double_or.ok()) {
        confidence_use_double = true;
        confidence_double = confidence_double_or.value();
    } else if (confidence_float_or.ok()) {
        confidence_use_double = false;
        confidence_float = confidence_float_or.value();
    } else {
        error_message = "Failed to open confidence for update.";
        return false;
    }

    auto tri_area_double_or =
        openArrayForUpdate<double, 1>(store, run_base + "triangle_area", context_);
    auto tri_area_float_or =
        openArrayForUpdate<float, 1>(store, run_base + "triangle_area", context_);
    bool tri_area_use_double = false;
    ts::TensorStore<double, 1> tri_area_double;
    ts::TensorStore<float, 1> tri_area_float;
    if (tri_area_double_or.ok()) {
        tri_area_use_double = true;
        tri_area_double = tri_area_double_or.value();
    } else if (tri_area_float_or.ok()) {
        tri_area_use_double = false;
        tri_area_float = tri_area_float_or.value();
    } else {
        error_message = "Failed to open triangle_area for update.";
        return false;
    }

    auto min_angle_double_or =
        openArrayForUpdate<double, 1>(store, run_base + "min_angle", context_);
    auto min_angle_float_or =
        openArrayForUpdate<float, 1>(store, run_base + "min_angle", context_);
    bool min_angle_use_double = false;
    ts::TensorStore<double, 1> min_angle_double;
    ts::TensorStore<float, 1> min_angle_float;
    if (min_angle_double_or.ok()) {
        min_angle_use_double = true;
        min_angle_double = min_angle_double_or.value();
    } else if (min_angle_float_or.ok()) {
        min_angle_use_double = false;
        min_angle_float = min_angle_float_or.value();
    } else {
        error_message = "Failed to open min_angle for update.";
        return false;
    }

    auto tri_angles_double_or =
        openArrayForUpdate<double, 2>(store, run_base + "triangle_angles", context_);
    auto tri_angles_float_or =
        openArrayForUpdate<float, 2>(store, run_base + "triangle_angles", context_);
    bool tri_angles_use_double = false;
    ts::TensorStore<double, 2> tri_angles_double;
    ts::TensorStore<float, 2> tri_angles_float;
    if (tri_angles_double_or.ok()) {
        tri_angles_use_double = true;
        tri_angles_double = tri_angles_double_or.value();
    } else if (tri_angles_float_or.ok()) {
        tri_angles_use_double = false;
        tri_angles_float = tri_angles_float_or.value();
    } else {
        error_message = "Failed to open triangle_angles for update.";
        return false;
    }

    ts::TensorStore<int8_t, 1> quality_i8;
    ts::TensorStore<uint8_t, 1> quality_u8;
    ts::TensorStore<int16_t, 1> quality_i16;
    ts::TensorStore<uint16_t, 1> quality_u16;
    ts::TensorStore<int32_t, 1> quality_i32;
    ts::TensorStore<uint32_t, 1> quality_u32;
    ts::TensorStore<int64_t, 1> quality_i64;
    ts::TensorStore<uint64_t, 1> quality_u64;
    int quality_mode = -1;
    if (auto quality_i8_or =
            openArrayForUpdate<int8_t, 1>(store, run_base + "quality_labels", context_);
        quality_i8_or.ok()) {
        quality_i8 = quality_i8_or.value();
        quality_mode = 0;
    } else if (auto quality_u8_or =
                   openArrayForUpdate<uint8_t, 1>(
                       store, run_base + "quality_labels", context_);
               quality_u8_or.ok()) {
        quality_u8 = quality_u8_or.value();
        quality_mode = 1;
    } else if (auto quality_i16_or =
                   openArrayForUpdate<int16_t, 1>(
                       store, run_base + "quality_labels", context_);
               quality_i16_or.ok()) {
        quality_i16 = quality_i16_or.value();
        quality_mode = 2;
    } else if (auto quality_u16_or =
                   openArrayForUpdate<uint16_t, 1>(
                       store, run_base + "quality_labels", context_);
               quality_u16_or.ok()) {
        quality_u16 = quality_u16_or.value();
        quality_mode = 3;
    } else if (auto quality_i32_or =
                   openArrayForUpdate<int32_t, 1>(
                       store, run_base + "quality_labels", context_);
               quality_i32_or.ok()) {
        quality_i32 = quality_i32_or.value();
        quality_mode = 4;
    } else if (auto quality_u32_or =
                   openArrayForUpdate<uint32_t, 1>(
                       store, run_base + "quality_labels", context_);
               quality_u32_or.ok()) {
        quality_u32 = quality_u32_or.value();
        quality_mode = 5;
    } else if (auto quality_i64_or =
                   openArrayForUpdate<int64_t, 1>(
                       store, run_base + "quality_labels", context_);
               quality_i64_or.ok()) {
        quality_i64 = quality_i64_or.value();
        quality_mode = 6;
    } else if (auto quality_u64_or =
                   openArrayForUpdate<uint64_t, 1>(
                       store, run_base + "quality_labels", context_);
               quality_u64_or.ok()) {
        quality_u64 = quality_u64_or.value();
        quality_mode = 7;
    } else {
        error_message = "Failed to open quality_labels for update.";
        return false;
    }

    auto open_bool_like = [&](const std::string& path,
                              ts::TensorStore<bool, 1>& bool_store,
                              ts::TensorStore<uint8_t, 1>& u8_store,
                              ts::TensorStore<int8_t, 1>& i8_store,
                              ts::TensorStore<int32_t, 1>& i32_store,
                              int* mode_out) -> bool {
        auto as_bool = openArrayForUpdate<bool, 1>(store, path, context_);
        if (as_bool.ok()) {
            bool_store = as_bool.value();
            *mode_out = 0;
            return true;
        }
        auto as_u8 = openArrayForUpdate<uint8_t, 1>(store, path, context_);
        if (as_u8.ok()) {
            u8_store = as_u8.value();
            *mode_out = 1;
            return true;
        }
        auto as_i8 = openArrayForUpdate<int8_t, 1>(store, path, context_);
        if (as_i8.ok()) {
            i8_store = as_i8.value();
            *mode_out = 2;
            return true;
        }
        auto as_i32 = openArrayForUpdate<int32_t, 1>(store, path, context_);
        if (as_i32.ok()) {
            i32_store = as_i32.value();
            *mode_out = 3;
            return true;
        }
        return false;
    };

    ts::TensorStore<bool, 1> refined_success_b;
    ts::TensorStore<uint8_t, 1> refined_success_u8;
    ts::TensorStore<int8_t, 1> refined_success_i8;
    ts::TensorStore<int32_t, 1> refined_success_i32;
    int refined_success_mode = -1;
    if (!open_bool_like(run_base + "refined_success",
                        refined_success_b,
                        refined_success_u8,
                        refined_success_i8,
                        refined_success_i32,
                        &refined_success_mode)) {
        error_message = "Failed to open refined_success for update.";
        return false;
    }

    ts::TensorStore<bool, 1> flip_corrected_b;
    ts::TensorStore<uint8_t, 1> flip_corrected_u8;
    ts::TensorStore<int8_t, 1> flip_corrected_i8;
    ts::TensorStore<int32_t, 1> flip_corrected_i32;
    int flip_corrected_mode = -1;
    if (!open_bool_like(run_base + "flip_corrected",
                        flip_corrected_b,
                        flip_corrected_u8,
                        flip_corrected_i8,
                        flip_corrected_i32,
                        &flip_corrected_mode)) {
        error_message = "Failed to open flip_corrected for update.";
        return false;
    }

    ts::TensorStore<bool, 1> confidence_valid_b;
    ts::TensorStore<uint8_t, 1> confidence_valid_u8;
    ts::TensorStore<int8_t, 1> confidence_valid_i8;
    ts::TensorStore<int32_t, 1> confidence_valid_i32;
    int confidence_valid_mode = -1;
    if (!open_bool_like(run_base + "confidence_valid",
                        confidence_valid_b,
                        confidence_valid_u8,
                        confidence_valid_i8,
                        confidence_valid_i32,
                        &confidence_valid_mode)) {
        error_message = "Failed to open confidence_valid for update.";
        return false;
    }

    ts::TensorStore<bool, 1> geometry_valid_b;
    ts::TensorStore<uint8_t, 1> geometry_valid_u8;
    ts::TensorStore<int8_t, 1> geometry_valid_i8;
    ts::TensorStore<int32_t, 1> geometry_valid_i32;
    int geometry_valid_mode = -1;
    if (!open_bool_like(run_base + "geometry_valid",
                        geometry_valid_b,
                        geometry_valid_u8,
                        geometry_valid_i8,
                        geometry_valid_i32,
                        &geometry_valid_mode)) {
        error_message = "Failed to open geometry_valid for update.";
        return false;
    }

    ts::TensorStore<bool, 1> usable_b;
    ts::TensorStore<uint8_t, 1> usable_u8;
    ts::TensorStore<int8_t, 1> usable_i8;
    ts::TensorStore<int32_t, 1> usable_i32;
    int usable_mode = -1;
    if (!open_bool_like(run_base + "usable_keypoints",
                        usable_b,
                        usable_u8,
                        usable_i8,
                        usable_i32,
                        &usable_mode)) {
        error_message = "Failed to open usable_keypoints for update.";
        return false;
    }

    ts::TensorStore<bool, 1> heading_finite_b;
    ts::TensorStore<uint8_t, 1> heading_finite_u8;
    ts::TensorStore<int8_t, 1> heading_finite_i8;
    ts::TensorStore<int32_t, 1> heading_finite_i32;
    int heading_finite_mode = -1;
    if (!open_bool_like(run_base + "heading_finite",
                        heading_finite_b,
                        heading_finite_u8,
                        heading_finite_i8,
                        heading_finite_i32,
                        &heading_finite_mode)) {
        error_message = "Failed to open heading_finite for update.";
        return false;
    }

    ts::TensorStore<bool, 1> heading_usable_b;
    ts::TensorStore<uint8_t, 1> heading_usable_u8;
    ts::TensorStore<int8_t, 1> heading_usable_i8;
    ts::TensorStore<int32_t, 1> heading_usable_i32;
    int heading_usable_mode = -1;
    if (!open_bool_like(run_base + "heading_usable",
                        heading_usable_b,
                        heading_usable_u8,
                        heading_usable_i8,
                        heading_usable_i32,
                        &heading_usable_mode)) {
        error_message = "Failed to open heading_usable for update.";
        return false;
    }

    auto write_bool_like = [&](int mode,
                               ts::TensorStore<bool, 1>& b,
                               ts::TensorStore<uint8_t, 1>& u8,
                               ts::TensorStore<int8_t, 1>& i8,
                               ts::TensorStore<int32_t, 1>& i32,
                               int32_t roi,
                               bool value,
                               const std::string& name) -> bool {
        if (mode == 0) {
            return writeScalarRow1D<bool>(b, roi, value ? 1.0 : 0.0, name,
                                          &error_message);
        }
        if (mode == 1) {
            return writeScalarRow1D<uint8_t>(u8, roi, value ? 1.0 : 0.0, name,
                                             &error_message);
        }
        if (mode == 2) {
            return writeScalarRow1D<int8_t>(i8, roi, value ? 1.0 : 0.0, name,
                                            &error_message);
        }
        return writeScalarRow1D<int32_t>(i32, roi, value ? 1.0 : 0.0, name,
                                         &error_message);
    };

    auto read_bool_like = [&](int mode,
                              ts::TensorStore<bool, 1>& b,
                              ts::TensorStore<uint8_t, 1>& u8,
                              ts::TensorStore<int8_t, 1>& i8,
                              ts::TensorStore<int32_t, 1>& i32,
                              int32_t roi,
                              double& value_out,
                              const std::string& name) -> bool {
        if (mode == 0) {
            return readScalarRow1D<bool>(b, roi, value_out, name, &error_message);
        }
        if (mode == 1) {
            return readScalarRow1D<uint8_t>(u8, roi, value_out, name,
                                            &error_message);
        }
        if (mode == 2) {
            return readScalarRow1D<int8_t>(i8, roi, value_out, name,
                                           &error_message);
        }
        return readScalarRow1D<int32_t>(i32, roi, value_out, name,
                                        &error_message);
    };

    auto read_quality_like = [&](int32_t roi, double& value_out) -> bool {
        switch (quality_mode) {
            case 0:
                return readScalarRow1D<int8_t>(
                    quality_i8, roi, value_out, "quality_labels", &error_message);
            case 1:
                return readScalarRow1D<uint8_t>(
                    quality_u8, roi, value_out, "quality_labels", &error_message);
            case 2:
                return readScalarRow1D<int16_t>(
                    quality_i16, roi, value_out, "quality_labels", &error_message);
            case 3:
                return readScalarRow1D<uint16_t>(
                    quality_u16, roi, value_out, "quality_labels", &error_message);
            case 4:
                return readScalarRow1D<int32_t>(
                    quality_i32, roi, value_out, "quality_labels", &error_message);
            case 5:
                return readScalarRow1D<uint32_t>(
                    quality_u32, roi, value_out, "quality_labels", &error_message);
            case 6:
                return readScalarRow1D<int64_t>(
                    quality_i64, roi, value_out, "quality_labels", &error_message);
            case 7:
                return readScalarRow1D<uint64_t>(
                    quality_u64, roi, value_out, "quality_labels", &error_message);
            default:
                error_message = "Invalid quality_labels mode.";
                return false;
        }
    };

    auto write_quality_like = [&](int32_t roi, int32_t value) -> bool {
        switch (quality_mode) {
            case 0:
                return writeScalarRow1D<int8_t>(
                    quality_i8, roi, static_cast<double>(value), "quality_labels", &error_message);
            case 1:
                return writeScalarRow1D<uint8_t>(
                    quality_u8, roi, static_cast<double>(value), "quality_labels", &error_message);
            case 2:
                return writeScalarRow1D<int16_t>(
                    quality_i16, roi, static_cast<double>(value), "quality_labels", &error_message);
            case 3:
                return writeScalarRow1D<uint16_t>(
                    quality_u16, roi, static_cast<double>(value), "quality_labels", &error_message);
            case 4:
                return writeScalarRow1D<int32_t>(
                    quality_i32, roi, static_cast<double>(value), "quality_labels", &error_message);
            case 5:
                return writeScalarRow1D<uint32_t>(
                    quality_u32, roi, static_cast<double>(value), "quality_labels", &error_message);
            case 6:
                return writeScalarRow1D<int64_t>(
                    quality_i64, roi, static_cast<double>(value), "quality_labels", &error_message);
            case 7:
                return writeScalarRow1D<uint64_t>(
                    quality_u64, roi, static_cast<double>(value), "quality_labels", &error_message);
            default:
                error_message = "Invalid quality_labels mode.";
                return false;
        }
    };

    std::vector<int32_t> detection_source_i32;
    if (!readInt32Array(store, run_base + "detection_source", detection_source_i32) ||
        detection_source_i32.size() != roi_count) {
        std::vector<uint8_t> detection_source_bool;
        if (readBoolArrayFlexible(store,
                                  run_base + "detection_source",
                                  context_,
                                  detection_source_bool) &&
            detection_source_bool.size() == roi_count) {
            detection_source_i32.assign(roi_count, 0);
            for (size_t i = 0; i < roi_count; ++i) {
                detection_source_i32[i] = detection_source_bool[i] ? 1 : 0;
            }
        } else {
            detection_source_i32.assign(roi_count, 0);
        }
    }

    // Reason channels
    bool reason_bytes_available = false;
    bool reason_bytes_is_u8 = false;
    ts::TensorStore<uint8_t, 2> reason_bytes_u8;
    ts::TensorStore<char, 2> reason_bytes_char;
    size_t reason_bytes_width = 0;

    auto reason_bytes_u8_or =
        openArrayForUpdate<uint8_t, 2>(store, run_base + "reason_bytes", context_);
    if (reason_bytes_u8_or.ok()) {
        reason_bytes_available = true;
        reason_bytes_is_u8 = true;
        reason_bytes_u8 = reason_bytes_u8_or.value();
        reason_bytes_width = static_cast<size_t>(reason_bytes_u8.domain().shape()[1]);
    } else {
        auto reason_bytes_char_or =
            openArrayForUpdate<char, 2>(store, run_base + "reason_bytes", context_);
        if (reason_bytes_char_or.ok()) {
            reason_bytes_available = true;
            reason_bytes_is_u8 = false;
            reason_bytes_char = reason_bytes_char_or.value();
            reason_bytes_width =
                static_cast<size_t>(reason_bytes_char.domain().shape()[1]);
        }
    }

    bool reason_text_available = false;
    ts::TensorStore<std::string, 1> reason_text_store;
    if (arrayExists(store, run_base + "reason")) {
        auto reason_text_or =
            openArrayForUpdate<std::string, 1>(store, run_base + "reason", context_);
        if (reason_text_or.ok()) {
            reason_text_available = true;
            reason_text_store = reason_text_or.value();
        }
    }

    if (!reason_bytes_available && !reason_text_available) {
        error_message = "Neither reason_bytes nor writable reason dataset is available.";
        return false;
    }

    std::vector<std::string> reason_values(roi_count, std::string{});
    bool reason_loaded = false;
    if (reason_bytes_available) {
        if (reason_bytes_is_u8) {
            auto read_result = ts::Read(reason_bytes_u8).result();
            if (read_result.ok()) {
                auto arr = read_result.value();
                if (arr.rank() == 2 && static_cast<size_t>(arr.shape()[0]) == roi_count) {
                    const size_t width = static_cast<size_t>(arr.shape()[1]);
                    const uint8_t* ptr = static_cast<const uint8_t*>(arr.data());
                    for (size_t row = 0; row < roi_count; ++row) {
                        size_t len = 0;
                        while (len < width && ptr[row * width + len] != 0) {
                            ++len;
                        }
                        reason_values[row] = std::string(
                            reinterpret_cast<const char*>(ptr + row * width),
                            reinterpret_cast<const char*>(ptr + row * width + len));
                    }
                    reason_loaded = true;
                }
            }
        } else {
            auto read_result = ts::Read(reason_bytes_char).result();
            if (read_result.ok()) {
                auto arr = read_result.value();
                if (arr.rank() == 2 && static_cast<size_t>(arr.shape()[0]) == roi_count) {
                    const size_t width = static_cast<size_t>(arr.shape()[1]);
                    const char* ptr = static_cast<const char*>(arr.data());
                    for (size_t row = 0; row < roi_count; ++row) {
                        size_t len = 0;
                        while (len < width && ptr[row * width + len] != '\0') {
                            ++len;
                        }
                        reason_values[row] = std::string(ptr + row * width,
                                                         ptr + row * width + len);
                    }
                    reason_loaded = true;
                }
            }
        }
    }
    if (!reason_loaded && reason_text_available) {
        auto read_result = ts::Read(reason_text_store).result();
        if (read_result.ok()) {
            auto arr = read_result.value();
            if (arr.rank() == 1 && static_cast<size_t>(arr.shape()[0]) == roi_count) {
                reason_values.resize(roi_count);
                for (size_t i = 0; i < roi_count; ++i) {
                    reason_values[i] = arr(static_cast<ts::Index>(i));
                }
                reason_loaded = true;
            }
        }
    }

    if (!reason_loaded) {
        reason_values.assign(roi_count, std::string{});
    }

    std::vector<std::array<double, 2>> roi_offsets(
        roi_count,
        {std::numeric_limits<double>::quiet_NaN(),
         std::numeric_limits<double>::quiet_NaN()});

    if (data_.keypoint_detection_index_by_roi.size() == roi_count &&
        data_.roi_offset_x.size() == data_.bbox_norm_coords.size() &&
        data_.roi_offset_y.size() == data_.bbox_norm_coords.size()) {
        for (size_t roi = 0; roi < roi_count; ++roi) {
            const int32_t det_index = data_.keypoint_detection_index_by_roi[roi];
            if (det_index < 0) {
                continue;
            }
            const size_t det = static_cast<size_t>(det_index);
            if (det >= data_.roi_offset_x.size() || det >= data_.roi_offset_y.size()) {
                continue;
            }
            const double ox = static_cast<double>(data_.roi_offset_x[det]);
            const double oy = static_cast<double>(data_.roi_offset_y[det]);
            if (isFiniteNumber(ox) && isFiniteNumber(oy)) {
                roi_offsets[roi] = {ox, oy};
            }
        }
    }

    bool need_crop_offsets = false;
    for (const auto& write : deduped_writes) {
        if (write.mark_fish_present_no_keypoints || write.mark_detection_issue) {
            continue;
        }
        if (static_cast<size_t>(write.roi_index) >= roi_offsets.size() ||
            !isFinitePair(roi_offsets[write.roi_index])) {
            need_crop_offsets = true;
            break;
        }
    }

    if (need_crop_offsets && !data_.keypoints_source_crop_run.empty()) {
        const std::string crop_path = "crop_runs/" +
                                      NormalizeCropRunName(data_.keypoints_source_crop_run) +
                                      "/roi_coordinates_full";

        auto roi_float = openArrayAny<float, 2>(store, crop_path, context_);
        if (roi_float.ok()) {
            auto read_result = ts::Read(roi_float.value()).result();
            if (read_result.ok()) {
                auto array = read_result.value();
                auto shape = array.shape();
                if (shape.size() == 2 &&
                    shape[0] == static_cast<ts::Index>(roi_count) &&
                    shape[1] >= 2) {
                    const size_t cols = static_cast<size_t>(shape[1]);
                    const float* ptr = static_cast<const float*>(array.data());
                    for (size_t roi = 0; roi < roi_count; ++roi) {
                        if (isFinitePair(roi_offsets[roi])) {
                            continue;
                        }
                        const double ox = static_cast<double>(ptr[roi * cols + 0]);
                        const double oy = static_cast<double>(ptr[roi * cols + 1]);
                        if (isFiniteNumber(ox) && isFiniteNumber(oy)) {
                            roi_offsets[roi] = {ox, oy};
                        }
                    }
                }
            }
        }

        auto roi_int = openArrayAny<int32_t, 2>(store, crop_path, context_);
        if (roi_int.ok()) {
            auto read_result = ts::Read(roi_int.value()).result();
            if (read_result.ok()) {
                auto array = read_result.value();
                auto shape = array.shape();
                if (shape.size() == 2 &&
                    shape[0] == static_cast<ts::Index>(roi_count) &&
                    shape[1] >= 2) {
                    const size_t cols = static_cast<size_t>(shape[1]);
                    const int32_t* ptr = static_cast<const int32_t*>(array.data());
                    for (size_t roi = 0; roi < roi_count; ++roi) {
                        if (isFinitePair(roi_offsets[roi])) {
                            continue;
                        }
                        const double ox = static_cast<double>(ptr[roi * cols + 0]);
                        const double oy = static_cast<double>(ptr[roi * cols + 1]);
                        if (isFiniteNumber(ox) && isFiniteNumber(oy)) {
                            roi_offsets[roi] = {ox, oy};
                        }
                    }
                }
            }
        }
    }

    const double image_width = static_cast<double>(data_.image_width);
    const double image_height = static_cast<double>(data_.image_height);

    bool any_row_changed = false;
    bool any_reason_changed = false;
    bool reason_bytes_needs_resize = false;

    std::vector<uint8_t> changed_row_mask(roi_count, 0);

    for (const auto& write : deduped_writes) {
        if (write.roi_index < 0 ||
            static_cast<size_t>(write.roi_index) >= roi_count) {
            error_message = "roi_index " + std::to_string(write.roi_index) +
                            " is outside refined keypoint ROI range.";
            return false;
        }
        const int32_t roi = write.roi_index;

        const double nan_value = makeNanNumber();
        std::vector<std::array<double, 2>> row_roi(
            keypoints_per_roi, {nan_value, nan_value});
        std::vector<std::array<double, 2>> row_img(
            keypoints_per_roi, {nan_value, nan_value});
        std::vector<std::array<double, 2>> row_norm(
            keypoints_per_roi, {nan_value, nan_value});

        RowReasonAction reason_action = RowReasonAction::kManualCorrection;

        if (write.mark_fish_present_no_keypoints || write.mark_detection_issue) {
            reason_action = write.mark_detection_issue
                                ? RowReasonAction::kDetectionIssue
                                : RowReasonAction::kFishPresentNoKeypoints;
        } else {
            if (write.keypoints_roi.empty()) {
                error_message = "roi_index " + std::to_string(roi) +
                                " has no keypoints_roi payload.";
                return false;
            }
            if (write.keypoints_roi.size() != keypoints_per_roi) {
                error_message = "roi_index " + std::to_string(roi) +
                                " keypoints_roi size mismatch.";
                return false;
            }
            for (size_t kp = 0; kp < keypoints_per_roi; ++kp) {
                const auto& src = write.keypoints_roi[kp];
                if (isFiniteNumber(src[0]) && isFiniteNumber(src[1])) {
                    row_roi[kp] = src;
                }
            }
        }

        if (reason_action == RowReasonAction::kManualCorrection &&
            anyFiniteKeypoint(row_roi)) {
            const auto offset = roi_offsets[roi];
            if (!isFinitePair(offset)) {
                error_message = "Missing ROI offset for roi_index " +
                                std::to_string(roi) + ".";
                return false;
            }
            if (!(image_width > 0.0) || !(image_height > 0.0)) {
                error_message =
                    "Image dimensions unavailable for keypoints_norm computation.";
                return false;
            }
            for (size_t kp = 0; kp < keypoints_per_roi; ++kp) {
                if (!isFinitePair(row_roi[kp])) {
                    continue;
                }
                const double img_x = row_roi[kp][0] + offset[0];
                const double img_y = row_roi[kp][1] + offset[1];
                row_img[kp] = {img_x, img_y};
                row_norm[kp] = {img_x / image_width, img_y / image_height};
            }
        }

        const double heading_value = computeHeadingFromPoints(row_roi);
        TriangleMetrics metrics = computeTriangleMetrics(row_roi);

        bool geometry_valid = false;
        bool confidence_valid = false;
        bool usable_keypoints = false;
        bool refined_success = false;

        std::vector<double> keypoint_conf_row(keypoints_per_roi, nan_value);
        double confidence_value = nan_value;

        if (reason_action == RowReasonAction::kManualCorrection) {
            std::fill(keypoint_conf_row.begin(), keypoint_conf_row.end(), 1.0);
            confidence_value = 1.0;
            refined_success = true;
            confidence_valid = true;
            for (double kp_conf : keypoint_conf_row) {
                if (!isFiniteNumber(kp_conf) ||
                    kp_conf < thresholds.confidence_threshold) {
                    confidence_valid = false;
                    break;
                }
            }

            bool area_valid = isFiniteNumber(metrics.area) &&
                              (metrics.area >= thresholds.min_triangle_area);
            if (area_valid && thresholds.has_max_triangle_area) {
                area_valid = metrics.area <= thresholds.max_triangle_area;
            }
            const bool angle_valid =
                isFiniteNumber(metrics.min_angle) &&
                (metrics.min_angle >= thresholds.min_triangle_angle);
            geometry_valid = area_valid && angle_valid;
            usable_keypoints = confidence_valid && geometry_valid;
        }

        const int32_t detection_source =
            (static_cast<size_t>(roi) < detection_source_i32.size())
                ? detection_source_i32[roi]
                : 0;
        const bool heading_finite = isFiniteNumber(heading_value);
        const bool heading_usable =
            refined_success && detection_source == 0 && heading_finite;

        const std::string existing_reason =
            (static_cast<size_t>(roi) < reason_values.size())
                ? reason_values[roi]
                : std::string{};
        const std::string next_reason =
            buildReasonLabel(existing_reason, reason_action, geometry_valid);

        bool row_changed = false;

        {
            std::vector<std::array<double, 2>> existing;
            if (keypoints_use_double) {
                if (!readKeypointRow3D<double>(keypoints_roi_double,
                                               roi,
                                               keypoints_per_roi,
                                               existing,
                                               "keypoints_roi",
                                               &error_message)) {
                    return false;
                }
                if (!nanAwareEqualPointRows(existing, row_roi)) {
                    if (!writeKeypointRow3D<double>(keypoints_roi_double,
                                                    roi,
                                                    row_roi,
                                                    "keypoints_roi",
                                                    &error_message)) {
                        return false;
                    }
                    row_changed = true;
                }
            } else {
                if (!readKeypointRow3D<float>(keypoints_roi_float,
                                              roi,
                                              keypoints_per_roi,
                                              existing,
                                              "keypoints_roi",
                                              &error_message)) {
                    return false;
                }
                if (!nanAwareEqualPointRows(existing, row_roi)) {
                    if (!writeKeypointRow3D<float>(keypoints_roi_float,
                                                   roi,
                                                   row_roi,
                                                   "keypoints_roi",
                                                   &error_message)) {
                        return false;
                    }
                    row_changed = true;
                }
            }
        }

        {
            std::vector<std::array<double, 2>> existing;
            if (keypoints_img_use_double) {
                if (!readKeypointRow3D<double>(keypoints_img_double,
                                               roi,
                                               keypoints_per_roi,
                                               existing,
                                               "keypoints_img",
                                               &error_message)) {
                    return false;
                }
                if (!nanAwareEqualPointRows(existing, row_img)) {
                    if (!writeKeypointRow3D<double>(keypoints_img_double,
                                                    roi,
                                                    row_img,
                                                    "keypoints_img",
                                                    &error_message)) {
                        return false;
                    }
                    row_changed = true;
                }
            } else {
                if (!readKeypointRow3D<float>(keypoints_img_float,
                                              roi,
                                              keypoints_per_roi,
                                              existing,
                                              "keypoints_img",
                                              &error_message)) {
                    return false;
                }
                if (!nanAwareEqualPointRows(existing, row_img)) {
                    if (!writeKeypointRow3D<float>(keypoints_img_float,
                                                   roi,
                                                   row_img,
                                                   "keypoints_img",
                                                   &error_message)) {
                        return false;
                    }
                    row_changed = true;
                }
            }
        }

        {
            std::vector<std::array<double, 2>> existing;
            if (keypoints_norm_use_double) {
                if (!readKeypointRow3D<double>(keypoints_norm_double,
                                               roi,
                                               keypoints_per_roi,
                                               existing,
                                               "keypoints_norm",
                                               &error_message)) {
                    return false;
                }
                if (!nanAwareEqualPointRows(existing, row_norm)) {
                    if (!writeKeypointRow3D<double>(keypoints_norm_double,
                                                    roi,
                                                    row_norm,
                                                    "keypoints_norm",
                                                    &error_message)) {
                        return false;
                    }
                    row_changed = true;
                }
            } else {
                if (!readKeypointRow3D<float>(keypoints_norm_float,
                                              roi,
                                              keypoints_per_roi,
                                              existing,
                                              "keypoints_norm",
                                              &error_message)) {
                    return false;
                }
                if (!nanAwareEqualPointRows(existing, row_norm)) {
                    if (!writeKeypointRow3D<float>(keypoints_norm_float,
                                                   roi,
                                                   row_norm,
                                                   "keypoints_norm",
                                                   &error_message)) {
                        return false;
                    }
                    row_changed = true;
                }
            }
        }

        {
            double existing = nan_value;
            if (heading_use_double) {
                if (!readScalarRow1D<double>(heading_double,
                                             roi,
                                             existing,
                                             "heading",
                                             &error_message)) {
                    return false;
                }
                if (!nanAwareEqual(existing, heading_value)) {
                    if (!writeScalarRow1D<double>(heading_double,
                                                  roi,
                                                  heading_value,
                                                  "heading",
                                                  &error_message)) {
                        return false;
                    }
                    row_changed = true;
                }
            } else {
                if (!readScalarRow1D<float>(heading_float,
                                            roi,
                                            existing,
                                            "heading",
                                            &error_message)) {
                    return false;
                }
                if (!nanAwareEqual(existing, heading_value)) {
                    if (!writeScalarRow1D<float>(heading_float,
                                                 roi,
                                                 heading_value,
                                                 "heading",
                                                 &error_message)) {
                        return false;
                    }
                    row_changed = true;
                }
            }
        }

        {
            std::vector<double> existing;
            if (keypoint_conf_use_double) {
                if (!readRow2D<double>(keypoint_conf_double,
                                       roi,
                                       keypoints_per_roi,
                                       existing,
                                       "keypoint_confidences",
                                       &error_message)) {
                    return false;
                }
                if (!nanAwareEqualRows(existing, keypoint_conf_row)) {
                    if (!writeRow2D<double>(keypoint_conf_double,
                                            roi,
                                            keypoint_conf_row,
                                            "keypoint_confidences",
                                            &error_message)) {
                        return false;
                    }
                    row_changed = true;
                }
            } else {
                if (!readRow2D<float>(keypoint_conf_float,
                                      roi,
                                      keypoints_per_roi,
                                      existing,
                                      "keypoint_confidences",
                                      &error_message)) {
                    return false;
                }
                if (!nanAwareEqualRows(existing, keypoint_conf_row)) {
                    if (!writeRow2D<float>(keypoint_conf_float,
                                           roi,
                                           keypoint_conf_row,
                                           "keypoint_confidences",
                                           &error_message)) {
                        return false;
                    }
                    row_changed = true;
                }
            }
        }

        auto write_if_scalar_changed = [&](double new_value,
                                           const std::string& name,
                                           bool use_double,
                                           ts::TensorStore<double, 1>& d,
                                           ts::TensorStore<float, 1>& f) -> bool {
            double existing = nan_value;
            if (use_double) {
                if (!readScalarRow1D<double>(d, roi, existing, name, &error_message)) {
                    return false;
                }
                if (!nanAwareEqual(existing, new_value)) {
                    if (!writeScalarRow1D<double>(d,
                                                  roi,
                                                  new_value,
                                                  name,
                                                  &error_message)) {
                        return false;
                    }
                    row_changed = true;
                }
                return true;
            }
            if (!readScalarRow1D<float>(f, roi, existing, name, &error_message)) {
                return false;
            }
            if (!nanAwareEqual(existing, new_value)) {
                if (!writeScalarRow1D<float>(f,
                                             roi,
                                             new_value,
                                             name,
                                             &error_message)) {
                    return false;
                }
                row_changed = true;
            }
            return true;
        };

        if (!write_if_scalar_changed(confidence_value,
                                     "confidence",
                                     confidence_use_double,
                                     confidence_double,
                                     confidence_float)) {
            return false;
        }
        if (!write_if_scalar_changed(metrics.area,
                                     "triangle_area",
                                     tri_area_use_double,
                                     tri_area_double,
                                     tri_area_float)) {
            return false;
        }
        if (!write_if_scalar_changed(metrics.min_angle,
                                     "min_angle",
                                     min_angle_use_double,
                                     min_angle_double,
                                     min_angle_float)) {
            return false;
        }

        {
            std::vector<double> new_angles = {
                metrics.angles[0], metrics.angles[1], metrics.angles[2]};
            std::vector<double> existing;
            if (tri_angles_use_double) {
                if (!readRow2D<double>(tri_angles_double,
                                       roi,
                                       3,
                                       existing,
                                       "triangle_angles",
                                       &error_message)) {
                    return false;
                }
                if (!nanAwareEqualRows(existing, new_angles)) {
                    if (!writeRow2D<double>(tri_angles_double,
                                            roi,
                                            new_angles,
                                            "triangle_angles",
                                            &error_message)) {
                        return false;
                    }
                    row_changed = true;
                }
            } else {
                if (!readRow2D<float>(tri_angles_float,
                                      roi,
                                      3,
                                      existing,
                                      "triangle_angles",
                                      &error_message)) {
                    return false;
                }
                if (!nanAwareEqualRows(existing, new_angles)) {
                    if (!writeRow2D<float>(tri_angles_float,
                                           roi,
                                           new_angles,
                                           "triangle_angles",
                                           &error_message)) {
                        return false;
                    }
                    row_changed = true;
                }
            }
        }

        {
            double existing_quality = nan_value;
            if (!read_quality_like(roi, existing_quality)) {
                return false;
            }
            if (static_cast<int32_t>(existing_quality) != 0) {
                if (!write_quality_like(roi, 0)) {
                    return false;
                }
                row_changed = true;
            }
        }

        auto write_bool_field = [&](bool value,
                                    int mode,
                                    ts::TensorStore<bool, 1>& b,
                                    ts::TensorStore<uint8_t, 1>& u8,
                                    ts::TensorStore<int8_t, 1>& i8,
                                    ts::TensorStore<int32_t, 1>& i32,
                                    const std::string& name) -> bool {
            double existing = 0.0;
            if (!read_bool_like(mode, b, u8, i8, i32, roi, existing, name)) {
                return false;
            }
            const bool existing_b = existing != 0.0;
            if (existing_b == value) {
                return true;
            }
            if (!write_bool_like(mode, b, u8, i8, i32, roi, value, name)) {
                return false;
            }
            row_changed = true;
            return true;
        };

        if (!write_bool_field(refined_success,
                              refined_success_mode,
                              refined_success_b,
                              refined_success_u8,
                              refined_success_i8,
                              refined_success_i32,
                              "refined_success")) {
            return false;
        }
        if (!write_bool_field(false,
                              flip_corrected_mode,
                              flip_corrected_b,
                              flip_corrected_u8,
                              flip_corrected_i8,
                              flip_corrected_i32,
                              "flip_corrected")) {
            return false;
        }
        if (!write_bool_field(confidence_valid,
                              confidence_valid_mode,
                              confidence_valid_b,
                              confidence_valid_u8,
                              confidence_valid_i8,
                              confidence_valid_i32,
                              "confidence_valid")) {
            return false;
        }
        if (!write_bool_field(geometry_valid,
                              geometry_valid_mode,
                              geometry_valid_b,
                              geometry_valid_u8,
                              geometry_valid_i8,
                              geometry_valid_i32,
                              "geometry_valid")) {
            return false;
        }
        if (!write_bool_field(usable_keypoints,
                              usable_mode,
                              usable_b,
                              usable_u8,
                              usable_i8,
                              usable_i32,
                              "usable_keypoints")) {
            return false;
        }
        if (!write_bool_field(heading_finite,
                              heading_finite_mode,
                              heading_finite_b,
                              heading_finite_u8,
                              heading_finite_i8,
                              heading_finite_i32,
                              "heading_finite")) {
            return false;
        }
        if (!write_bool_field(heading_usable,
                              heading_usable_mode,
                              heading_usable_b,
                              heading_usable_u8,
                              heading_usable_i8,
                              heading_usable_i32,
                              "heading_usable")) {
            return false;
        }

        if (next_reason != existing_reason) {
            any_reason_changed = true;
            row_changed = true;
            reason_values[roi] = next_reason;

            if (reason_bytes_available && next_reason.size() + 1 > reason_bytes_width) {
                reason_bytes_needs_resize = true;
            }

            if (reason_text_available) {
                const ts::Index shape[1] = {1};
                auto source = ts::AllocateArray<std::string>(shape);
                source.data()[0] = next_reason;
                auto write_result =
                    ts::Write(source,
                              reason_text_store |
                                  ts::Dims(0).IndexSlice(static_cast<ts::Index>(roi)))
                        .commit_future.result();
                if (!write_result.ok()) {
                    error_message = "Failed to write reason row " +
                                    std::to_string(roi) + ": " +
                                    write_result.status().ToString();
                    return false;
                }
            }
        }

        if (row_changed) {
            any_row_changed = true;
            changed_row_mask[roi] = 1;
            if (changed_roi_indices) {
                changed_roi_indices->push_back(roi);
            }
        }
    }

    if (!any_row_changed) {
        return true;
    }

    if (reason_bytes_available && any_reason_changed) {
        if (reason_bytes_needs_resize) {
            size_t new_width = reason_bytes_width;
            for (const auto& reason : reason_values) {
                new_width = std::max(new_width, reason.size() + 1);
            }
            const ts::Index det_chunk = std::max<ts::Index>(
                1, std::min<ts::Index>(1000, static_cast<ts::Index>(roi_count)));
            if (reason_bytes_is_u8) {
                std::vector<uint8_t> bytes(roi_count * new_width, 0);
                for (size_t i = 0; i < roi_count; ++i) {
                    const auto& reason = reason_values[i];
                    const size_t row_offset = i * new_width;
                    const size_t copy_len =
                        std::min(reason.size(), new_width - 1);
                    if (copy_len > 0) {
                        std::memcpy(bytes.data() + row_offset,
                                    reason.data(),
                                    copy_len);
                    }
                    bytes[row_offset + copy_len] = 0;
                }
                if (!writeNumericArray2DFlat<uint8_t>(
                        store,
                        context_,
                        run_base + "reason_bytes",
                        "uint8",
                        bytes,
                        static_cast<ts::Index>(roi_count),
                        static_cast<ts::Index>(new_width),
                        det_chunk,
                        static_cast<ts::Index>(new_width),
                        0,
                        false,
                        &error_message)) {
                    return false;
                }
            } else {
                std::vector<char> bytes(roi_count * new_width, '\0');
                for (size_t i = 0; i < roi_count; ++i) {
                    const auto& reason = reason_values[i];
                    const size_t row_offset = i * new_width;
                    const size_t copy_len =
                        std::min(reason.size(), new_width - 1);
                    if (copy_len > 0) {
                        std::memcpy(bytes.data() + row_offset,
                                    reason.data(),
                                    copy_len);
                    }
                    bytes[row_offset + copy_len] = '\0';
                }
                if (!writeNumericArray2DFlat<char>(
                        store,
                        context_,
                        run_base + "reason_bytes",
                        "int8",
                        bytes,
                        static_cast<ts::Index>(roi_count),
                        static_cast<ts::Index>(new_width),
                        det_chunk,
                        static_cast<ts::Index>(new_width),
                        0,
                        false,
                        &error_message)) {
                    return false;
                }
            }
        } else {
            for (size_t roi = 0; roi < roi_count; ++roi) {
                if (changed_row_mask[roi] == 0) {
                    continue;
                }
                const std::string& reason = reason_values[roi];
                if (reason_bytes_is_u8) {
                    const ts::Index shape[1] = {static_cast<ts::Index>(reason_bytes_width)};
                    auto source = ts::AllocateArray<uint8_t>(shape);
                    std::fill(source.data(), source.data() + reason_bytes_width, uint8_t{0});
                    const size_t copy_len =
                        std::min(reason.size(), reason_bytes_width > 0 ? reason_bytes_width - 1 : 0);
                    if (copy_len > 0) {
                        std::memcpy(source.data(), reason.data(), copy_len);
                    }
                    auto write_result =
                        ts::Write(source,
                                  reason_bytes_u8 |
                                      ts::Dims(0).IndexSlice(static_cast<ts::Index>(roi)))
                            .commit_future.result();
                    if (!write_result.ok()) {
                        error_message = "Failed to write reason_bytes row " +
                                        std::to_string(roi) + ": " +
                                        write_result.status().ToString();
                        return false;
                    }
                } else {
                    const ts::Index shape[1] = {static_cast<ts::Index>(reason_bytes_width)};
                    auto source = ts::AllocateArray<char>(shape);
                    std::fill(source.data(), source.data() + reason_bytes_width, '\0');
                    const size_t copy_len =
                        std::min(reason.size(), reason_bytes_width > 0 ? reason_bytes_width - 1 : 0);
                    if (copy_len > 0) {
                        std::memcpy(source.data(), reason.data(), copy_len);
                    }
                    auto write_result =
                        ts::Write(source,
                                  reason_bytes_char |
                                      ts::Dims(0).IndexSlice(static_cast<ts::Index>(roi)))
                            .commit_future.result();
                    if (!write_result.ok()) {
                        error_message = "Failed to write reason_bytes row " +
                                        std::to_string(roi) + ": " +
                                        write_result.status().ToString();
                        return false;
                    }
                }
            }
        }
    }

    // Refresh summary_statistics.postprocess only when payload changes.
    json postprocess;
    postprocess["total_rois"] = static_cast<int64_t>(roi_count);

    std::vector<uint8_t> source_success;
    int64_t source_success_count = 0;
    if (readBoolArrayFlexible(store,
                              run_base + "source_success",
                              context_,
                              source_success)) {
        for (uint8_t v : source_success) {
            source_success_count += (v != 0) ? 1 : 0;
        }
    }
    postprocess["source_success"] = source_success_count;

    auto count_true = [&](const std::string& path) -> int64_t {
        std::vector<uint8_t> values;
        if (!readBoolArrayFlexible(store, path, context_, values)) {
            return 0;
        }
        int64_t count = 0;
        for (uint8_t v : values) {
            count += (v != 0) ? 1 : 0;
        }
        return count;
    };

    const int64_t refined_success_count =
        count_true(run_base + "refined_success");
    const int64_t usable_count = count_true(run_base + "usable_keypoints");
    const int64_t confidence_valid_count =
        count_true(run_base + "confidence_valid");
    const int64_t geometry_valid_count =
        count_true(run_base + "geometry_valid");
    const int64_t flip_corrected_count =
        count_true(run_base + "flip_corrected");
    const int64_t heading_finite_count =
        count_true(run_base + "heading_finite");
    const int64_t heading_usable_count =
        count_true(run_base + "heading_usable");

    postprocess["refined_success"] = refined_success_count;
    postprocess["remaining_failures"] =
        static_cast<int64_t>(roi_count) - refined_success_count;
    postprocess["success_rate_percent"] =
        (roi_count > 0)
            ? (100.0 * static_cast<double>(refined_success_count) /
               static_cast<double>(roi_count))
            : 0.0;
    postprocess["usable_keypoints"] = usable_count;
    postprocess["confidence_valid"] = confidence_valid_count;
    postprocess["geometry_valid"] = geometry_valid_count;
    postprocess["flip_corrected"] = flip_corrected_count;
    postprocess["heading_finite"] = heading_finite_count;
    postprocess["heading_usable"] = heading_usable_count;

    std::unordered_map<std::string, int64_t> detection_source_counts_map;
    for (int32_t v : detection_source_i32) {
        detection_source_counts_map[std::to_string(v)]++;
    }
    json detection_source_counts = json::object();
    for (const auto& kv : detection_source_counts_map) {
        detection_source_counts[kv.first] = kv.second;
    }
    postprocess["detection_source_counts"] = detection_source_counts;

    json reason_counts = json::object();
    for (const auto& reason : reason_values) {
        for (const auto& tag : splitReasonTags(reason)) {
            if (tag.empty()) {
                continue;
            }
            if (!reason_counts.contains(tag)) {
                reason_counts[tag] = 0;
            }
            reason_counts[tag] = reason_counts[tag].get<int64_t>() + 1;
        }
    }
    postprocess["reason_counts"] = reason_counts;

    std::vector<int32_t> retune_ids;
    if (!readInt32Array(store, run_base + "retune_id", retune_ids) ||
        retune_ids.size() != roi_count) {
        retune_ids.assign(roi_count, -1);
    }
    std::unordered_map<std::string, int64_t> retune_counts_map;
    int64_t retune_total = 0;
    for (int32_t v : retune_ids) {
        const std::string key = std::to_string(v);
        retune_counts_map[key]++;
        if (v != -1) {
            retune_total++;
        }
    }
    json retune_counts = json::object();
    for (const auto& kv : retune_counts_map) {
        retune_counts[kv.first] = kv.second;
    }
    postprocess["retune_id_counts"] = retune_counts;
    postprocess["retune_total"] = retune_total;

    int64_t manual_corrections = 0;
    if (reason_counts.contains("manual_correction") &&
        reason_counts["manual_correction"].is_number_integer()) {
        manual_corrections = reason_counts["manual_correction"].get<int64_t>();
    }
    postprocess["manual_corrections"] = manual_corrections;

    json summary_statistics = json::object();
    if (run_attrs.contains("summary_statistics") &&
        run_attrs["summary_statistics"].is_object()) {
        summary_statistics = run_attrs["summary_statistics"];
    }

    json existing_postprocess = json::object();
    if (summary_statistics.contains("postprocess") &&
        summary_statistics["postprocess"].is_object()) {
        existing_postprocess = summary_statistics["postprocess"];
    }

    if (existing_postprocess != postprocess) {
        summary_statistics["postprocess"] = postprocess;
        summary_statistics["postprocess_updated_utc"] = currentUtcIsoTimestamp();
        run_attrs["summary_statistics"] = summary_statistics;

        if (!writeNodeMetaV3(store, run_base, run_group_meta, &error_message)) {
            return false;
        }
    }

    return true;
}

bool ZarrDetectionLoader::setKeypointReviewStatus(
    const KeypointReviewStatusOptions& options,
    std::string& error_message,
    std::string* resolved_refined_run) {
    error_message.clear();
    if (resolved_refined_run) {
        resolved_refined_run->clear();
    }

    if (root_path_.empty()) {
        error_message = "No loaded Zarr archive to write into.";
        return false;
    }

    if (options.intended_use != "training" &&
        options.intended_use != "full_recording") {
        error_message =
            "options.intended_use must be \"training\" or \"full_recording\".";
        return false;
    }
    if (options.state != "approved" && options.state != "pending" &&
        options.state != "rejected" && options.state != "needs_review") {
        error_message =
            "options.state must be one of approved|pending|rejected|needs_review.";
        return false;
    }
    if (options.method != "manual" && options.method != "algorithmic" &&
        options.method != "hybrid" && options.method != "spotcheck") {
        error_message =
            "options.method must be one of manual|algorithmic|hybrid|spotcheck.";
        return false;
    }

    const std::string kvstore_path = normalizeKvstoreFileRootPath(root_path_);
    auto kv_spec = ts::kvstore::Spec::FromJson(
        {{"driver", "file"}, {"path", kvstore_path}});
    if (!kv_spec.ok()) {
        error_message =
            "Failed to create kvstore spec: " + kv_spec.status().ToString();
        return false;
    }

    auto store_result = ts::kvstore::Open(kv_spec.value(), context_).result();
    if (!store_result.ok()) {
        error_message =
            "Failed to open kvstore: " + store_result.status().ToString();
        return false;
    }
    const auto store = store_result.value();

    auto refined_group_attrs = readAttrsAny(store, "refined_keypoints_runs");
    if (!refined_group_attrs.has_value() || !refined_group_attrs->is_object()) {
        error_message = "refined_keypoints_runs group is missing.";
        return false;
    }

    const std::string refined_run = extractLatestRunName(*refined_group_attrs);
    if (refined_run.empty()) {
        error_message = "refined_keypoints_runs has no latest run pointer.";
        return false;
    }
    if (resolved_refined_run) {
        *resolved_refined_run = refined_run;
    }

    const std::string refined_run_path =
        "refined_keypoints_runs/" + refined_run + "/";
    auto run_meta = normalizeGroupMetadataV3(
        readNodeMetaV3(store, refined_run_path).value_or(makeEmptyGroupMetadataV3()));
    auto& run_attrs = run_meta["attributes"];

    const std::string timestamp = currentUtcIsoTimestamp();
    json review_status = json{{"state", options.state},
                              {"method", options.method},
                              {"intended_use", options.intended_use},
                              {"timestamp", timestamp}};
    if (!options.reviewer.empty()) {
        review_status["reviewer"] = options.reviewer;
    }
    if (!options.notes.empty()) {
        review_status["notes"] = options.notes;
    }

    if (!run_attrs.contains("keypoint_signature") ||
        !run_attrs["keypoint_signature"].is_object()) {
        run_attrs["keypoint_signature"] = json{{"signature_version", 1},
                                                {"generated_by", "crimson"},
                                                {"generated_at", timestamp},
                                                {"run", refined_run}};
    }
    run_attrs["keypoint_review_status"] = review_status;
    run_attrs["keypoint_review_signature"] = run_attrs["keypoint_signature"];

    if (!writeNodeMetaV3(store, refined_run_path, run_meta, &error_message)) {
        return false;
    }

    auto refined_group_meta = normalizeGroupMetadataV3(
        readNodeMetaV3(store, "refined_keypoints_runs")
            .value_or(makeEmptyGroupMetadataV3()));
    refined_group_meta["attributes"]["keypoint_review_status_latest"] = refined_run;
    if (!writeNodeMetaV3(store,
                         "refined_keypoints_runs",
                         refined_group_meta,
                         &error_message)) {
        return false;
    }

    return true;
}
