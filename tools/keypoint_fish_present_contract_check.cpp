#include "zarr_loader.h"
#include "zarr_loader_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
using json = nlohmann::json;

constexpr int32_t kRoiIndex = 0;

bool nanAwareEqual(double a, double b) {
    if (std::isnan(a) && std::isnan(b)) {
        return true;
    }
    return a == b;
}

bool nanAwareEqualVec(const std::vector<double>& a, const std::vector<double>& b) {
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
bool writeArray3DFlat(const ts::kvstore::KvStore& store,
                      const ts::Context& context,
                      const std::string& path,
                      const std::string& data_type,
                      const std::vector<T>& values,
                      ts::Index d0,
                      ts::Index d1,
                      ts::Index d2,
                      ts::Index c0,
                      ts::Index c1,
                      ts::Index c2,
                      const json& fill_value,
                      bool little_endian,
                      std::string* error_message) {
    if (d0 < 0 || d1 < 0 || d2 < 0) {
        if (error_message) {
            *error_message = "Invalid 3D array shape for '" + path + "'";
        }
        return false;
    }
    const size_t expected =
        static_cast<size_t>(d0) * static_cast<size_t>(d1) * static_cast<size_t>(d2);
    if (values.size() != expected) {
        if (error_message) {
            *error_message = "3D shape/value mismatch for '" + path + "'";
        }
        return false;
    }

    const json metadata = makeNumericArrayMetadata(
        {d0, d1, d2},
        {std::max<ts::Index>(1, c0), std::max<ts::Index>(1, c1), std::max<ts::Index>(1, c2)},
        data_type,
        fill_value,
        little_endian);
    auto open_result = openArrayForWrite<T, 3>(store, path, metadata, context);
    if (!open_result.ok()) {
        if (error_message) {
            *error_message = "Failed to open '" + path + "': " +
                             open_result.status().ToString();
        }
        return false;
    }

    const ts::Index extents[3] = {d0, d1, d2};
    auto source = ts::AllocateArray<T>(extents);
    std::copy(values.begin(), values.end(), source.data());
    auto write_result = ts::Write(source, open_result.value()).commit_future.result();
    if (!write_result.ok()) {
        if (error_message) {
            *error_message = "Failed to write '" + path + "': " +
                             write_result.status().ToString();
        }
        return false;
    }
    return true;
}

bool writeStringArray1D(const ts::kvstore::KvStore& store,
                        const ts::Context& context,
                        const std::string& path,
                        const std::vector<std::string>& values,
                        ts::Index chunk_size,
                        std::string* error_message) {
    const ts::Index rows = static_cast<ts::Index>(values.size());
    const json metadata = {
        {"shape", json::array({rows})},
        {"data_type", "string"},
        {"chunk_grid",
         {{"name", "regular"},
          {"configuration", {{"chunk_shape", json::array({std::max<ts::Index>(1, chunk_size)})}}}}},
        {"chunk_key_encoding",
         {{"name", "default"}, {"configuration", {{"separator", "/"}}}}},
        {"fill_value", ""},
        {"codecs",
         json::array({json{{"name", "vlen-utf8"}, {"configuration", json::object()}},
                      json{{"name", "zstd"},
                           {"configuration", json{{"level", 0}, {"checksum", false}}}}})},
        {"attributes", json::object()},
        {"zarr_format", 3},
        {"node_type", "array"},
        {"storage_transformers", json::array()},
    };

    auto open_result =
        openArrayForWrite<std::string, 1>(store, path, metadata, context);
    if (!open_result.ok()) {
        if (error_message) {
            *error_message = "Failed to open string array '" + path + "': " +
                             open_result.status().ToString();
        }
        return false;
    }

    const ts::Index extents[1] = {rows};
    auto source = ts::AllocateArray<std::string>(extents);
    for (size_t i = 0; i < values.size(); ++i) {
        source.data()[i] = values[i];
    }
    auto write_result = ts::Write(source, open_result.value()).commit_future.result();
    if (!write_result.ok()) {
        if (error_message) {
            *error_message = "Failed to write string array '" + path + "': " +
                             write_result.status().ToString();
        }
        return false;
    }
    return true;
}

template <typename T>
std::optional<std::vector<double>> read3DRowTyped(const ts::kvstore::KvStore& store,
                                                  const ts::Context& context,
                                                  const std::string& path,
                                                  int32_t roi_index) {
    auto arr_or = openArrayAny<T, 3>(store, path, context);
    if (!arr_or.ok()) {
        return std::nullopt;
    }
    auto read_result =
        ts::Read(arr_or.value() |
                 ts::Dims(0).IndexSlice(static_cast<ts::Index>(roi_index)))
            .result();
    if (!read_result.ok()) {
        return std::nullopt;
    }
    auto arr = read_result.value();
    if (arr.rank() != 2) {
        return std::nullopt;
    }
    const size_t rows = static_cast<size_t>(arr.shape()[0]);
    const size_t cols = static_cast<size_t>(arr.shape()[1]);
    std::vector<double> out(rows * cols,
                            std::numeric_limits<double>::quiet_NaN());
    const T* src = static_cast<const T*>(arr.data());
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<double>(src[i]);
    }
    return out;
}

std::vector<double> read3DRowAny(const ts::kvstore::KvStore& store,
                                 const ts::Context& context,
                                 const std::string& path,
                                 int32_t roi_index) {
    if (auto out = read3DRowTyped<double>(store, context, path, roi_index)) {
        return *out;
    }
    if (auto out = read3DRowTyped<float>(store, context, path, roi_index)) {
        return *out;
    }
    return {};
}

template <typename T>
std::optional<std::vector<double>> read2DRowTyped(const ts::kvstore::KvStore& store,
                                                  const ts::Context& context,
                                                  const std::string& path,
                                                  int32_t roi_index) {
    auto arr_or = openArrayAny<T, 2>(store, path, context);
    if (!arr_or.ok()) {
        return std::nullopt;
    }
    auto read_result =
        ts::Read(arr_or.value() |
                 ts::Dims(0).IndexSlice(static_cast<ts::Index>(roi_index)))
            .result();
    if (!read_result.ok()) {
        return std::nullopt;
    }
    auto arr = read_result.value();
    if (arr.rank() != 1) {
        return std::nullopt;
    }
    const size_t cols = static_cast<size_t>(arr.shape()[0]);
    std::vector<double> out(cols, std::numeric_limits<double>::quiet_NaN());
    const T* src = static_cast<const T*>(arr.data());
    for (size_t i = 0; i < cols; ++i) {
        out[i] = static_cast<double>(src[i]);
    }
    return out;
}

std::vector<double> read2DRowAny(const ts::kvstore::KvStore& store,
                                 const ts::Context& context,
                                 const std::string& path,
                                 int32_t roi_index) {
    if (auto out = read2DRowTyped<double>(store, context, path, roi_index)) {
        return *out;
    }
    if (auto out = read2DRowTyped<float>(store, context, path, roi_index)) {
        return *out;
    }
    if (auto out = read2DRowTyped<int32_t>(store, context, path, roi_index)) {
        return *out;
    }
    if (auto out = read2DRowTyped<int8_t>(store, context, path, roi_index)) {
        return *out;
    }
    if (auto out = read2DRowTyped<uint8_t>(store, context, path, roi_index)) {
        return *out;
    }
    if (auto out = read2DRowTyped<char>(store, context, path, roi_index)) {
        return *out;
    }
    return {};
}

template <typename T>
std::optional<double> read1DScalarTyped(const ts::kvstore::KvStore& store,
                                        const ts::Context& context,
                                        const std::string& path,
                                        int32_t roi_index) {
    auto arr_or = openArrayAny<T, 1>(store, path, context);
    if (!arr_or.ok()) {
        return std::nullopt;
    }
    auto read_result =
        ts::Read(arr_or.value() |
                 ts::Dims(0).IndexSlice(static_cast<ts::Index>(roi_index)))
            .result();
    if (!read_result.ok()) {
        return std::nullopt;
    }
    auto arr = read_result.value();
    if (arr.rank() != 0) {
        return std::nullopt;
    }
    return static_cast<double>(arr());
}

double read1DScalarAny(const ts::kvstore::KvStore& store,
                       const ts::Context& context,
                       const std::string& path,
                       int32_t roi_index) {
    if (auto v = read1DScalarTyped<double>(store, context, path, roi_index)) return *v;
    if (auto v = read1DScalarTyped<float>(store, context, path, roi_index)) return *v;
    if (auto v = read1DScalarTyped<int32_t>(store, context, path, roi_index)) return *v;
    if (auto v = read1DScalarTyped<int8_t>(store, context, path, roi_index)) return *v;
    if (auto v = read1DScalarTyped<uint8_t>(store, context, path, roi_index)) return *v;
    if (auto v = read1DScalarTyped<bool>(store, context, path, roi_index)) return *v;
    return std::numeric_limits<double>::quiet_NaN();
}

std::string readReasonBytes(const ts::kvstore::KvStore& store,
                            const ts::Context& context,
                            const std::string& path,
                            int32_t roi_index) {
    auto as_u8 = openArrayAny<uint8_t, 2>(store, path, context);
    if (as_u8.ok()) {
        auto read_result = ts::Read(as_u8.value()).result();
        if (read_result.ok()) {
            auto arr = read_result.value();
            if (arr.rank() == 2 && roi_index >= 0 &&
                static_cast<ts::Index>(roi_index) < arr.shape()[0]) {
                const size_t width = static_cast<size_t>(arr.shape()[1]);
                const uint8_t* ptr = static_cast<const uint8_t*>(arr.data());
                const size_t row_offset = static_cast<size_t>(roi_index) * width;
                size_t len = 0;
                while (len < width && ptr[row_offset + len] != 0) {
                    ++len;
                }
                return std::string(
                    reinterpret_cast<const char*>(ptr + row_offset),
                    reinterpret_cast<const char*>(ptr + row_offset + len));
            }
        }
    }

    auto as_char = openArrayAny<char, 2>(store, path, context);
    if (as_char.ok()) {
        auto read_result = ts::Read(as_char.value()).result();
        if (read_result.ok()) {
            auto arr = read_result.value();
            if (arr.rank() == 2 && roi_index >= 0 &&
                static_cast<ts::Index>(roi_index) < arr.shape()[0]) {
                const size_t width = static_cast<size_t>(arr.shape()[1]);
                const char* ptr = static_cast<const char*>(arr.data());
                const size_t row_offset = static_cast<size_t>(roi_index) * width;
                size_t len = 0;
                while (len < width && ptr[row_offset + len] != '\0') {
                    ++len;
                }
                return std::string(ptr + row_offset, ptr + row_offset + len);
            }
        }
    }

    return {};
}

std::string readReasonText(const ts::kvstore::KvStore& store,
                           const ts::Context& context,
                           const std::string& path,
                           int32_t roi_index) {
    auto as_string = openArrayAny<std::string, 1>(store, path, context);
    if (!as_string.ok()) {
        return {};
    }
    auto read_result = ts::Read(as_string.value()).result();
    if (!read_result.ok()) {
        return {};
    }
    auto arr = read_result.value();
    if (arr.rank() != 1 || roi_index < 0 ||
        static_cast<ts::Index>(roi_index) >= arr.shape()[0]) {
        return {};
    }
    return arr(static_cast<ts::Index>(roi_index));
}

struct RoiSnapshot {
    std::vector<double> keypoints_roi;
    std::vector<double> keypoints_img;
    std::vector<double> keypoints_norm;
    double heading = std::numeric_limits<double>::quiet_NaN();
    double confidence = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> keypoint_confidences;
    double triangle_area = std::numeric_limits<double>::quiet_NaN();
    double min_angle = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> triangle_angles;
    bool refined_success = false;
    bool confidence_valid = false;
    bool geometry_valid = false;
    bool usable_keypoints = false;
    bool heading_finite = false;
    bool heading_usable = false;
    bool flip_corrected = false;
    int quality_label = -1;
    std::string reason_bytes;
    std::string reason_text;
};

RoiSnapshot readSnapshot(const ts::kvstore::KvStore& store,
                         const ts::Context& context,
                         const std::string& run_base,
                         int32_t roi_index) {
    RoiSnapshot s;
    s.keypoints_roi = read3DRowAny(store, context, run_base + "keypoints_roi", roi_index);
    s.keypoints_img = read3DRowAny(store, context, run_base + "keypoints_img", roi_index);
    s.keypoints_norm = read3DRowAny(store, context, run_base + "keypoints_norm", roi_index);
    s.heading = read1DScalarAny(store, context, run_base + "heading", roi_index);
    s.confidence = read1DScalarAny(store, context, run_base + "confidence", roi_index);
    s.keypoint_confidences =
        read2DRowAny(store, context, run_base + "keypoint_confidences", roi_index);
    s.triangle_area =
        read1DScalarAny(store, context, run_base + "triangle_area", roi_index);
    s.min_angle = read1DScalarAny(store, context, run_base + "min_angle", roi_index);
    s.triangle_angles =
        read2DRowAny(store, context, run_base + "triangle_angles", roi_index);

    s.refined_success =
        read1DScalarAny(store, context, run_base + "refined_success", roi_index) != 0.0;
    s.confidence_valid =
        read1DScalarAny(store, context, run_base + "confidence_valid", roi_index) != 0.0;
    s.geometry_valid =
        read1DScalarAny(store, context, run_base + "geometry_valid", roi_index) != 0.0;
    s.usable_keypoints =
        read1DScalarAny(store, context, run_base + "usable_keypoints", roi_index) != 0.0;
    s.heading_finite =
        read1DScalarAny(store, context, run_base + "heading_finite", roi_index) != 0.0;
    s.heading_usable =
        read1DScalarAny(store, context, run_base + "heading_usable", roi_index) != 0.0;
    s.flip_corrected =
        read1DScalarAny(store, context, run_base + "flip_corrected", roi_index) != 0.0;
    s.quality_label = static_cast<int>(
        read1DScalarAny(store, context, run_base + "quality_labels", roi_index));
    s.reason_bytes = readReasonBytes(store, context, run_base + "reason_bytes", roi_index);
    s.reason_text = readReasonText(store, context, run_base + "reason", roi_index);
    return s;
}

bool snapshotsEqual(const RoiSnapshot& a, const RoiSnapshot& b) {
    return nanAwareEqualVec(a.keypoints_roi, b.keypoints_roi) &&
           nanAwareEqualVec(a.keypoints_img, b.keypoints_img) &&
           nanAwareEqualVec(a.keypoints_norm, b.keypoints_norm) &&
           nanAwareEqual(a.heading, b.heading) &&
           nanAwareEqual(a.confidence, b.confidence) &&
           nanAwareEqualVec(a.keypoint_confidences, b.keypoint_confidences) &&
           nanAwareEqual(a.triangle_area, b.triangle_area) &&
           nanAwareEqual(a.min_angle, b.min_angle) &&
           nanAwareEqualVec(a.triangle_angles, b.triangle_angles) &&
           a.refined_success == b.refined_success &&
           a.confidence_valid == b.confidence_valid &&
           a.geometry_valid == b.geometry_valid &&
           a.usable_keypoints == b.usable_keypoints &&
           a.heading_finite == b.heading_finite &&
           a.heading_usable == b.heading_usable &&
           a.flip_corrected == b.flip_corrected &&
           a.quality_label == b.quality_label &&
           a.reason_bytes == b.reason_bytes &&
           a.reason_text == b.reason_text;
}

json snapshotToJson(const RoiSnapshot& s) {
    return json{
        {"keypoints_roi", s.keypoints_roi},
        {"keypoints_img", s.keypoints_img},
        {"keypoints_norm", s.keypoints_norm},
        {"heading", s.heading},
        {"confidence", s.confidence},
        {"keypoint_confidences", s.keypoint_confidences},
        {"triangle_area", s.triangle_area},
        {"min_angle", s.min_angle},
        {"triangle_angles", s.triangle_angles},
        {"refined_success", s.refined_success},
        {"confidence_valid", s.confidence_valid},
        {"geometry_valid", s.geometry_valid},
        {"usable_keypoints", s.usable_keypoints},
        {"heading_finite", s.heading_finite},
        {"heading_usable", s.heading_usable},
        {"flip_corrected", s.flip_corrected},
        {"quality_label", s.quality_label},
        {"reason_bytes", s.reason_bytes},
        {"reason_text", s.reason_text},
    };
}

bool allNan(const std::vector<double>& values) {
    for (double v : values) {
        if (!std::isnan(v)) {
            return false;
        }
    }
    return true;
}

bool containsTag(const std::string& reason, const std::string& tag) {
    size_t start = 0;
    while (start <= reason.size()) {
        size_t end = reason.find('|', start);
        if (end == std::string::npos) {
            end = reason.size();
        }
        std::string token = reason.substr(start, end - start);
        if (token == tag) {
            return true;
        }
        if (end == reason.size()) {
            break;
        }
        start = end + 1;
    }
    return false;
}

bool hasAnyStaleKey(const json& value) {
    if (value.is_object()) {
        for (const auto& item : value.items()) {
            if (item.key().find("stale") != std::string::npos) {
                return true;
            }
            if (hasAnyStaleKey(item.value())) {
                return true;
            }
        }
    } else if (value.is_array()) {
        for (const auto& child : value) {
            if (hasAnyStaleKey(child)) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

int main() {
    int failures = 0;
    auto assert_true = [&](bool cond, const std::string& label) {
        if (cond) {
            std::cout << "[ASSERT PASS] " << label << std::endl;
        } else {
            std::cout << "[ASSERT FAIL] " << label << std::endl;
            failures++;
        }
    };

    char temp_template[] = "/tmp/crimson_kp_fish_contract_XXXXXX";
    char* temp_dir = mkdtemp(temp_template);
    if (!temp_dir) {
        std::cerr << "Failed to create temp directory." << std::endl;
        return 2;
    }

    const std::string zarr_path = std::string(temp_dir) + "/fixture.zarr";
    std::filesystem::create_directories(zarr_path);

    const std::string detect_run = "detect_test_0001";
    const std::string refined_run = "refined_keypoints_test_0001";
    const std::string run_base = "refined_keypoints_runs/" + refined_run + "/";
    const std::string initial_reason = "sticky|low_confidence";

    std::string error_message;
    const ts::Context context = ts::Context::Default();
    auto kv_spec = ts::kvstore::Spec::FromJson(
        {{"driver", "file"}, {"path", normalizeKvstoreFileRootPath(zarr_path)}});
    if (!kv_spec.ok()) {
        std::cerr << "kvstore spec failed: " << kv_spec.status().ToString() << std::endl;
        return 2;
    }
    auto store_result = ts::kvstore::Open(kv_spec.value(), context).result();
    if (!store_result.ok()) {
        std::cerr << "kvstore open failed: " << store_result.status().ToString() << std::endl;
        return 2;
    }
    const auto store = store_result.value();

    json root_meta = makeEmptyGroupMetadataV3();
    root_meta["attributes"]["source_video_metadata"] =
        json{{"width", 640}, {"height", 480}, {"fps", 120.0}};
    if (!writeNodeMetaV3(store, "", root_meta, &error_message)) {
        std::cerr << "Failed writing root metadata: " << error_message << std::endl;
        return 2;
    }

    json detect_group = makeEmptyGroupMetadataV3();
    detect_group["attributes"]["latest"] = detect_run;
    if (!writeNodeMetaV3(store, "detect_runs", detect_group, &error_message)) {
        std::cerr << "Failed writing detect_runs metadata: " << error_message << std::endl;
        return 2;
    }
    if (!writeNodeMetaV3(store, "detect_runs/" + detect_run, makeEmptyGroupMetadataV3(),
                         &error_message)) {
        std::cerr << "Failed writing detect run metadata: " << error_message << std::endl;
        return 2;
    }

    if (!writeNumericArray1D<int32_t>(store, context,
                                      "detect_runs/" + detect_run + "/frame_indices",
                                      "int32",
                                      std::vector<int32_t>{0},
                                      1,
                                      0,
                                      false,
                                      &error_message)) {
        std::cerr << error_message << std::endl;
        return 2;
    }
    if (!writeNumericArray2DFlat<float>(store, context,
                                        "detect_runs/" + detect_run + "/bbox_norm_coords",
                                        "float32",
                                        std::vector<float>{0.5f, 0.5f, 0.2f, 0.2f},
                                        1,
                                        4,
                                        1,
                                        4,
                                        0.0f,
                                        false,
                                        &error_message)) {
        std::cerr << error_message << std::endl;
        return 2;
    }
    if (!writeNumericArray1D<int32_t>(store, context,
                                      "detect_runs/" + detect_run + "/n_detections",
                                      "int32",
                                      std::vector<int32_t>{1},
                                      1,
                                      0,
                                      false,
                                      &error_message)) {
        std::cerr << error_message << std::endl;
        return 2;
    }

    json refined_group = makeEmptyGroupMetadataV3();
    refined_group["attributes"]["latest"] = refined_run;
    if (!writeNodeMetaV3(store, "refined_keypoints_runs", refined_group,
                         &error_message)) {
        std::cerr << "Failed writing refined group metadata: " << error_message << std::endl;
        return 2;
    }

    json run_meta = makeEmptyGroupMetadataV3();
    run_meta["attributes"]["summary_statistics"] = json{
        {"refine",
         {{"confidence_threshold", 0.3},
          {"min_triangle_angle", 10.0},
          {"min_triangle_area", 100.0}}},
        {"postprocess", {{"sentinel", 1}}},
        {"postprocess_updated_utc", "2000-01-01T00:00:00Z"},
    };
    run_meta["attributes"]["keypoint_labels"] = json::array({"bladder", "eye_left", "eye_right"});
    if (!writeNodeMetaV3(store, run_base, run_meta, &error_message)) {
        std::cerr << "Failed writing refined run metadata: " << error_message << std::endl;
        return 2;
    }

    if (!writeNumericArray1D<int32_t>(store, context, run_base + "frame_indices", "int32",
                                      std::vector<int32_t>{0}, 1, 0, false,
                                      &error_message)) {
        std::cerr << error_message << std::endl;
        return 2;
    }
    if (!writeNumericArray1D<int32_t>(store, context, run_base + "frame_counts", "int32",
                                      std::vector<int32_t>{1}, 1, 0, false,
                                      &error_message)) {
        std::cerr << error_message << std::endl;
        return 2;
    }

    const std::vector<double> keypoints_roi = {
        10.0, 11.0, 12.0, 13.0, 14.0, 15.0};
    const std::vector<double> keypoints_img = {
        110.0, 111.0, 112.0, 113.0, 114.0, 115.0};
    const std::vector<double> keypoints_norm = {
        0.171875, 0.23125, 0.175, 0.23541667, 0.178125, 0.23958333};
    if (!writeArray3DFlat<double>(store, context, run_base + "keypoints_roi",
                                  "float64", keypoints_roi, 1, 3, 2, 1, 3, 2,
                                  0.0, false, &error_message) ||
        !writeArray3DFlat<double>(store, context, run_base + "keypoints_img",
                                  "float64", keypoints_img, 1, 3, 2, 1, 3, 2,
                                  0.0, false, &error_message) ||
        !writeArray3DFlat<double>(store, context, run_base + "keypoints_norm",
                                  "float64", keypoints_norm, 1, 3, 2, 1, 3, 2,
                                  0.0, false, &error_message)) {
        std::cerr << error_message << std::endl;
        return 2;
    }

    if (!writeNumericArray1D<double>(store, context, run_base + "heading", "float64",
                                     std::vector<double>{45.0}, 1,
                                     0.0, false,
                                     &error_message) ||
        !writeNumericArray1D<double>(store, context, run_base + "confidence", "float64",
                                     std::vector<double>{0.2}, 1,
                                     0.0, false,
                                     &error_message) ||
        !writeNumericArray2DFlat<double>(store, context,
                                         run_base + "keypoint_confidences", "float64",
                                         std::vector<double>{0.2, 0.2, 0.2},
                                         1, 3, 1, 3,
                                         0.0,
                                         false, &error_message) ||
        !writeNumericArray1D<double>(store, context, run_base + "triangle_area", "float64",
                                     std::vector<double>{50.0}, 1,
                                     0.0, false,
                                     &error_message) ||
        !writeNumericArray1D<double>(store, context, run_base + "min_angle", "float64",
                                     std::vector<double>{5.0}, 1,
                                     0.0, false,
                                     &error_message) ||
        !writeNumericArray2DFlat<double>(store, context,
                                         run_base + "triangle_angles", "float64",
                                         std::vector<double>{5.0, 6.0, 7.0},
                                         1, 3, 1, 3,
                                         0.0,
                                         false, &error_message)) {
        std::cerr << error_message << std::endl;
        return 2;
    }

    if (!writeNumericArray1D<int8_t>(store, context, run_base + "quality_labels", "int8",
                                     std::vector<int8_t>{6}, 1, 0, false,
                                     &error_message) ||
        !writeNumericArray1D<uint8_t>(store, context, run_base + "refined_success", "uint8",
                                      std::vector<uint8_t>{1}, 1, 0, false,
                                      &error_message) ||
        !writeNumericArray1D<uint8_t>(store, context, run_base + "confidence_valid", "uint8",
                                      std::vector<uint8_t>{1}, 1, 0, false,
                                      &error_message) ||
        !writeNumericArray1D<uint8_t>(store, context, run_base + "geometry_valid", "uint8",
                                      std::vector<uint8_t>{1}, 1, 0, false,
                                      &error_message) ||
        !writeNumericArray1D<uint8_t>(store, context, run_base + "usable_keypoints", "uint8",
                                      std::vector<uint8_t>{1}, 1, 0, false,
                                      &error_message) ||
        !writeNumericArray1D<uint8_t>(store, context, run_base + "heading_finite", "uint8",
                                      std::vector<uint8_t>{1}, 1, 0, false,
                                      &error_message) ||
        !writeNumericArray1D<uint8_t>(store, context, run_base + "heading_usable", "uint8",
                                      std::vector<uint8_t>{1}, 1, 0, false,
                                      &error_message) ||
        !writeNumericArray1D<uint8_t>(store, context, run_base + "flip_corrected", "uint8",
                                      std::vector<uint8_t>{1}, 1, 0, false,
                                      &error_message) ||
        !writeNumericArray1D<uint8_t>(store, context, run_base + "source_success", "uint8",
                                      std::vector<uint8_t>{1}, 1, 0, false,
                                      &error_message) ||
        !writeNumericArray1D<uint8_t>(store, context, run_base + "detection_success", "uint8",
                                      std::vector<uint8_t>{1}, 1, 0, false,
                                      &error_message) ||
        !writeNumericArray1D<int8_t>(store, context, run_base + "detection_source", "int8",
                                     std::vector<int8_t>{0}, 1, 0, false,
                                     &error_message) ||
        !writeNumericArray1D<int32_t>(store, context, run_base + "retune_id", "int32",
                                      std::vector<int32_t>{-1}, 1, 0, false,
                                      &error_message)) {
        std::cerr << error_message << std::endl;
        return 2;
    }

    const size_t reason_width = 64;
    std::vector<uint8_t> reason_bytes(reason_width, 0);
    std::copy(initial_reason.begin(), initial_reason.end(), reason_bytes.begin());
    if (!writeNumericArray2DFlat<uint8_t>(store, context, run_base + "reason_bytes", "uint8",
                                          reason_bytes, 1,
                                          static_cast<ts::Index>(reason_width), 1,
                                          static_cast<ts::Index>(reason_width), 0,
                                          false, &error_message)) {
        std::cerr << error_message << std::endl;
        return 2;
    }
    bool reason_text_created = false;
    if (writeStringArray1D(store, context, run_base + "reason",
                           std::vector<std::string>{initial_reason}, 1,
                           &error_message)) {
        reason_text_created = true;
    } else {
        std::cout << "[INFO] reason string array unavailable in this build; "
                     "running reason_bytes-only assertions."
                  << std::endl;
    }

    ZarrDetectionLoader loader;
    std::string load_error;
    const bool load_ok = loader.loadZarrFile(zarr_path, load_error);
    assert_true(load_ok, "loadZarrFile succeeds on fixture");
    if (!load_ok) {
        std::cerr << "load error: " << load_error << std::endl;
        return 2;
    }

    auto before_meta_opt = readNodeMetaV3(store, run_base);
    assert_true(before_meta_opt.has_value(), "run metadata exists before write");
    const json before_meta = before_meta_opt.value_or(json::object());

    RoiSnapshot before = readSnapshot(store, context, run_base, kRoiIndex);
    std::cout << "EVIDENCE_BEFORE_ROI_0: " << snapshotToJson(before).dump() << std::endl;

    ManualKeypointRoiWrite fish_no_kp;
    fish_no_kp.roi_index = kRoiIndex;
    fish_no_kp.mark_fish_present_no_keypoints = true;

    std::vector<int32_t> changed_first;
    std::string write_error;
    const bool first_ok =
        loader.writeManualRefinedKeypoints({fish_no_kp}, write_error, &changed_first);
    assert_true(first_ok, "first fish_present_no_keypoints write succeeds");
    if (!first_ok) {
        std::cerr << "write error: " << write_error << std::endl;
        return 2;
    }
    assert_true(changed_first.size() == 1 && changed_first[0] == kRoiIndex,
                "first write reports changed roi row");

    auto after_first_meta_opt = readNodeMetaV3(store, run_base);
    assert_true(after_first_meta_opt.has_value(),
                "run metadata exists after first write");
    const json after_first_meta = after_first_meta_opt.value_or(json::object());

    RoiSnapshot after_first = readSnapshot(store, context, run_base, kRoiIndex);
    std::cout << "EVIDENCE_AFTER_FIRST_WRITE_ROI_0: "
              << snapshotToJson(after_first).dump() << std::endl;

    assert_true(allNan(after_first.keypoints_roi),
                "keypoints_roi set to NaN");
    assert_true(allNan(after_first.keypoints_img),
                "keypoints_img set to NaN");
    assert_true(allNan(after_first.keypoints_norm),
                "keypoints_norm set to NaN");
    assert_true(std::isnan(after_first.heading), "heading set to NaN");
    assert_true(std::isnan(after_first.confidence), "confidence set to NaN");
    assert_true(allNan(after_first.keypoint_confidences),
                "keypoint_confidences set to NaN");
    assert_true(std::isnan(after_first.triangle_area),
                "triangle_area set to NaN");
    assert_true(std::isnan(after_first.min_angle), "min_angle set to NaN");
    assert_true(allNan(after_first.triangle_angles),
                "triangle_angles set to NaN");

    assert_true(!after_first.refined_success, "refined_success false");
    assert_true(!after_first.confidence_valid, "confidence_valid false");
    assert_true(!after_first.geometry_valid, "geometry_valid false");
    assert_true(!after_first.usable_keypoints, "usable_keypoints false");
    assert_true(!after_first.heading_finite, "heading_finite false");
    assert_true(!after_first.heading_usable, "heading_usable false");
    assert_true(!after_first.flip_corrected, "flip_corrected false");
    assert_true(after_first.quality_label == 0, "quality_labels set to 0");

    assert_true(containsTag(after_first.reason_bytes, "fish_present_no_keypoints"),
                "reason contains fish_present_no_keypoints");
    if (reason_text_created) {
        assert_true(after_first.reason_text == after_first.reason_bytes,
                    "reason and reason_bytes synchronized");
    }

    const auto& before_attrs = before_meta.contains("attributes")
                                   ? before_meta["attributes"]
                                   : json::object();
    const auto& first_attrs = after_first_meta.contains("attributes")
                                  ? after_first_meta["attributes"]
                                  : json::object();
    std::string before_pp_ts;
    if (before_attrs.contains("summary_statistics") &&
        before_attrs["summary_statistics"].is_object() &&
        before_attrs["summary_statistics"].contains("postprocess_updated_utc") &&
        before_attrs["summary_statistics"]["postprocess_updated_utc"].is_string()) {
        before_pp_ts =
            before_attrs["summary_statistics"]["postprocess_updated_utc"].get<std::string>();
    }
    std::string after_first_pp_ts;
    if (first_attrs.contains("summary_statistics") &&
        first_attrs["summary_statistics"].is_object() &&
        first_attrs["summary_statistics"].contains("postprocess_updated_utc") &&
        first_attrs["summary_statistics"]["postprocess_updated_utc"].is_string()) {
        after_first_pp_ts =
            first_attrs["summary_statistics"]["postprocess_updated_utc"].get<std::string>();
    }
    assert_true(!after_first_pp_ts.empty(), "postprocess_updated_utc present after first write");
    assert_true(after_first_pp_ts != before_pp_ts,
                "postprocess_updated_utc changed on first effective write");

    assert_true(!hasAnyStaleKey(first_attrs), "no stale marker side effects in attrs");

    std::vector<int32_t> changed_second;
    const bool second_ok =
        loader.writeManualRefinedKeypoints({fish_no_kp}, write_error, &changed_second);
    assert_true(second_ok, "second fish_present_no_keypoints write succeeds");
    if (!second_ok) {
        std::cerr << "second write error: " << write_error << std::endl;
        return 2;
    }
    assert_true(changed_second.empty(),
                "second write is a no-op (no changed roi indices)");

    auto after_second_meta_opt = readNodeMetaV3(store, run_base);
    assert_true(after_second_meta_opt.has_value(),
                "run metadata exists after second write");
    const json after_second_meta = after_second_meta_opt.value_or(json::object());
    RoiSnapshot after_second = readSnapshot(store, context, run_base, kRoiIndex);
    std::cout << "EVIDENCE_AFTER_SECOND_WRITE_ROI_0: "
              << snapshotToJson(after_second).dump() << std::endl;

    assert_true(snapshotsEqual(after_first, after_second),
                "second write produced no ROI state delta");
    assert_true(after_first_meta == after_second_meta,
                "second write produced no metadata delta");

    const auto& second_attrs = after_second_meta.contains("attributes")
                                   ? after_second_meta["attributes"]
                                   : json::object();
    std::string after_second_pp_ts;
    if (second_attrs.contains("summary_statistics") &&
        second_attrs["summary_statistics"].is_object() &&
        second_attrs["summary_statistics"].contains("postprocess_updated_utc") &&
        second_attrs["summary_statistics"]["postprocess_updated_utc"].is_string()) {
        after_second_pp_ts =
            second_attrs["summary_statistics"]["postprocess_updated_utc"].get<std::string>();
    }
    assert_true(after_second_pp_ts == after_first_pp_ts,
                "postprocess_updated_utc unchanged on no-op write");
    assert_true(!hasAnyStaleKey(second_attrs),
                "no stale marker side effects after no-op write");

    if (failures > 0) {
        std::cout << "[RESULT] FAILURES=" << failures << std::endl;
        return 1;
    }
    std::cout << "[RESULT] ALL ASSERTIONS PASSED" << std::endl;
    std::cout << "[FIXTURE] " << zarr_path << std::endl;
    return 0;
}
