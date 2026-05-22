#include "zarr/clipped_detection_repository.h"

#include "zarr_loader_internal.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <sstream>
#include <vector>

using json = nlohmann::json;

namespace {

double elapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - start)
        .count();
}

const char* sourceKindLabel(int32_t code) {
    switch (code) {
        case 0:
            return "none";
        case 1:
            return "raw_detect";
        case 2:
            return "interpolated";
        case 3:
            return "manual";
        default:
            return "unknown";
    }
}

bool finiteBox(const std::array<float, 4>& box) {
    return std::isfinite(box[0]) && std::isfinite(box[1]) &&
           std::isfinite(box[2]) && std::isfinite(box[3]);
}

std::optional<double> jsonNumberAttr(const json& attrs, const char* key) {
    if (!attrs.contains(key)) {
        return std::nullopt;
    }
    const auto& value = attrs.at(key);
    if (value.is_number()) {
        return value.get<double>();
    }
    if (value.is_string()) {
        try {
            size_t consumed = 0;
            const double parsed = std::stod(value.get<std::string>(), &consumed);
            if (consumed > 0) {
                return parsed;
            }
        } catch (const std::exception&) {
        }
    }
    return std::nullopt;
}

std::optional<int> jsonPositiveIntAttr(const json& attrs, const char* key) {
    const auto value = jsonNumberAttr(attrs, key);
    if (!value.has_value() || !std::isfinite(*value) || *value <= 0.0 ||
        *value > static_cast<double>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    return static_cast<int>(std::lround(*value));
}

int firstPositiveIntAttr(const json& attrs,
                         std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        if (auto value = jsonPositiveIntAttr(attrs, key)) {
            return *value;
        }
    }
    return 0;
}

double xyxyBoxError(const std::array<float, 4>& a,
                    const std::array<float, 4>& b) {
    double error = 0.0;
    for (size_t i = 0; i < 4; ++i) {
        error += std::abs(static_cast<double>(a[i]) -
                          static_cast<double>(b[i]));
    }
    return error;
}

bool shouldScaleInferencePixelBoxesToSource(
    const std::vector<std::array<float, 4>>& pixel_xyxy,
    const std::vector<std::array<float, 4>>& normalized_cxcywh,
    int inference_width,
    int inference_height,
    int source_width,
    int source_height) {
    if (inference_width <= 0 || inference_height <= 0 ||
        source_width <= 0 || source_height <= 0 ||
        (inference_width == source_width &&
         inference_height == source_height)) {
        return false;
    }

    if (normalized_cxcywh.size() == pixel_xyxy.size()) {
        const size_t max_samples = 256;
        const size_t stride =
            std::max<size_t>(size_t{1}, pixel_xyxy.size() / max_samples);
        double inference_error = 0.0;
        double source_error = 0.0;
        size_t samples = 0;
        for (size_t row = 0; row < pixel_xyxy.size() &&
                             samples < max_samples; row += stride) {
            if (!finiteBox(pixel_xyxy[row]) ||
                !finiteBox(normalized_cxcywh[row])) {
                continue;
            }
            const auto inference_xyxy = normalizedBoxToPixels(
                normalized_cxcywh[row], inference_width, inference_height);
            const auto source_xyxy = normalizedBoxToPixels(
                normalized_cxcywh[row], source_width, source_height);
            inference_error += xyxyBoxError(pixel_xyxy[row], inference_xyxy);
            source_error += xyxyBoxError(pixel_xyxy[row], source_xyxy);
            ++samples;
        }

        if (samples > 0) {
            const double avg_inference_error =
                inference_error / static_cast<double>(samples);
            const double avg_source_error =
                source_error / static_cast<double>(samples);
            return avg_inference_error <= 2.0 &&
                   avg_source_error > avg_inference_error * 8.0;
        }
    }

    return false;
}

void scalePixelXyxyBoxes(std::vector<std::array<float, 4>>& boxes,
                         int source_width,
                         int source_height,
                         int target_width,
                         int target_height) {
    if (source_width <= 0 || source_height <= 0 ||
        target_width <= 0 || target_height <= 0) {
        return;
    }

    const float scale_x =
        static_cast<float>(target_width) / static_cast<float>(source_width);
    const float scale_y =
        static_cast<float>(target_height) / static_cast<float>(source_height);
    const float max_x = static_cast<float>(target_width);
    const float max_y = static_cast<float>(target_height);

    for (auto& box : boxes) {
        if (!finiteBox(box)) {
            continue;
        }
        box[0] = std::clamp(box[0] * scale_x, 0.0f, max_x);
        box[1] = std::clamp(box[1] * scale_y, 0.0f, max_y);
        box[2] = std::clamp(box[2] * scale_x, 0.0f, max_x);
        box[3] = std::clamp(box[3] * scale_y, 0.0f, max_y);
        if (box[2] < box[0]) {
            std::swap(box[0], box[2]);
        }
        if (box[3] < box[1]) {
            std::swap(box[1], box[3]);
        }
    }
}

struct ClippedRunGeometry {
    int inference_width = 0;
    int inference_height = 0;
    int source_width = 0;
    int source_height = 0;
};

ClippedRunGeometry clippedRunGeometryFromAttrs(const json& attrs) {
    ClippedRunGeometry geometry;
    geometry.inference_width = firstPositiveIntAttr(
        attrs, {"inference_width", "input_width", "image_width", "width"});
    geometry.inference_height = firstPositiveIntAttr(
        attrs, {"inference_height", "input_height", "image_height", "height"});
    geometry.source_width = firstPositiveIntAttr(
        attrs, {"source_full_width", "source_video_width", "video_width",
                "native_width_px"});
    geometry.source_height = firstPositiveIntAttr(
        attrs, {"source_full_height", "source_video_height", "video_height",
                "native_height_px"});
    return geometry;
}

std::string jsonStringAttr(const json& attrs, const char* key) {
    if (!attrs.contains(key)) {
        return "";
    }
    const auto& value = attrs.at(key);
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<int64_t>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<uint64_t>());
    }
    if (value.is_number_float()) {
        std::ostringstream oss;
        oss << value.get<double>();
        return oss.str();
    }
    return "";
}

bool bboxImgAttrsDeclareSourcePixels(const std::optional<json>& attrs,
                                     int target_width,
                                     int target_height) {
    if (!attrs.has_value() || !attrs->is_object()) {
        return false;
    }
    const std::string space =
        jsonStringAttr(*attrs, "bbox_img_xyxy_coordinate_space");
    if (space != "source_image_xyxy" && space != "full_image_xyxy") {
        return false;
    }
    const auto ref_width =
        jsonPositiveIntAttr(*attrs, "bbox_img_xyxy_reference_width");
    const auto ref_height =
        jsonPositiveIntAttr(*attrs, "bbox_img_xyxy_reference_height");
    if (ref_width.has_value() && target_width > 0 &&
        *ref_width != target_width) {
        return false;
    }
    if (ref_height.has_value() && target_height > 0 &&
        *ref_height != target_height) {
        return false;
    }
    return true;
}

bool summaryDeclaresOnlyRawDetect(const std::optional<json>& attrs,
                                  size_t expected_rows) {
    if (!attrs.has_value() || !attrs->contains("summary_statistics") ||
        !(*attrs)["summary_statistics"].is_object()) {
        return false;
    }
    const auto& summary = (*attrs)["summary_statistics"];
    auto count_attr = [&](const char* key) -> std::optional<size_t> {
        const auto value = jsonNumberAttr(summary, key);
        if (!value.has_value() || !std::isfinite(*value) || *value < 0.0) {
            return std::nullopt;
        }
        return static_cast<size_t>(std::llround(*value));
    };
    const auto total_rows = count_attr("total_rows");
    const auto rows_raw_detect = count_attr("rows_raw_detect");
    if (!total_rows.has_value() || !rows_raw_detect.has_value() ||
        *total_rows != expected_rows || *rows_raw_detect != expected_rows) {
        return false;
    }
    const auto rows_none = count_attr("rows_none").value_or(0);
    const auto rows_interpolated = count_attr("rows_interpolated").value_or(0);
    const auto rows_manual = count_attr("rows_manual").value_or(0);
    return rows_none == 0 && rows_interpolated == 0 && rows_manual == 0;
}

bool readInt32Array(const ts::kvstore::KvStore& store,
                    const ts::Context& context,
                    const std::string& path,
                    std::vector<int32_t>& out) {
    auto attempt = [&](auto type_token) -> bool {
        using Source = decltype(type_token);
        auto open_result = openArrayAny<Source, 1>(store, path, context);
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
        const size_t length = static_cast<size_t>(array.shape()[0]);
        out.resize(length);
        bool overflow = false;
        for (size_t i = 0; i < length; ++i) {
            out[i] = convertToInt32WithClamp<Source>(
                array(static_cast<ts::Index>(i)), overflow);
        }
        return true;
    };

    try {
        return attempt(int32_t{}) ||
               attempt(uint32_t{}) ||
               attempt(int64_t{}) ||
               attempt(uint64_t{}) ||
               attempt(int16_t{}) ||
               attempt(uint16_t{}) ||
               attempt(int8_t{}) ||
               attempt(uint8_t{});
    } catch (const std::exception& e) {
        std::cerr << "Error reading int32 array at " << path << ": "
                  << e.what() << std::endl;
        return false;
    }
}

bool readFloatArray(const ts::kvstore::KvStore& store,
                    const ts::Context& context,
                    const std::string& path,
                    std::vector<float>& out) {
    auto attempt = [&](auto type_token) -> bool {
        using Source = decltype(type_token);
        auto open_result = openArrayAny<Source, 1>(store, path, context);
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
        const size_t length = static_cast<size_t>(array.shape()[0]);
        out.resize(length);
        for (size_t i = 0; i < length; ++i) {
            out[i] =
                static_cast<float>(array(static_cast<ts::Index>(i)));
        }
        return true;
    };

    try {
        return attempt(float{}) || attempt(double{});
    } catch (const std::exception& e) {
        std::cerr << "Error reading float array at " << path << ": "
                  << e.what() << std::endl;
        return false;
    }
}

bool readFloatMatrix(const ts::kvstore::KvStore& store,
                     const ts::Context& context,
                     const std::string& path,
                     std::vector<std::array<float, 4>>& out) {
    auto attempt = [&](auto type_token) -> bool {
        using Source = decltype(type_token);
        auto open_result = openArrayAny<Source, 2>(store, path, context);
        if (!open_result.ok()) {
            return false;
        }
        auto array_result = ts::Read(open_result.value()).result();
        if (!array_result.ok()) {
            return false;
        }
        auto array = array_result.value();
        if (array.rank() != 2 || array.shape()[1] != 4) {
            return false;
        }
        const size_t rows = static_cast<size_t>(array.shape()[0]);
        out.resize(rows);
        for (size_t row = 0; row < rows; ++row) {
            for (ts::Index col = 0; col < 4; ++col) {
                out[row][static_cast<size_t>(col)] = static_cast<float>(
                    array(static_cast<ts::Index>(row), col));
            }
        }
        return true;
    };

    try {
        return attempt(float{}) || attempt(double{});
    } catch (const std::exception& e) {
        std::cerr << "Error reading float matrix at " << path << ": "
                  << e.what() << std::endl;
        return false;
    }
}

}  // namespace

bool loadClippedRefinedCollectionDetections(
    const ts::kvstore::KvStore& store,
    const ts::Context& context,
    const PaletteClippedResolver& resolver,
    int current_image_width,
    int current_image_height,
    ClippedDetectionLoadResult& result) {
    result = ClippedDetectionLoadResult();
    if (!resolver.loaded() || resolver.selectedRunCount() == 0) {
        return false;
    }

    const size_t total_parent_frames = resolver.totalParentFrames();
    if (total_parent_frames == 0) {
        return false;
    }

    result.image_width = current_image_width;
    result.image_height = current_image_height;
    result.selected_runs = resolver.selectedRunCount();

    InterpolationRunData stage;
    stage.run_name = resolver.collectionId();
    stage.stage_label = "clipped_instances";
    stage.method = "finalized_clipped_refined_detect_collection";
    stage.uses_palette_layout = true;
    stage.has_flat_detections = true;
    stage.boxes_are_pixel_xyxy = true;

    std::vector<int32_t> parent_counts(total_parent_frames, 0);
    std::vector<int32_t> parent_frame_indices;
    std::vector<std::array<float, 4>> boxes;
    std::vector<float> scores;
    std::vector<int32_t> class_ids;
    std::vector<uint8_t> detection_source;
    std::vector<std::string> detection_reason;

    bool any_scores = false;
    bool any_class_ids = false;
    bool aggregate_boxes_are_pixel_xyxy = true;
    bool aggregate_geometry_initialized = false;
    const auto load_start = std::chrono::steady_clock::now();

    const auto& selected_runs = resolver.selectedRuns();
    for (const auto& selected : selected_runs) {
        const std::string instances_base =
            selected.refined_group_path + "/instances/";

        std::vector<int32_t> clip_frame_indices;
        auto phase_start = std::chrono::steady_clock::now();
        if (!readInt32Array(store, context, instances_base + "frame_indices",
                            clip_frame_indices)) {
            result.timing.read_frame_indices_ms += elapsedMs(phase_start);
            std::cout << "  Clipped refined instances missing frame_indices: "
                      << instances_base << std::endl;
            continue;
        }
        result.timing.read_frame_indices_ms += elapsedMs(phase_start);

        ClippedRunGeometry geometry;
        std::optional<json> refined_attrs;
        std::optional<json> instances_attrs;
        if (!selected.detect_group_path.empty()) {
            phase_start = std::chrono::steady_clock::now();
            if (auto detect_attrs =
                    readAttrsAny(store, selected.detect_group_path)) {
                geometry = clippedRunGeometryFromAttrs(*detect_attrs);
            }
            result.timing.read_attrs_ms += elapsedMs(phase_start);
        }
        phase_start = std::chrono::steady_clock::now();
        refined_attrs = readAttrsAny(store, selected.refined_group_path);
        instances_attrs = readAttrsAny(store, instances_base);
        result.timing.read_attrs_ms += elapsedMs(phase_start);
        if (geometry.source_width > 0 && geometry.source_height > 0) {
            const bool no_image_dims =
                result.image_width <= 0 || result.image_height <= 0;
            const bool image_dims_match_inference =
                result.image_width == geometry.inference_width &&
                result.image_height == geometry.inference_height;
            if (no_image_dims || image_dims_match_inference) {
                result.image_width = geometry.source_width;
                result.image_height = geometry.source_height;
            }
        }

        std::vector<std::array<float, 4>> run_boxes;
        bool run_boxes_are_pixel_xyxy = false;
        phase_start = std::chrono::steady_clock::now();
        if (readFloatMatrix(store, context, instances_base + "bbox_img_xyxy",
                            run_boxes)) {
            result.timing.read_bbox_img_ms += elapsedMs(phase_start);
            run_boxes_are_pixel_xyxy = true;
            const int bbox_target_width =
                geometry.source_width > 0 ? geometry.source_width : result.image_width;
            const int bbox_target_height =
                geometry.source_height > 0 ? geometry.source_height : result.image_height;
            const bool metadata_declares_source_pixels =
                bboxImgAttrsDeclareSourcePixels(instances_attrs,
                                                bbox_target_width,
                                                bbox_target_height) ||
                bboxImgAttrsDeclareSourcePixels(refined_attrs,
                                                bbox_target_width,
                                                bbox_target_height);
            if (!metadata_declares_source_pixels) {
                std::vector<std::array<float, 4>> run_norm_boxes;
                phase_start = std::chrono::steady_clock::now();
                readFloatMatrix(store, context,
                                instances_base + "bbox_norm_coords",
                                run_norm_boxes);
                result.timing.read_bbox_norm_ms += elapsedMs(phase_start);
                if (shouldScaleInferencePixelBoxesToSource(
                        run_boxes, run_norm_boxes, geometry.inference_width,
                        geometry.inference_height, geometry.source_width,
                        geometry.source_height)) {
                    scalePixelXyxyBoxes(run_boxes, geometry.inference_width,
                                        geometry.inference_height,
                                        geometry.source_width,
                                        geometry.source_height);
                    ++result.scaled_bbox_runs;
                    if (result.first_scaled_geometry.empty()) {
                        result.first_scaled_geometry =
                            std::to_string(geometry.inference_width) + "x" +
                            std::to_string(geometry.inference_height) + " -> " +
                            std::to_string(geometry.source_width) + "x" +
                            std::to_string(geometry.source_height);
                    }
                }
            }
        } else {
            result.timing.read_bbox_img_ms += elapsedMs(phase_start);
            phase_start = std::chrono::steady_clock::now();
            if (readFloatMatrix(store, context,
                                instances_base + "bbox_norm_coords",
                                run_boxes)) {
                run_boxes_are_pixel_xyxy = false;
                result.timing.read_bbox_norm_ms += elapsedMs(phase_start);
            } else {
                result.timing.read_bbox_norm_ms += elapsedMs(phase_start);
                std::cout << "  Clipped refined instances missing bbox arrays: "
                          << instances_base << std::endl;
                continue;
            }
        }

        if (run_boxes.size() != clip_frame_indices.size()) {
            std::cout << "  Clipped refined instances length mismatch: "
                      << instances_base << std::endl;
            continue;
        }

        const int normalized_target_width =
            geometry.source_width > 0 ? geometry.source_width : result.image_width;
        const int normalized_target_height =
            geometry.source_height > 0 ? geometry.source_height : result.image_height;
        if (!run_boxes_are_pixel_xyxy && normalized_target_width > 0 &&
            normalized_target_height > 0) {
            for (auto& box : run_boxes) {
                box = normalizedBoxToPixels(box, normalized_target_width,
                                            normalized_target_height);
            }
            run_boxes_are_pixel_xyxy = true;
        }

        if (!aggregate_geometry_initialized) {
            aggregate_boxes_are_pixel_xyxy = run_boxes_are_pixel_xyxy;
            stage.boxes_are_pixel_xyxy = run_boxes_are_pixel_xyxy;
            aggregate_geometry_initialized = true;
        } else if (run_boxes_are_pixel_xyxy != aggregate_boxes_are_pixel_xyxy) {
            std::cout << "  Clipped refined instances mix pixel and normalized "
                      << "bbox geometry; skipping run "
                      << selected.refined_group_path << std::endl;
            continue;
        }

        std::vector<float> run_scores;
        phase_start = std::chrono::steady_clock::now();
        const bool run_has_scores =
            (readFloatArray(store, context,
                            instances_base + "confidence_scores", run_scores) ||
             readFloatArray(store, context, instances_base + "scores",
                            run_scores)) &&
            run_scores.size() == run_boxes.size();
        result.timing.read_scores_ms += elapsedMs(phase_start);
        any_scores = any_scores || run_has_scores;

        std::vector<int32_t> run_class_ids;
        phase_start = std::chrono::steady_clock::now();
        const bool run_has_class_ids =
            readInt32Array(store, context, instances_base + "class_ids",
                           run_class_ids) &&
            run_class_ids.size() == run_boxes.size();
        result.timing.read_class_ids_ms += elapsedMs(phase_start);
        any_class_ids = any_class_ids || run_has_class_ids;

        std::vector<int32_t> run_source_kind_codes;
        const bool all_rows_raw_detect =
            summaryDeclaresOnlyRawDetect(refined_attrs, run_boxes.size());
        bool run_has_source_kind_codes = false;
        if (!all_rows_raw_detect) {
            phase_start = std::chrono::steady_clock::now();
            run_has_source_kind_codes =
                readInt32Array(store, context,
                               instances_base + "source_kind_codes",
                               run_source_kind_codes) &&
                run_source_kind_codes.size() == run_boxes.size();
            result.timing.read_source_kind_ms += elapsedMs(phase_start);
        }

        phase_start = std::chrono::steady_clock::now();
        for (size_t row = 0; row < run_boxes.size(); ++row) {
            const int32_t clip_frame = clip_frame_indices[row];
            if (clip_frame < 0 ||
                static_cast<size_t>(clip_frame) >=
                    selected.parent_frame_by_clip_local.size()) {
                continue;
            }
            const int64_t parent_frame =
                selected.parent_frame_by_clip_local[static_cast<size_t>(clip_frame)];
            if (parent_frame < 0 ||
                static_cast<size_t>(parent_frame) >= total_parent_frames) {
                continue;
            }
            if (!finiteBox(run_boxes[row])) {
                continue;
            }

            parent_frame_indices.push_back(static_cast<int32_t>(parent_frame));
            boxes.push_back(run_boxes[row]);
            scores.push_back(run_has_scores ? run_scores[row] : 1.0f);
            class_ids.push_back(run_has_class_ids ? run_class_ids[row] : 0);

            int32_t source_kind = 1;
            if (all_rows_raw_detect) {
                source_kind = 1;
            } else if (run_has_source_kind_codes) {
                source_kind = run_source_kind_codes[row];
            }
            detection_source.push_back(source_kind == 2 ? 1 : 0);
            detection_reason.emplace_back(sourceKindLabel(source_kind));
            parent_counts[static_cast<size_t>(parent_frame)]++;
        }
        result.timing.append_rows_ms += elapsedMs(phase_start);
        ++result.loaded_runs;
        result.total_clip_rows += run_boxes.size();
    }

    if (parent_frame_indices.empty()) {
        result.timing.total_ms = elapsedMs(load_start);
        return false;
    }

    const auto sort_start = std::chrono::steady_clock::now();
    std::vector<size_t> order(parent_frame_indices.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (parent_frame_indices[a] != parent_frame_indices[b]) {
            return parent_frame_indices[a] < parent_frame_indices[b];
        }
        return a < b;
    });

    auto reorder_int32 = [&](std::vector<int32_t>& values) {
        std::vector<int32_t> sorted(values.size());
        for (size_t i = 0; i < order.size(); ++i) {
            sorted[i] = values[order[i]];
        }
        values = std::move(sorted);
    };
    auto reorder_float = [&](std::vector<float>& values) {
        std::vector<float> sorted(values.size());
        for (size_t i = 0; i < order.size(); ++i) {
            sorted[i] = values[order[i]];
        }
        values = std::move(sorted);
    };
    auto reorder_u8 = [&](std::vector<uint8_t>& values) {
        std::vector<uint8_t> sorted(values.size());
        for (size_t i = 0; i < order.size(); ++i) {
            sorted[i] = values[order[i]];
        }
        values = std::move(sorted);
    };
    auto reorder_string = [&](std::vector<std::string>& values) {
        std::vector<std::string> sorted(values.size());
        for (size_t i = 0; i < order.size(); ++i) {
            sorted[i] = std::move(values[order[i]]);
        }
        values = std::move(sorted);
    };

    std::vector<std::array<float, 4>> sorted_boxes(boxes.size());
    for (size_t i = 0; i < order.size(); ++i) {
        sorted_boxes[i] = boxes[order[i]];
    }
    boxes = std::move(sorted_boxes);
    reorder_int32(parent_frame_indices);
    reorder_float(scores);
    reorder_int32(class_ids);
    reorder_u8(detection_source);
    reorder_string(detection_reason);
    result.timing.sort_reorder_ms = elapsedMs(sort_start);

    const auto offsets_start = std::chrono::steady_clock::now();
    stage.frame_offsets.assign(total_parent_frames + 1, 0);
    size_t running = 0;
    for (size_t frame = 0; frame < total_parent_frames; ++frame) {
        running += static_cast<size_t>(std::max(parent_counts[frame], 0));
        stage.frame_offsets[frame + 1] = running;
    }
    result.timing.offsets_ms = elapsedMs(offsets_start);

    stage.frame_indices = std::move(parent_frame_indices);
    stage.bbox_norm_coords = std::move(boxes);
    stage.flat_scores = any_scores ? std::move(scores) : std::vector<float>();
    stage.flat_class_ids =
        any_class_ids ? std::move(class_ids) : std::vector<int32_t>();
    stage.detection_source = std::move(detection_source);
    stage.detection_reason = std::move(detection_reason);
    stage.n_detections = std::move(parent_counts);
    stage.has_scores = any_scores;
    stage.has_class_ids = any_class_ids;
    stage.is_loaded = true;

    result.stage = std::move(stage);
    result.timing.total_ms = elapsedMs(load_start);
    return true;
}
