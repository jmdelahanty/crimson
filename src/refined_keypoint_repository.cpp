#include "refined_keypoint_repository.h"

#include "zarr_loader_internal.h"

#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <vector>

namespace {

using json = nlohmann::json;

constexpr size_t kManualKeypointCount = 3;
constexpr size_t kManualKeypointDims = 2;
constexpr size_t kMaxStaleIndexHistory = 2048;

enum class KeypointEditMode {
    ManualCorrection,
    FishPresentNoKeypoints,
    DetectionIssue,
};

struct KeypointGeometryMetrics {
    double area = std::numeric_limits<double>::quiet_NaN();
    std::array<double, 3> angles = {
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::quiet_NaN(),
    };
    double min_angle = std::numeric_limits<double>::quiet_NaN();
    double max_angle = std::numeric_limits<double>::quiet_NaN();
};

struct RefinedRunContext {
    ts::kvstore::KvStore store;
    ts::Context tensorstore_context = ts::Context::Default();
    std::string archive_path;
    std::string parent_name;
    std::string run_name;
    std::string run_path;
    json run_meta;
    size_t total_rois = 0;
    int image_width = 0;
    int image_height = 0;
    double confidence_threshold = 0.3;
    double min_triangle_angle = 10.0;
    double min_triangle_area = 100.0;
    std::optional<double> max_triangle_area;
};

struct ReasonLabelsState {
    std::vector<std::string> labels;
    bool decoded = false;
    bool reason_bytes_exists = false;
    bool reason_exists = false;
};

bool isAllowedValue(const std::string& value,
                    std::initializer_list<const char*> allowed) {
    for (const char* candidate : allowed) {
        if (value == candidate) {
            return true;
        }
    }
    return false;
}

std::optional<std::string> sha256Hex(const std::string& payload) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    if (SHA256(reinterpret_cast<const unsigned char*>(payload.data()),
               payload.size(),
               digest) == nullptr) {
        return std::nullopt;
    }

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(SHA256_DIGEST_LENGTH * 2);
    for (size_t i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        out[i * 2] = kHex[(digest[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHex[digest[i] & 0x0F];
    }
    return out;
}

json getNestedObjectOrNull(const json& source, const char* key) {
    if (source.contains(key) && source[key].is_object()) {
        return source[key];
    }
    return nullptr;
}

json buildKeypointSignature(const json& attrs) {
    json params = getNestedObjectOrNull(attrs, "parameters");
    if (!params.is_object()) {
        const json provenance = getNestedObjectOrNull(attrs, "provenance");
        if (provenance.is_object() &&
            provenance.contains("parameters") &&
            provenance["parameters"].is_object()) {
            params = provenance["parameters"];
        }
    }

    json parameter_source = nullptr;
    if (attrs.contains("parameter_source")) {
        parameter_source = attrs["parameter_source"];
    } else if (params.is_object() &&
               params.contains("parameter_source")) {
        parameter_source = params["parameter_source"];
    }

    json parameters_hash = nullptr;
    if (params.is_object()) {
        if (const auto digest = sha256Hex(params.dump());
            digest.has_value()) {
            parameters_hash = *digest;
        }
    }

    return json{
        {"signature_version", 1},
        {"source_keypoints_run", attrs.value("source_keypoints_run", json(nullptr))},
        {"source_crop_run", attrs.value("source_crop_run", json(nullptr))},
        {"source_detect_run", attrs.value("source_detect_run", json(nullptr))},
        {"source_refined_run", attrs.value("source_refined_run", json(nullptr))},
        {"parameter_source", parameter_source},
        {"parameters_hash", parameters_hash},
    };
}

template <typename T, int Rank>
ts::Result<ts::TensorStore<T, Rank>> openExistingArrayForWrite(
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
    return ts::Open<T, Rank>(
               spec,
               ts::OpenMode::open,
               ts::ReadWriteMode::write,
               context)
        .result();
}

double triangleArea(
    const std::array<std::array<double, 2>, kManualKeypointCount>& vertices) {
    const double v1x = vertices[1][0] - vertices[0][0];
    const double v1y = vertices[1][1] - vertices[0][1];
    const double v2x = vertices[2][0] - vertices[0][0];
    const double v2y = vertices[2][1] - vertices[0][1];
    return 0.5 * std::abs(v1x * v2y - v1y * v2x);
}

std::array<double, 3> edgeLengths(
    const std::array<std::array<double, 2>, kManualKeypointCount>& vertices) {
    auto dist = [&](size_t a, size_t b) -> double {
        const double dx = vertices[a][0] - vertices[b][0];
        const double dy = vertices[a][1] - vertices[b][1];
        return std::sqrt(dx * dx + dy * dy);
    };
    return {dist(0, 1), dist(0, 2), dist(1, 2)};
}

KeypointGeometryMetrics computeGeometryMetrics(
    const std::array<std::array<double, 2>, kManualKeypointCount>& vertices) {
    for (const auto& point : vertices) {
        if (!std::isfinite(point[0]) || !std::isfinite(point[1])) {
            return {};
        }
    }

    KeypointGeometryMetrics metrics;
    metrics.area = triangleArea(vertices);
    const auto edges = edgeLengths(vertices);

    auto safe_angle = [](double numerator, double denominator) -> double {
        if (!(denominator > 0.0)) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        const double cosine = std::clamp(numerator / denominator, -1.0, 1.0);
        return std::acos(cosine) * 180.0 / M_PI;
    };

    const double a = edges[0];
    const double b = edges[1];
    const double c = edges[2];
    metrics.angles[0] = safe_angle(b * b + c * c - a * a, 2.0 * b * c);
    metrics.angles[1] = safe_angle(a * a + c * c - b * b, 2.0 * a * c);
    metrics.angles[2] = safe_angle(a * a + b * b - c * c, 2.0 * a * b);

    std::vector<double> finite_angles;
    for (double angle : metrics.angles) {
        if (std::isfinite(angle)) {
            finite_angles.push_back(angle);
        }
    }
    if (!finite_angles.empty()) {
        metrics.min_angle =
            *std::min_element(finite_angles.begin(), finite_angles.end());
        metrics.max_angle =
            *std::max_element(finite_angles.begin(), finite_angles.end());
    }
    return metrics;
}

double computeHeadingFromPoints(
    const KeypointHeadingComputationSpec& heading_spec,
    const std::array<std::array<double, 2>, kManualKeypointCount>& points) {
    std::vector<std::array<double, 2>> positions(points.begin(), points.end());
    double heading_deg = std::numeric_limits<double>::quiet_NaN();
    if (!evaluateKeypointHeadingDegrees(heading_spec, positions, heading_deg)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return heading_deg;
}

bool jsonScalarEqual(const json& lhs, const json& rhs) {
    return lhs == rhs;
}

template <typename T>
bool valuesEqualWithNaN(const T& lhs, const T& rhs) {
    if constexpr (std::is_floating_point_v<T>) {
        return (std::isnan(lhs) && std::isnan(rhs)) || lhs == rhs;
    } else {
        return lhs == rhs;
    }
}

template <typename T>
bool flatVectorsEqual(const std::vector<T>& lhs, const std::vector<T>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (!valuesEqualWithNaN(lhs[i], rhs[i])) {
            return false;
        }
    }
    return true;
}

std::optional<std::vector<ts::Index>> readShapeFromNodeMeta(const json& node_meta) {
    if (!node_meta.is_object() || !node_meta.contains("shape") ||
        !node_meta["shape"].is_array()) {
        return std::nullopt;
    }
    std::vector<ts::Index> shape;
    for (const auto& dim : node_meta["shape"]) {
        if (!dim.is_number_integer() && !dim.is_number_unsigned()) {
            return std::nullopt;
        }
        shape.push_back(static_cast<ts::Index>(dim.get<int64_t>()));
    }
    return shape;
}

std::string extractDataTypeName(const json& node_meta) {
    if (!node_meta.is_object() || !node_meta.contains("data_type")) {
        return "";
    }
    const json& dtype = node_meta["data_type"];
    if (dtype.is_string()) {
        return toLowerCopy(dtype.get<std::string>());
    }
    if (dtype.is_object()) {
        if (dtype.contains("name") && dtype["name"].is_string()) {
            return toLowerCopy(dtype["name"].get<std::string>());
        }
        if (dtype.contains("kind") && dtype["kind"].is_string()) {
            return toLowerCopy(dtype["kind"].get<std::string>());
        }
    }
    return "";
}

template <typename T, int Rank>
bool writeFlatRowIfChangedTyped(
    const ts::kvstore::KvStore& store,
    const ts::Context& context,
    const std::string& path,
    size_t roi_index,
    const std::vector<T>& flat_values,
    const std::array<ts::Index, Rank>& extents,
    std::string* error_message,
    bool* changed_out) {
    auto open_result = openExistingArrayForWrite<T, Rank>(store, path, context);
    if (!open_result.ok()) {
        if (error_message != nullptr) {
            *error_message =
                "Failed to open array '" + path + "' for write: " +
                open_result.status().ToString();
        }
        return false;
    }

    auto slice = open_result.value() |
                 ts::Dims(0).HalfOpenInterval(
                     static_cast<ts::Index>(roi_index),
                     static_cast<ts::Index>(roi_index + 1));
    auto current_result = ts::Read(slice).result();
    if (!current_result.ok()) {
        if (error_message != nullptr) {
            *error_message =
                "Failed to read existing row from '" + path + "': " +
                current_result.status().ToString();
        }
        return false;
    }

    const auto current = current_result.value();
    const size_t expected_count = flat_values.size();
    std::vector<T> current_flat;
    current_flat.resize(expected_count);
    const T* current_ptr = static_cast<const T*>(current.data());
    if (current_ptr != nullptr && expected_count > 0) {
        std::copy(current_ptr, current_ptr + expected_count, current_flat.begin());
    }

    if (flatVectorsEqual(current_flat, flat_values)) {
        if (changed_out != nullptr) {
            *changed_out = false;
        }
        return true;
    }

    auto source = ts::AllocateArray<T>(extents);
    if (!flat_values.empty()) {
        std::copy(flat_values.begin(), flat_values.end(), source.data());
    }

    auto write_result = ts::Write(source, slice).commit_future.result();
    if (!write_result.ok()) {
        if (error_message != nullptr) {
            *error_message =
                "Failed to write row into '" + path + "': " +
                write_result.status().ToString();
        }
        return false;
    }
    if (changed_out != nullptr) {
        *changed_out = true;
    }
    return true;
}

bool writeFloatScalarRowIfChanged(const RefinedRunContext& ctx,
                                  const std::string& path,
                                  size_t roi_index,
                                  double value,
                                  std::string* error_message,
                                  bool* changed_out) {
    const auto meta = readNodeMetaV3(ctx.store, path);
    if (!meta.has_value()) {
        if (changed_out != nullptr) {
            *changed_out = false;
        }
        return true;
    }
    const std::string dtype = extractDataTypeName(*meta);
    if (dtype == "float32") {
        return writeFlatRowIfChangedTyped<float, 1>(
            ctx.store, ctx.tensorstore_context, path, roi_index,
            {static_cast<float>(value)}, {1}, error_message, changed_out);
    }
    return writeFlatRowIfChangedTyped<double, 1>(
        ctx.store, ctx.tensorstore_context, path, roi_index,
        {value}, {1}, error_message, changed_out);
}

bool writeIntScalarRowIfChanged(const RefinedRunContext& ctx,
                                const std::string& path,
                                size_t roi_index,
                                int32_t value,
                                std::string* error_message,
                                bool* changed_out) {
    const auto meta = readNodeMetaV3(ctx.store, path);
    if (!meta.has_value()) {
        if (changed_out != nullptr) {
            *changed_out = false;
        }
        return true;
    }
    const std::string dtype = extractDataTypeName(*meta);
    if (dtype == "int8") {
        return writeFlatRowIfChangedTyped<int8_t, 1>(
            ctx.store, ctx.tensorstore_context, path, roi_index,
            {static_cast<int8_t>(value)}, {1}, error_message, changed_out);
    }
    if (dtype == "uint8" || dtype == "bool") {
        return writeFlatRowIfChangedTyped<uint8_t, 1>(
            ctx.store, ctx.tensorstore_context, path, roi_index,
            {static_cast<uint8_t>(value)}, {1}, error_message, changed_out);
    }
    return writeFlatRowIfChangedTyped<int32_t, 1>(
        ctx.store, ctx.tensorstore_context, path, roi_index,
        {value}, {1}, error_message, changed_out);
}

bool writeBoolScalarRowIfChanged(const RefinedRunContext& ctx,
                                 const std::string& path,
                                 size_t roi_index,
                                 bool value,
                                 std::string* error_message,
                                 bool* changed_out) {
    const auto meta = readNodeMetaV3(ctx.store, path);
    if (!meta.has_value()) {
        if (changed_out != nullptr) {
            *changed_out = false;
        }
        return true;
    }
    const std::string dtype = extractDataTypeName(*meta);
    if (dtype == "uint8") {
        return writeFlatRowIfChangedTyped<uint8_t, 1>(
            ctx.store, ctx.tensorstore_context, path, roi_index,
            {static_cast<uint8_t>(value ? 1 : 0)}, {1}, error_message, changed_out);
    }
    if (dtype == "int8" || dtype == "int32") {
        return writeIntScalarRowIfChanged(
            ctx, path, roi_index, value ? 1 : 0, error_message, changed_out);
    }
    return writeFlatRowIfChangedTyped<bool, 1>(
        ctx.store, ctx.tensorstore_context, path, roi_index,
        {value}, {1}, error_message, changed_out);
}

bool writeFloat2DRowIfChanged(const RefinedRunContext& ctx,
                              const std::string& path,
                              size_t roi_index,
                              const std::vector<double>& values,
                              size_t cols,
                              std::string* error_message,
                              bool* changed_out) {
    const auto meta = readNodeMetaV3(ctx.store, path);
    if (!meta.has_value()) {
        if (changed_out != nullptr) {
            *changed_out = false;
        }
        return true;
    }
    const auto shape = readShapeFromNodeMeta(*meta);
    if (!shape.has_value() || shape->size() != 2 || static_cast<size_t>((*shape)[1]) != cols) {
        if (error_message != nullptr) {
            *error_message = "Unexpected shape for array '" + path + "'.";
        }
        return false;
    }
    const std::string dtype = extractDataTypeName(*meta);
    if (dtype == "float32") {
        std::vector<float> casted;
        casted.reserve(values.size());
        for (double value : values) {
            casted.push_back(static_cast<float>(value));
        }
        return writeFlatRowIfChangedTyped<float, 2>(
            ctx.store, ctx.tensorstore_context, path, roi_index, casted,
            {1, static_cast<ts::Index>(cols)}, error_message, changed_out);
    }
    return writeFlatRowIfChangedTyped<double, 2>(
        ctx.store, ctx.tensorstore_context, path, roi_index, values,
        {1, static_cast<ts::Index>(cols)}, error_message, changed_out);
}

bool writeFloat3DRowIfChanged(const RefinedRunContext& ctx,
                              const std::string& path,
                              size_t roi_index,
                              const std::vector<double>& values,
                              size_t dim1,
                              size_t dim2,
                              std::string* error_message,
                              bool* changed_out) {
    const auto meta = readNodeMetaV3(ctx.store, path);
    if (!meta.has_value()) {
        if (changed_out != nullptr) {
            *changed_out = false;
        }
        return true;
    }
    const auto shape = readShapeFromNodeMeta(*meta);
    if (!shape.has_value() || shape->size() != 3 ||
        static_cast<size_t>((*shape)[1]) != dim1 ||
        static_cast<size_t>((*shape)[2]) != dim2) {
        if (error_message != nullptr) {
            *error_message = "Unexpected shape for array '" + path + "'.";
        }
        return false;
    }
    const std::string dtype = extractDataTypeName(*meta);
    if (dtype == "float32") {
        std::vector<float> casted;
        casted.reserve(values.size());
        for (double value : values) {
            casted.push_back(static_cast<float>(value));
        }
        return writeFlatRowIfChangedTyped<float, 3>(
            ctx.store, ctx.tensorstore_context, path, roi_index, casted,
            {1, static_cast<ts::Index>(dim1), static_cast<ts::Index>(dim2)},
            error_message, changed_out);
    }
    return writeFlatRowIfChangedTyped<double, 3>(
        ctx.store, ctx.tensorstore_context, path, roi_index, values,
        {1, static_cast<ts::Index>(dim1), static_cast<ts::Index>(dim2)},
        error_message, changed_out);
}

bool readStringRowsAsByteMatrix(const ts::kvstore::KvStore& store,
                                const ts::Context& context,
                                const std::string& path,
                                std::vector<std::string>& out) {
    auto read_uint8 = [&](auto type_token) -> bool {
        using Element = decltype(type_token);
        auto open_result = openArrayAny<Element, 2>(store, path, context);
        if (!open_result.ok()) {
            return false;
        }
        auto array_result = ts::Read(open_result.value()).result();
        if (!array_result.ok()) {
            return false;
        }
        auto array = array_result.value();
        if (array.rank() != 2) {
            return false;
        }
        const size_t rows = static_cast<size_t>(array.shape()[0]);
        const size_t width = static_cast<size_t>(array.shape()[1]);
        out.resize(rows);
        const auto* data = static_cast<const Element*>(array.data());
        for (size_t row = 0; row < rows; ++row) {
            const auto* row_ptr = data + row * width;
            size_t length = 0;
            while (length < width && row_ptr[length] != static_cast<Element>(0)) {
                ++length;
            }
            out[row] = std::string(
                reinterpret_cast<const char*>(row_ptr),
                reinterpret_cast<const char*>(row_ptr + length));
        }
        return true;
    };

    return read_uint8(uint8_t{}) || read_uint8(char{});
}

bool readStringRowsAsStringArray(const ts::kvstore::KvStore& store,
                                 const ts::Context& context,
                                 const std::string& path,
                                 std::vector<std::string>& out) {
    auto open_result = openArrayAny<std::string, 1>(store, path, context);
    if (!open_result.ok()) {
        return false;
    }
    auto array_result = ts::Read(open_result.value()).result();
    if (!array_result.ok()) {
        return false;
    }
    auto array = array_result.value();
    if (array.rank() != 1) {
        return false;
    }
    const size_t rows = static_cast<size_t>(array.shape()[0]);
    out.resize(rows);
    for (size_t row = 0; row < rows; ++row) {
        out[row] = array(static_cast<ts::Index>(row));
    }
    return true;
}

ReasonLabelsState readReasonLabels(const RefinedRunContext& ctx) {
    ReasonLabelsState state;
    const std::string reason_bytes_path = ctx.run_path + "/reason_bytes";
    const std::string reason_text_path = ctx.run_path + "/reason";
    state.reason_bytes_exists = readNodeMetaV3(ctx.store, reason_bytes_path).has_value();
    state.reason_exists = readNodeMetaV3(ctx.store, reason_text_path).has_value();

    if (state.reason_bytes_exists &&
        readStringRowsAsByteMatrix(
            ctx.store, ctx.tensorstore_context, reason_bytes_path, state.labels)) {
        state.decoded = true;
        return state;
    }

    if (state.reason_exists) {
        auto node_meta = readNodeMetaV3(ctx.store, reason_text_path);
        const bool unsupported_string_dtype =
            node_meta.has_value() && usesZarrV3StringDataType(*node_meta);
        if (!unsupported_string_dtype &&
            (readStringRowsAsStringArray(
                 ctx.store, ctx.tensorstore_context, reason_text_path, state.labels) ||
             readStringRowsAsByteMatrix(
                 ctx.store, ctx.tensorstore_context, reason_text_path, state.labels))) {
            state.decoded = true;
            return state;
        }
    }

    state.labels.assign(ctx.total_rois, std::string());
    return state;
}

std::vector<uint8_t> encodeReasonBytes(const std::vector<std::string>& labels,
                                       size_t width) {
    std::vector<uint8_t> out(labels.size() * width, 0);
    for (size_t row = 0; row < labels.size(); ++row) {
        const std::string& text = labels[row];
        const size_t base = row * width;
        const size_t copy_len = std::min(text.size(), width > 0 ? width - 1 : 0);
        if (copy_len > 0) {
            std::memcpy(out.data() + base, text.data(), copy_len);
        }
        if (width > 0) {
            out[base + copy_len] = 0;
        }
    }
    return out;
}

bool writeReasonBytesRow(const RefinedRunContext& ctx,
                         const ReasonLabelsState& state,
                         size_t roi_index,
                         const std::string& value,
                         std::string* error_message,
                         bool* changed_out) {
    std::vector<std::string> labels = state.labels;
    if (labels.size() < ctx.total_rois) {
        labels.resize(ctx.total_rois);
    }
    if (roi_index >= labels.size()) {
        if (error_message != nullptr) {
            *error_message = "Reason label index out of bounds.";
        }
        return false;
    }
    if (labels[roi_index] == value && state.reason_bytes_exists) {
        if (changed_out != nullptr) {
            *changed_out = false;
        }
        return true;
    }
    labels[roi_index] = value;

    const std::string path = ctx.run_path + "/reason_bytes";
    auto meta = readNodeMetaV3(ctx.store, path);
    if (meta.has_value()) {
        const auto shape = readShapeFromNodeMeta(*meta);
        if (!shape.has_value() || shape->size() != 2) {
            if (error_message != nullptr) {
                *error_message = "Unexpected shape for existing reason_bytes array.";
            }
            return false;
        }
        size_t width = static_cast<size_t>((*shape)[1]);
        width = std::max<size_t>(width, 1);
        if (value.size() + 1 <= width) {
            std::vector<uint8_t> row_bytes(width, 0);
            const size_t copy_len = std::min(value.size(), width - 1);
            if (copy_len > 0) {
                std::memcpy(row_bytes.data(), value.data(), copy_len);
            }
            return writeFlatRowIfChangedTyped<uint8_t, 2>(
                ctx.store, ctx.tensorstore_context, path, roi_index, row_bytes,
                {1, static_cast<ts::Index>(width)}, error_message, changed_out);
        }
    } else if (state.reason_exists && !state.decoded) {
        if (error_message != nullptr) {
            *error_message =
                "Cannot create reason_bytes because existing reason labels could not be decoded.";
        }
        return false;
    }

    size_t width = 16;
    for (const std::string& label : labels) {
        width = std::max(width, label.size() + 1);
    }
    std::string write_error;
    if (!writeNumericArray2DFlat<uint8_t>(
            ctx.store,
            ctx.tensorstore_context,
            path,
            "uint8",
            encodeReasonBytes(labels, width),
            static_cast<ts::Index>(labels.size()),
            static_cast<ts::Index>(width),
            std::max<ts::Index>(1, std::min<ts::Index>(1024, static_cast<ts::Index>(labels.size()))),
            static_cast<ts::Index>(width),
            0,
            false,
            &write_error)) {
        if (error_message != nullptr) {
            *error_message = write_error;
        }
        return false;
    }
    if (changed_out != nullptr) {
        *changed_out = true;
    }
    return true;
}

std::vector<std::string> splitReasonTags(const std::string& value) {
    std::vector<std::string> tags;
    std::stringstream stream(value);
    std::string item;
    while (std::getline(stream, item, '|')) {
        if (!item.empty()) {
            tags.push_back(item);
        }
    }
    return tags;
}

std::string buildManualReason(const std::string& existing, bool geom_ok) {
    const std::vector<std::string> existing_tags = splitReasonTags(existing);
    const std::vector<std::string> drop_tags = {
        "detection_failed",
        "low_confidence",
        "confidence_missing",
        "fish_present_no_keypoints",
        "detection_issue",
        "manual_correction",
        "geometry_issue",
    };

    std::vector<std::string> tags;
    for (const std::string& tag : existing_tags) {
        if (std::find(drop_tags.begin(), drop_tags.end(), tag) == drop_tags.end()) {
            tags.push_back(tag);
        }
    }
    tags.push_back("manual_correction");
    if (!geom_ok) {
        tags.push_back("geometry_issue");
    }

    std::vector<std::string> unique;
    for (const std::string& tag : tags) {
        if (tag.empty()) {
            continue;
        }
        if (std::find(unique.begin(), unique.end(), tag) == unique.end()) {
            unique.push_back(tag);
        }
    }

    if (unique.empty()) {
        return "manual_correction";
    }

    std::ostringstream joined;
    for (size_t i = 0; i < unique.size(); ++i) {
        if (i > 0) {
            joined << '|';
        }
        joined << unique[i];
    }
    return joined.str();
}

std::string buildFailureReason(const std::string& existing,
                               const std::string& new_tag) {
    std::vector<std::string> tags;
    for (const std::string& tag : splitReasonTags(existing)) {
        if (tag != "detection_failed") {
            tags.push_back(tag);
        }
    }
    tags.push_back(new_tag);
    std::sort(tags.begin(), tags.end());
    tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
    if (tags.empty()) {
        return "manual_correction";
    }
    std::ostringstream joined;
    for (size_t i = 0; i < tags.size(); ++i) {
        if (i > 0) {
            joined << '|';
        }
        joined << tags[i];
    }
    return joined.str();
}

bool readBoolVectorAny(const RefinedRunContext& ctx,
                       const std::string& path,
                       std::vector<uint8_t>& out) {
    auto bool_store = openArrayAny<bool, 1>(ctx.store, path, ctx.tensorstore_context);
    if (bool_store.ok()) {
        auto result = ts::Read(bool_store.value()).result();
        if (result.ok()) {
            auto array = result.value();
            out.resize(static_cast<size_t>(array.shape()[0]));
            const bool* data = static_cast<const bool*>(array.data());
            for (size_t i = 0; i < out.size(); ++i) {
                out[i] = data[i] ? 1 : 0;
            }
            return true;
        }
    }

    auto u8_store = openArrayAny<uint8_t, 1>(ctx.store, path, ctx.tensorstore_context);
    if (u8_store.ok()) {
        auto result = ts::Read(u8_store.value()).result();
        if (result.ok()) {
            auto array = result.value();
            out.resize(static_cast<size_t>(array.shape()[0]));
            const uint8_t* data = static_cast<const uint8_t*>(array.data());
            std::copy(data, data + out.size(), out.begin());
            return true;
        }
    }

    auto i8_store = openArrayAny<int8_t, 1>(ctx.store, path, ctx.tensorstore_context);
    if (i8_store.ok()) {
        auto result = ts::Read(i8_store.value()).result();
        if (result.ok()) {
            auto array = result.value();
            out.resize(static_cast<size_t>(array.shape()[0]));
            const int8_t* data = static_cast<const int8_t*>(array.data());
            for (size_t i = 0; i < out.size(); ++i) {
                out[i] = data[i] != 0 ? 1 : 0;
            }
            return true;
        }
    }

    auto i32_store = openArrayAny<int32_t, 1>(ctx.store, path, ctx.tensorstore_context);
    if (i32_store.ok()) {
        auto result = ts::Read(i32_store.value()).result();
        if (result.ok()) {
            auto array = result.value();
            out.resize(static_cast<size_t>(array.shape()[0]));
            const int32_t* data = static_cast<const int32_t*>(array.data());
            for (size_t i = 0; i < out.size(); ++i) {
                out[i] = data[i] != 0 ? 1 : 0;
            }
            return true;
        }
    }

    return false;
}

bool readIntVectorAny(const RefinedRunContext& ctx,
                      const std::string& path,
                      std::vector<int32_t>& out) {
    auto i32_store = openArrayAny<int32_t, 1>(ctx.store, path, ctx.tensorstore_context);
    if (i32_store.ok()) {
        auto result = ts::Read(i32_store.value()).result();
        if (result.ok()) {
            auto array = result.value();
            out.resize(static_cast<size_t>(array.shape()[0]));
            const int32_t* data = static_cast<const int32_t*>(array.data());
            std::copy(data, data + out.size(), out.begin());
            return true;
        }
    }

    auto i8_store = openArrayAny<int8_t, 1>(ctx.store, path, ctx.tensorstore_context);
    if (i8_store.ok()) {
        auto result = ts::Read(i8_store.value()).result();
        if (result.ok()) {
            auto array = result.value();
            out.resize(static_cast<size_t>(array.shape()[0]));
            const int8_t* data = static_cast<const int8_t*>(array.data());
            for (size_t i = 0; i < out.size(); ++i) {
                out[i] = static_cast<int32_t>(data[i]);
            }
            return true;
        }
    }

    auto u8_store = openArrayAny<uint8_t, 1>(ctx.store, path, ctx.tensorstore_context);
    if (u8_store.ok()) {
        auto result = ts::Read(u8_store.value()).result();
        if (result.ok()) {
            auto array = result.value();
            out.resize(static_cast<size_t>(array.shape()[0]));
            const uint8_t* data = static_cast<const uint8_t*>(array.data());
            for (size_t i = 0; i < out.size(); ++i) {
                out[i] = static_cast<int32_t>(data[i]);
            }
            return true;
        }
    }

    auto bool_store = openArrayAny<bool, 1>(ctx.store, path, ctx.tensorstore_context);
    if (bool_store.ok()) {
        auto result = ts::Read(bool_store.value()).result();
        if (result.ok()) {
            auto array = result.value();
            out.resize(static_cast<size_t>(array.shape()[0]));
            const bool* data = static_cast<const bool*>(array.data());
            for (size_t i = 0; i < out.size(); ++i) {
                out[i] = data[i] ? 1 : 0;
            }
            return true;
        }
    }

    return false;
}

int countTruthyPath(const RefinedRunContext& ctx, const std::string& path) {
    std::vector<uint8_t> values;
    if (!readBoolVectorAny(ctx, path, values)) {
        return 0;
    }
    int count = 0;
    for (uint8_t value : values) {
        if (value != 0) {
            ++count;
        }
    }
    return count;
}

json countIntValuesPath(const RefinedRunContext& ctx, const std::string& path) {
    std::vector<int32_t> values;
    if (!readIntVectorAny(ctx, path, values)) {
        return json::object();
    }
    std::map<std::string, int> counts;
    for (int32_t value : values) {
        counts[std::to_string(value)] += 1;
    }
    json out = json::object();
    for (const auto& [key, count] : counts) {
        out[key] = count;
    }
    return out;
}

json countReasonTags(const std::vector<std::string>& labels) {
    std::map<std::string, int> counts;
    for (const std::string& label : labels) {
        for (const std::string& tag : splitReasonTags(label)) {
            counts[tag] += 1;
        }
    }
    json out = json::object();
    for (const auto& [key, count] : counts) {
        out[key] = count;
    }
    return out;
}

std::vector<int> mergeIntList(const json& existing, const std::vector<int>& add_values) {
    std::vector<int> merged;
    if (existing.is_array()) {
        for (const auto& item : existing) {
            if (item.is_number_integer() || item.is_number_unsigned()) {
                merged.push_back(static_cast<int>(item.get<int64_t>()));
            }
        }
    }
    for (int value : add_values) {
        merged.push_back(value);
    }
    std::sort(merged.begin(), merged.end());
    merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
    if (merged.size() > kMaxStaleIndexHistory) {
        merged.erase(merged.begin(), merged.end() - static_cast<std::ptrdiff_t>(kMaxStaleIndexHistory));
    }
    return merged;
}

std::string resolveSourceKeypointsRun(const json& attrs) {
    if (!attrs.is_object()) {
        return "";
    }
    if (attrs.contains("source_keypoints_run") &&
        attrs["source_keypoints_run"].is_string()) {
        return attrs["source_keypoints_run"].get<std::string>();
    }
    if (attrs.contains("source_keypoint_run") &&
        attrs["source_keypoint_run"].is_string()) {
        return attrs["source_keypoint_run"].get<std::string>();
    }
    return "";
}

std::vector<std::string> listRunGroupNames(const std::string& archive_path,
                                           const std::string& parent_name) {
    namespace fs = std::filesystem;
    std::vector<std::string> names;
    const fs::path parent_path = fs::path(archive_path) / parent_name;
    if (!fs::exists(parent_path) || !fs::is_directory(parent_path)) {
        return names;
    }
    for (const auto& entry : fs::directory_iterator(parent_path)) {
        if (!entry.is_directory()) {
            continue;
        }
        if (!fs::exists(entry.path() / "zarr.json")) {
            continue;
        }
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

int markDownstreamEyeMaskRunsStale(const RefinedRunContext& ctx,
                                   const std::vector<int>& roi_indices,
                                   const std::vector<int>& frame_indices,
                                   const std::string& reason,
                                   std::string* error_message) {
    if (ctx.parent_name.empty() || ctx.run_name.empty()) {
        return 0;
    }

    const std::string timestamp = currentUtcIsoTimestamp();
    int touched = 0;
    for (const std::string& parent_name :
         {"eye_masks_runs", "refined_eye_masks_runs"}) {
        for (const std::string& run_name :
             listRunGroupNames(ctx.archive_path, parent_name)) {
            const std::string run_path = parent_name + "/" + run_name;
            auto run_meta = readNodeMetaV3(ctx.store, run_path);
            if (!run_meta.has_value()) {
                continue;
            }
            json normalized = normalizeGroupMetadataV3(*run_meta);
            json& attrs = normalized["attributes"];
            if (resolveSourceKeypointsRun(attrs) != ctx.run_name) {
                continue;
            }
            if (attrs.contains("source_keypoint_group") &&
                attrs["source_keypoint_group"].is_string() &&
                attrs["source_keypoint_group"].get<std::string>() != ctx.parent_name) {
                continue;
            }

            json payload = json::object();
            if (attrs.contains("source_keypoint_stale") &&
                attrs["source_keypoint_stale"].is_object()) {
                payload = attrs["source_keypoint_stale"];
            }
            payload["state"] = "stale";
            payload["timestamp"] = timestamp;
            payload["reason"] = reason;
            payload["source_keypoint_group"] = ctx.parent_name;
            payload["source_keypoints_run"] = ctx.run_name;
            payload["roi_indices"] =
                mergeIntList(payload.value("roi_indices", json::array()), roi_indices);
            payload["frame_indices"] =
                mergeIntList(payload.value("frame_indices", json::array()), frame_indices);
            attrs["source_keypoint_stale"] = payload;

            std::string write_error;
            if (!writeNodeMetaV3(ctx.store, run_path, normalized, &write_error)) {
                if (error_message != nullptr) {
                    *error_message = write_error;
                }
                return touched;
            }
            ++touched;
        }
    }
    return touched;
}

bool openEditableRunContext(const ZarrDetectionLoader& loader,
                            RefinedRunContext& out,
                            std::string& error_message) {
    error_message.clear();
    const std::string archive_path = loader.getArchivePath();
    if (archive_path.empty()) {
        error_message = "No loaded Zarr archive to write into.";
        return false;
    }

    const std::string kvstore_path = normalizeKvstoreFileRootPath(archive_path);
    auto kv_spec = ts::kvstore::Spec::FromJson(
        {{"driver", "file"}, {"path", kvstore_path}});
    if (!kv_spec.ok()) {
        error_message =
            "Failed to create kvstore spec: " + kv_spec.status().ToString();
        return false;
    }

    auto store_result = ts::kvstore::Open(kv_spec.value()).result();
    if (!store_result.ok()) {
        error_message =
            "Failed to open kvstore: " + store_result.status().ToString();
        return false;
    }

    std::string parent_name;
    if (auto attrs = readAttrsAny(store_result.value(), "refined_keypoints_runs");
        attrs.has_value() && attrs->is_object()) {
        parent_name = "refined_keypoints_runs";
    } else if (auto attrs = readAttrsAny(store_result.value(), "keypoints_refined_runs");
               attrs.has_value() && attrs->is_object()) {
        parent_name = "keypoints_refined_runs";
    } else {
        error_message =
            "No refined_keypoints_runs parent exists in the loaded archive.";
        return false;
    }

    const std::string run_name = loader.getKeypointsRunName();
    if (run_name.empty()) {
        error_message = "Loaded refined keypoints have no resolved run name.";
        return false;
    }

    const std::string run_path = parent_name + "/" + run_name;
    auto run_meta = readNodeMetaV3(store_result.value(), run_path);
    if (!run_meta.has_value()) {
        error_message = "Refined keypoint run metadata is missing: " + run_path;
        return false;
    }

    auto keypoints_roi_meta =
        readNodeMetaV3(store_result.value(), run_path + "/keypoints_roi");
    if (!keypoints_roi_meta.has_value()) {
        error_message = "Refined run is missing keypoints_roi.";
        return false;
    }
    auto shape = readShapeFromNodeMeta(*keypoints_roi_meta);
    if (!shape.has_value() || shape->size() != 3 || (*shape)[0] < 0) {
        error_message = "Refined run keypoints_roi has invalid shape metadata.";
        return false;
    }

    out.store = store_result.value();
    out.archive_path = archive_path;
    out.parent_name = parent_name;
    out.run_name = run_name;
    out.run_path = run_path;
    out.run_meta = normalizeGroupMetadataV3(*run_meta);
    out.total_rois = static_cast<size_t>((*shape)[0]);
    out.image_width = loader.getImageWidth();
    out.image_height = loader.getImageHeight();

    const json& attrs = out.run_meta["attributes"];
    json summary_raw = attrs.value("summary_statistics", json::object());
    json refine_summary = summary_raw;
    if (summary_raw.is_object() &&
        summary_raw.contains("refine") &&
        summary_raw["refine"].is_object()) {
        refine_summary = summary_raw["refine"];
    }
    if (refine_summary.is_object()) {
        if (refine_summary.contains("confidence_threshold")) {
            out.confidence_threshold =
                refine_summary["confidence_threshold"].get<double>();
        }
        if (refine_summary.contains("min_triangle_angle")) {
            out.min_triangle_angle =
                refine_summary["min_triangle_angle"].get<double>();
        }
        if (refine_summary.contains("min_triangle_area")) {
            out.min_triangle_area =
                refine_summary["min_triangle_area"].get<double>();
        }
        if (refine_summary.contains("max_triangle_area") &&
            !refine_summary["max_triangle_area"].is_null()) {
            out.max_triangle_area =
                refine_summary["max_triangle_area"].get<double>();
        }
    }
    return true;
}

bool updatePostprocessSummary(RefinedRunContext& ctx,
                              std::string& error_message,
                              bool* changed_out) {
    error_message.clear();
    const std::string run_base = ctx.run_path + "/";
    ReasonLabelsState reason_state = readReasonLabels(ctx);
    json reason_counts = countReasonTags(reason_state.labels);

    const int total_rois = static_cast<int>(ctx.total_rois);
    const int source_success = countTruthyPath(ctx, run_base + "source_success");
    const int refined_success = countTruthyPath(ctx, run_base + "refined_success");
    const int usable_keypoints = countTruthyPath(ctx, run_base + "usable_keypoints");
    const int confidence_valid = countTruthyPath(ctx, run_base + "confidence_valid");
    const int geometry_valid = countTruthyPath(ctx, run_base + "geometry_valid");
    const int flip_corrected = countTruthyPath(ctx, run_base + "flip_corrected");
    const int heading_finite = countTruthyPath(ctx, run_base + "heading_finite");
    const int heading_usable = countTruthyPath(ctx, run_base + "heading_usable");
    const int remaining_failures = std::max(0, total_rois - refined_success);
    const double success_rate =
        total_rois > 0 ? (static_cast<double>(refined_success) / total_rois) * 100.0
                       : 0.0;

    json detection_source_counts =
        countIntValuesPath(ctx, run_base + "detection_source");
    json retune_id_counts = countIntValuesPath(ctx, run_base + "retune_id");
    int retune_total = 0;
    if (retune_id_counts.is_object()) {
        for (auto it = retune_id_counts.begin(); it != retune_id_counts.end(); ++it) {
            if (it.key() != "-1" && it.value().is_number_integer()) {
                retune_total += it.value().get<int>();
            }
        }
    }
    int manual_corrections = 0;
    if (reason_counts.contains("manual_correction") &&
        reason_counts["manual_correction"].is_number_integer()) {
        manual_corrections = reason_counts["manual_correction"].get<int>();
    }

    json post_stats = {
        {"total_rois", total_rois},
        {"source_success", source_success},
        {"refined_success", refined_success},
        {"remaining_failures", remaining_failures},
        {"success_rate_percent", std::round(success_rate * 100.0) / 100.0},
        {"usable_keypoints", usable_keypoints},
        {"confidence_valid", confidence_valid},
        {"geometry_valid", geometry_valid},
        {"flip_corrected", flip_corrected},
        {"heading_finite", heading_finite},
        {"heading_usable", heading_usable},
        {"detection_source_counts", detection_source_counts},
        {"reason_counts", reason_counts},
        {"retune_id_counts", retune_id_counts},
        {"retune_total", retune_total},
        {"manual_corrections", manual_corrections},
    };

    json& run_attrs = ctx.run_meta["attributes"];
    json summary_stats = run_attrs.value("summary_statistics", json::object());
    if (!summary_stats.is_object()) {
        summary_stats = json::object();
    }
    if (!summary_stats.contains("refine") || !summary_stats["refine"].is_object()) {
        json previous = summary_stats;
        summary_stats = json::object();
        summary_stats["refine"] = previous.is_object() ? previous : json::object();
    }

    const json previous_post = summary_stats.value("postprocess", json());
    const bool post_changed = previous_post != post_stats;
    summary_stats["postprocess"] = post_stats;
    if (post_changed || !summary_stats.contains("postprocess_updated_utc")) {
        summary_stats["postprocess_updated_utc"] = currentUtcIsoTimestamp();
    }
    run_attrs["summary_statistics"] = summary_stats;

    if (!post_changed && changed_out != nullptr) {
        *changed_out = false;
    } else if (changed_out != nullptr) {
        *changed_out = true;
    }

    if (!post_changed) {
        return true;
    }
    return writeNodeMetaV3(ctx.store, ctx.run_path, ctx.run_meta, &error_message);
}

bool validateEditableSelection(const RefinedKeypointSelection& selection,
                               const ZarrDetectionLoader& loader,
                               std::string& error_message) {
    if (!selection.valid) {
        error_message = selection.message.empty()
                            ? "Selected detection has no editable keypoint ROI."
                            : selection.message;
        return false;
    }
    if (!selection.editable) {
        error_message =
            "Selected keypoints come from a raw run; refined runs are required for in-place edits.";
        return false;
    }
    if (selection.run_name.empty() ||
        selection.run_name != loader.getKeypointsRunName()) {
        error_message =
            "Selected keypoint run no longer matches the active loaded run.";
        return false;
    }
    if (selection.roi_index < 0) {
        error_message = "Selected detection has an invalid ROI index.";
        return false;
    }
    return true;
}

bool applyKeypointEdit(const ZarrDetectionLoader& loader,
                       const RefinedKeypointSelection& selection,
                       KeypointEditMode mode,
                       const std::array<std::array<double, 2>, 3>* manual_points,
                       std::string& error_message,
                       RefinedKeypointEditResult* edit_result) {
    error_message.clear();
    if (edit_result != nullptr) {
        *edit_result = RefinedKeypointEditResult{};
    }

    if (!validateEditableSelection(selection, loader, error_message)) {
        return false;
    }

    RefinedRunContext ctx;
    if (!openEditableRunContext(loader, ctx, error_message)) {
        return false;
    }

    const size_t roi_index = static_cast<size_t>(selection.roi_index);
    if (roi_index >= ctx.total_rois) {
        error_message = "Selected ROI index is out of bounds for the active run.";
        return false;
    }

    const std::string run_base = ctx.run_path + "/";
    const bool needs_full_coordinates =
        arrayExists(ctx.store, run_base + "keypoints_img") ||
        arrayExists(ctx.store, run_base + "keypoints_norm");
    if (needs_full_coordinates && !selection.roi_metadata.has_crop_metadata) {
        error_message =
            "Selected ROI has no crop placement metadata; cannot update keypoints_img/keypoints_norm.";
        return false;
    }
    if (arrayExists(ctx.store, run_base + "keypoints_norm") &&
        (ctx.image_width <= 0 || ctx.image_height <= 0)) {
        error_message =
            "Loaded archive has no valid image dimensions for normalized keypoint writes.";
        return false;
    }

    std::array<std::array<double, 2>, kManualKeypointCount> points_roi{};
    if (mode == KeypointEditMode::ManualCorrection) {
        if (manual_points == nullptr) {
            error_message = "Manual correction points are missing.";
            return false;
        }
        points_roi = *manual_points;
        for (const auto& point : points_roi) {
            if (!std::isfinite(point[0]) || !std::isfinite(point[1])) {
                error_message =
                    "Manual correction points must all be finite ROI coordinates.";
                return false;
            }
        }
    } else {
        for (auto& point : points_roi) {
            point[0] = std::numeric_limits<double>::quiet_NaN();
            point[1] = std::numeric_limits<double>::quiet_NaN();
        }
    }

    std::array<std::array<double, 2>, kManualKeypointCount> points_img{};
    std::array<std::array<double, 2>, kManualKeypointCount> points_norm{};
    if (mode == KeypointEditMode::ManualCorrection) {
        const double offset_x = selection.roi_metadata.offset_x;
        const double offset_y = selection.roi_metadata.offset_y;
        for (size_t i = 0; i < kManualKeypointCount; ++i) {
            points_img[i][0] = points_roi[i][0] + offset_x;
            points_img[i][1] = points_roi[i][1] + offset_y;
            points_norm[i][0] =
                points_img[i][0] / static_cast<double>(ctx.image_width);
            points_norm[i][1] =
                points_img[i][1] / static_cast<double>(ctx.image_height);
        }
    } else {
        for (size_t i = 0; i < kManualKeypointCount; ++i) {
            points_img[i][0] = std::numeric_limits<double>::quiet_NaN();
            points_img[i][1] = std::numeric_limits<double>::quiet_NaN();
            points_norm[i][0] = std::numeric_limits<double>::quiet_NaN();
            points_norm[i][1] = std::numeric_limits<double>::quiet_NaN();
        }
    }

    const KeypointHeadingComputationSpec& heading_spec =
        loader.getHeadingComputationSpec();
    const double heading_value =
        mode == KeypointEditMode::ManualCorrection
            ? computeHeadingFromPoints(heading_spec, points_roi)
            : std::numeric_limits<double>::quiet_NaN();
    const KeypointGeometryMetrics geometry =
        mode == KeypointEditMode::ManualCorrection
            ? computeGeometryMetrics(points_roi)
            : KeypointGeometryMetrics{};

    bool geometry_ok = false;
    bool confidence_ok = false;
    bool refined_success = false;
    bool heading_finite = false;
    bool heading_usable = false;
    double confidence_value = std::numeric_limits<double>::quiet_NaN();
    std::array<double, 3> keypoint_confidences = {
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::quiet_NaN(),
    };
    if (mode == KeypointEditMode::ManualCorrection) {
        const bool max_ok = !ctx.max_triangle_area.has_value() ||
                            geometry.area <= *ctx.max_triangle_area;
        geometry_ok =
            std::isfinite(geometry.min_angle) &&
            std::isfinite(geometry.area) &&
            geometry.min_angle >= ctx.min_triangle_angle &&
            geometry.area >= ctx.min_triangle_area &&
            max_ok;
        confidence_value = 1.0;
        keypoint_confidences = {1.0, 1.0, 1.0};
        confidence_ok = confidence_value >= ctx.confidence_threshold;
        refined_success = true;
        heading_finite = std::isfinite(heading_value);

        std::vector<int32_t> detection_source;
        if (readIntVectorAny(ctx, run_base + "detection_source", detection_source) &&
            roi_index < detection_source.size()) {
            heading_usable = refined_success &&
                             detection_source[roi_index] == 0 &&
                             heading_finite;
        } else {
            heading_usable = refined_success && heading_finite;
        }
    }

    bool any_change = false;
    auto write_change = [&](bool success, bool changed) -> bool {
        if (!success) {
            return false;
        }
        any_change = any_change || changed;
        return true;
    };

    auto flatten_points = [](const auto& points) {
        std::vector<double> flat;
        flat.reserve(kManualKeypointCount * kManualKeypointDims);
        for (const auto& point : points) {
            flat.push_back(point[0]);
            flat.push_back(point[1]);
        }
        return flat;
    };

    bool changed = false;
    if (!write_change(
            writeFloat3DRowIfChanged(
                ctx, run_base + "keypoints_roi", roi_index, flatten_points(points_roi),
                kManualKeypointCount, kManualKeypointDims, &error_message, &changed),
            changed)) {
        return false;
    }
    if (!write_change(
            writeFloat3DRowIfChanged(
                ctx, run_base + "keypoints_img", roi_index, flatten_points(points_img),
                kManualKeypointCount, kManualKeypointDims, &error_message, &changed),
            changed)) {
        return false;
    }
    if (!write_change(
            writeFloat3DRowIfChanged(
                ctx, run_base + "keypoints_norm", roi_index, flatten_points(points_norm),
                kManualKeypointCount, kManualKeypointDims, &error_message, &changed),
            changed)) {
        return false;
    }
    if (!write_change(
            writeFloatScalarRowIfChanged(
                ctx, run_base + "heading", roi_index, heading_value, &error_message, &changed),
            changed)) {
        return false;
    }
    if (!write_change(
            writeFloatScalarRowIfChanged(
                ctx, run_base + "confidence", roi_index, confidence_value, &error_message,
                &changed),
            changed)) {
        return false;
    }
    if (!write_change(
            writeFloat2DRowIfChanged(
                ctx, run_base + "keypoint_confidences", roi_index,
                {keypoint_confidences[0], keypoint_confidences[1],
                 keypoint_confidences[2]},
                3, &error_message, &changed),
            changed)) {
        return false;
    }
    if (!write_change(
            writeFloatScalarRowIfChanged(
                ctx, run_base + "triangle_area", roi_index, geometry.area, &error_message,
                &changed),
            changed)) {
        return false;
    }
    if (!write_change(
            writeFloatScalarRowIfChanged(
                ctx, run_base + "min_angle", roi_index, geometry.min_angle, &error_message,
                &changed),
            changed)) {
        return false;
    }
    if (!write_change(
            writeFloat2DRowIfChanged(
                ctx, run_base + "triangle_angles", roi_index,
                {geometry.angles[0], geometry.angles[1], geometry.angles[2]},
                3, &error_message, &changed),
            changed)) {
        return false;
    }
    if (!write_change(
            writeBoolScalarRowIfChanged(
                ctx, run_base + "refined_success", roi_index, refined_success,
                &error_message, &changed),
            changed)) {
        return false;
    }
    if (!write_change(
            writeBoolScalarRowIfChanged(
                ctx, run_base + "flip_corrected", roi_index, false,
                &error_message, &changed),
            changed)) {
        return false;
    }
    if (!write_change(
            writeIntScalarRowIfChanged(
                ctx, run_base + "quality_labels", roi_index, 0, &error_message, &changed),
            changed)) {
        return false;
    }
    if (!write_change(
            writeBoolScalarRowIfChanged(
                ctx, run_base + "confidence_valid", roi_index, confidence_ok,
                &error_message, &changed),
            changed)) {
        return false;
    }
    if (!write_change(
            writeBoolScalarRowIfChanged(
                ctx, run_base + "geometry_valid", roi_index, geometry_ok,
                &error_message, &changed),
            changed)) {
        return false;
    }
    if (!write_change(
            writeBoolScalarRowIfChanged(
                ctx, run_base + "usable_keypoints", roi_index,
                confidence_ok && geometry_ok, &error_message, &changed),
            changed)) {
        return false;
    }
    if (!write_change(
            writeBoolScalarRowIfChanged(
                ctx, run_base + "heading_finite", roi_index, heading_finite,
                &error_message, &changed),
            changed)) {
        return false;
    }
    if (!write_change(
            writeBoolScalarRowIfChanged(
                ctx, run_base + "heading_usable", roi_index, heading_usable,
                &error_message, &changed),
            changed)) {
        return false;
    }

    ReasonLabelsState reason_state = readReasonLabels(ctx);
    std::string existing_reason;
    if (roi_index < reason_state.labels.size()) {
        existing_reason = reason_state.labels[roi_index];
    }
    const std::string new_reason =
        mode == KeypointEditMode::ManualCorrection
            ? buildManualReason(existing_reason, geometry_ok)
            : buildFailureReason(
                  existing_reason,
                  mode == KeypointEditMode::FishPresentNoKeypoints
                      ? "fish_present_no_keypoints"
                      : "detection_issue");

    if (!write_change(
            writeReasonBytesRow(
                ctx, reason_state, roi_index, new_reason, &error_message, &changed),
            changed)) {
        return false;
    }
    reason_state.labels = reason_state.labels.empty()
                              ? std::vector<std::string>(ctx.total_rois)
                              : reason_state.labels;
    if (roi_index < reason_state.labels.size()) {
        reason_state.labels[roi_index] = new_reason;
    }

    if (!write_change(
            writeBoolScalarRowIfChanged(
                ctx, run_base + "edit_applied", roi_index, true, &error_message, &changed),
            changed)) {
        return false;
    }

    bool summary_changed = false;
    if (any_change &&
        !updatePostprocessSummary(ctx, error_message, &summary_changed)) {
        return false;
    }

    int stale_count = 0;
    if (any_change) {
        const std::string stale_reason =
            mode == KeypointEditMode::ManualCorrection
                ? "keypoint_manual_correction"
                : (mode == KeypointEditMode::FishPresentNoKeypoints
                       ? "keypoint_mark_no_keypoints"
                       : "keypoint_mark_detection_issue");
        stale_count = markDownstreamEyeMaskRunsStale(
            ctx,
            {static_cast<int>(roi_index)},
            {static_cast<int>(selection.frame_id)},
            stale_reason,
            &error_message);
        if (!error_message.empty()) {
            return false;
        }
    }

    if (edit_result != nullptr) {
        edit_result->changed = any_change;
        edit_result->summary_updated = summary_changed;
        edit_result->stale_eye_mask_runs = stale_count;
    }
    return true;
}

}  // namespace

bool RefinedKeypointRepository::canEditActiveRun(std::string* reason) const {
    if (!loader_.hasKeypointData()) {
        if (reason != nullptr) {
            *reason = "No keypoint run is loaded.";
        }
        return false;
    }
    if (loader_.getKeypointsRunName().empty()) {
        if (reason != nullptr) {
            *reason = "Loaded keypoints have no resolved run name.";
        }
        return false;
    }
    if (!loader_.isRefinedKeypoints()) {
        if (reason != nullptr) {
            *reason = "Keypoints are loaded from a raw run; refined runs are required for in-place edits.";
        }
        return false;
    }
    if (reason != nullptr) {
        reason->clear();
    }
    return true;
}

RefinedKeypointSelection
RefinedKeypointRepository::resolveFrameDetectionSelection(
    size_t frame_id,
    size_t detection_index,
    bool use_interpolated) const {
    RefinedKeypointSelection selection;
    selection.frame_id = frame_id;
    selection.detection_index = detection_index;
    selection.run_name = loader_.getKeypointsRunName();
    selection.editable = loader_.isRefinedKeypoints();

    if (!loader_.hasKeypointData()) {
        selection.message = "No keypoint run is loaded.";
        return selection;
    }
    if (selection.run_name.empty()) {
        selection.message = "Loaded keypoints have no resolved run name.";
        return selection;
    }

    selection.roi_metadata = loader_.getKeypointRoiMetadataForFrameDetection(
        frame_id, detection_index, use_interpolated);
    selection.roi_index = selection.roi_metadata.roi_index;
    if (!selection.roi_metadata.valid) {
        selection.message =
            use_interpolated
                ? "Interpolated detections do not have a stable keypoint ROI mapping."
                : "Selected detection has no keypoint ROI mapping.";
        return selection;
    }

    selection.valid = true;
    if (!selection.roi_metadata.has_crop_metadata) {
        selection.message =
            "ROI row resolved, but crop placement metadata is unavailable.";
    }
    return selection;
}

bool RefinedKeypointRepository::writeReviewStatus(
    const RefinedKeypointReviewStatusWriteOptions& options,
    std::string& error_message,
    std::string* resolved_run_name) const {
    error_message.clear();

    std::string edit_reason;
    if (!canEditActiveRun(&edit_reason)) {
        error_message = edit_reason;
        return false;
    }
    if (!isAllowedValue(options.state,
                        {"approved", "pending", "rejected", "needs_review"})) {
        error_message =
            "Review state must be one of approved|pending|rejected|needs_review.";
        return false;
    }
    if (!isAllowedValue(options.method,
                        {"manual", "algorithmic", "hybrid", "spotcheck"})) {
        error_message =
            "Review method must be one of manual|algorithmic|hybrid|spotcheck.";
        return false;
    }
    if (!isAllowedValue(options.intended_use,
                        {"training", "full_recording"})) {
        error_message =
            "Intended use must be one of training|full_recording.";
        return false;
    }

    RefinedRunContext ctx;
    if (!openEditableRunContext(loader_, ctx, error_message)) {
        return false;
    }
    if (resolved_run_name != nullptr) {
        *resolved_run_name = ctx.run_name;
    }

    json& run_attrs = ctx.run_meta["attributes"];
    const std::string timestamp_utc = currentUtcIsoTimestamp();
    json payload = {
        {"state", options.state},
        {"method", options.method},
        {"intended_use", options.intended_use},
        {"timestamp_utc", timestamp_utc},
        {"timestamp", timestamp_utc},
    };
    if (!options.reviewer.empty()) {
        payload["reviewer"] = options.reviewer;
    }
    if (!options.notes.empty()) {
        payload["notes"] = options.notes;
    }

    run_attrs["keypoint_review_status"] = payload;
    json signature = getNestedObjectOrNull(run_attrs, "keypoint_signature");
    if (!signature.is_object()) {
        signature = buildKeypointSignature(run_attrs);
        run_attrs["keypoint_signature"] = signature;
    }
    run_attrs["keypoint_review_signature"] = signature;

    if (!writeNodeMetaV3(ctx.store, ctx.run_path, ctx.run_meta, &error_message)) {
        return false;
    }

    if (options.update_latest) {
        json parent_meta = normalizeGroupMetadataV3(
            readNodeMetaV3(ctx.store, ctx.parent_name).value_or(makeEmptyGroupMetadataV3()));
        parent_meta["attributes"]["keypoint_review_status_latest"] = ctx.run_name;
        if (!writeNodeMetaV3(ctx.store, ctx.parent_name, parent_meta, &error_message)) {
            return false;
        }
    }

    return true;
}

bool RefinedKeypointRepository::writeManualCorrection(
    const RefinedKeypointSelection& selection,
    const std::array<std::array<double, 2>, 3>& keypoints_roi,
    std::string& error_message,
    RefinedKeypointEditResult* edit_result) const {
    return applyKeypointEdit(
        loader_,
        selection,
        KeypointEditMode::ManualCorrection,
        &keypoints_roi,
        error_message,
        edit_result);
}

bool RefinedKeypointRepository::markFishPresentNoKeypoints(
    const RefinedKeypointSelection& selection,
    std::string& error_message,
    RefinedKeypointEditResult* edit_result) const {
    return applyKeypointEdit(
        loader_,
        selection,
        KeypointEditMode::FishPresentNoKeypoints,
        nullptr,
        error_message,
        edit_result);
}

bool RefinedKeypointRepository::markDetectionIssue(
    const RefinedKeypointSelection& selection,
    std::string& error_message,
    RefinedKeypointEditResult* edit_result) const {
    return applyKeypointEdit(
        loader_,
        selection,
        KeypointEditMode::DetectionIssue,
        nullptr,
        error_message,
        edit_result);
}
