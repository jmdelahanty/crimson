#include "zarr_loader.h"
#include <iostream>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <stdexcept>
#include <optional>
#include <tensorstore/kvstore/operations.h>
#include <tensorstore/cast.h>
#include <tensorstore/driver/zarr/dtype.h>

using json = nlohmann::json;

namespace {
bool kvEntryExists(const ts::kvstore::KvStore& store, const std::string& key) {
    auto result = ts::kvstore::Read(store, key).result();
    return result.ok();
}

bool arrayExists(const ts::kvstore::KvStore& store, const std::string& array_path) {
    return kvEntryExists(store, array_path + ".zarray");
}

std::optional<json> readJsonAttrs(const ts::kvstore::KvStore& store,
                                  const std::string& path) {
    auto result = ts::kvstore::Read(store, path).result();
    if (!result.ok()) {
        return std::nullopt;
    }
    const auto& read_result = result.value();
    if (!read_result.has_value()) {
        return std::nullopt;
    }
    std::string payload;
    read_result.value.AppendToString(&payload);
    if (payload.empty()) {
        return json::object();
    }
    try {
        return json::parse(payload);
    } catch (const json::parse_error& e) {
        std::cerr << "Failed to parse JSON at " << path << ": " << e.what() << std::endl;
        return std::nullopt;
    }
}

std::string extractLatestRunName(const json& attrs) {
    if (attrs.contains("latest") && attrs["latest"].is_string()) {
        return attrs["latest"].get<std::string>();
    }
    if (attrs.contains("latest_completed") && attrs["latest_completed"].is_string()) {
        return attrs["latest_completed"].get<std::string>();
    }
    if (attrs.contains("latest_success") && attrs["latest_success"].is_string()) {
        return attrs["latest_success"].get<std::string>();
    }
    return "";
}

void populateDetectionMetadata(const json& attrs, ZarrDetectionData& data) {
    if (attrs.contains("method") && attrs["method"].is_string()) {
        data.detect_run_method = attrs["method"].get<std::string>();
    }
    if (attrs.contains("created_at") && attrs["created_at"].is_string()) {
        data.detect_run_created_at = attrs["created_at"].get<std::string>();
    } else if (attrs.contains("created_at_utc") && attrs["created_at_utc"].is_string()) {
        data.detect_run_created_at = attrs["created_at_utc"].get<std::string>();
    } else if (attrs.contains("completed_at_utc") && attrs["completed_at_utc"].is_string()) {
        data.detect_run_created_at = attrs["completed_at_utc"].get<std::string>();
    }
    if (attrs.contains("command") && attrs["command"].is_string()) {
        data.detect_run_command = attrs["command"].get<std::string>();
    }
    if (attrs.contains("source_detection_run") && attrs["source_detection_run"].is_string()) {
        data.detect_run_source = attrs["source_detection_run"].get<std::string>();
    }
    if (attrs.contains("inputs") && attrs["inputs"].is_object()) {
        const auto& inputs = attrs["inputs"];
        if (inputs.contains("source_detection_run") && inputs["source_detection_run"].is_string()) {
            data.detect_run_source = inputs["source_detection_run"].get<std::string>();
        } else if (inputs.contains("detection_run") && inputs["detection_run"].is_string()) {
            data.detect_run_source = inputs["detection_run"].get<std::string>();
        }
    }
    if (attrs.contains("provenance")) {
        try {
            data.detect_run_provenance_json = attrs["provenance"].dump();
        } catch (...) {
            // Ignore serialization issues, leave empty.
        }
    }
}

void populateInterpolationMetadata(const json& attrs,
                                   InterpolationRunData& interp) {
    if (attrs.contains("method") && attrs["method"].is_string()) {
        interp.method = attrs["method"].get<std::string>();
    }
    if (attrs.contains("created_at") && attrs["created_at"].is_string()) {
        interp.created_at = attrs["created_at"].get<std::string>();
    } else if (attrs.contains("completed_at_utc") && attrs["completed_at_utc"].is_string()) {
        interp.created_at = attrs["completed_at_utc"].get<std::string>();
    }
    if (attrs.contains("source_detection_run") && attrs["source_detection_run"].is_string()) {
        interp.source_detection_run = attrs["source_detection_run"].get<std::string>();
    }
    if (attrs.contains("inputs") && attrs["inputs"].is_object()) {
        const auto& inputs = attrs["inputs"];
        if (inputs.contains("source_detection_run") && inputs["source_detection_run"].is_string()) {
            interp.source_detection_run = inputs["source_detection_run"].get<std::string>();
        } else if (inputs.contains("detection_run") && inputs["detection_run"].is_string()) {
            interp.source_detection_run = inputs["detection_run"].get<std::string>();
        }
    }
    if (attrs.contains("provenance")) {
        try {
            interp.provenance_json = attrs["provenance"].dump();
        } catch (...) {
            // Ignore serialization issues.
        }
    }
}

std::array<float, 4> normalizedBoxToPixels(
    const std::array<float, 4>& norm_box,
    int image_width,
    int image_height
) {
    if (image_width <= 0 || image_height <= 0) {
        return norm_box;
    }

    float cx = norm_box[0] * static_cast<float>(image_width);
    float cy = norm_box[1] * static_cast<float>(image_height);
    float w = norm_box[2] * static_cast<float>(image_width);
    float h = norm_box[3] * static_cast<float>(image_height);

    float x_min = cx - (w * 0.5f);
    float y_min = cy - (h * 0.5f);
    float x_max = cx + (w * 0.5f);
    float y_max = cy + (h * 0.5f);

    x_min = std::clamp(x_min, 0.0f, static_cast<float>(image_width));
    y_min = std::clamp(y_min, 0.0f, static_cast<float>(image_height));
    x_max = std::clamp(x_max, 0.0f, static_cast<float>(image_width));
    y_max = std::clamp(y_max, 0.0f, static_cast<float>(image_height));

    if (x_max < x_min) {
        std::swap(x_max, x_min);
    }
    if (y_max < y_min) {
        std::swap(y_max, y_min);
    }

    return {x_min, y_min, x_max, y_max};
}
}  // namespace

ZarrDetectionLoader::ZarrDetectionLoader() {
    // Initialize TensorStore context with default settings
    context_ = ts::Context::Default();
}

ZarrDetectionLoader::~ZarrDetectionLoader() {
    // TensorStore handles cleanup automatically
}

bool ZarrDetectionLoader::loadZarrFile(const std::string& filepath,
                                       std::string& error_message) {
    try {
        // Clear any previous data
        data_ = ZarrDetectionData();
        
        std::cout << "Opening Zarr store: " << filepath << std::endl;

        // Validate filepath exists
        if (!std::filesystem::exists(filepath)) {
            error_message = "Zarr file/directory does not exist: " + filepath;
            return false;
        }

        // Open the kvstore
        auto spec_result = ts::kvstore::Spec::FromJson({
            {"driver", "file"},
            {"path", filepath}
        });
        
        if (!spec_result.ok()) {
            error_message = "Failed to create kvstore spec: " + spec_result.status().ToString();
            return false;
        }

        auto store_future = ts::kvstore::Open(spec_result.value(), context_);
        auto store_result = store_future.result();
        
        if (!store_result.ok()) {
            error_message = "Failed to open kvstore: " + store_result.status().ToString();
            return false;
        }
        auto store = store_result.value();

        // Populate metadata first; continue even if it fails (legacy files may not have it)
        if (!loadMetadata(store)) {
            std::cout << "  Warning: Could not read root metadata (.zattrs)" << std::endl;
        }

        bool loaded_layout = false;

        try {
            loaded_layout = loadDetectionRuns(store);
        } catch (const std::exception& palette_error) {
            error_message = std::string("Failed to load Palette detection_runs layout: ") +
                            palette_error.what();
            return false;
        }

        if (!loaded_layout) {
            if (!loadStandardFormat(store)) {
                error_message = "Failed to load supported zarr layouts (palette or legacy)";
                return false;
            }
        }
        
        // Validate we have essential data
        if (data_.total_frames == 0) {
            error_message = "Zarr file has 0 frames";
            return false;
        }
        
        // Try to load interpolation runs from the correct, known location
        if (loadInterpolationRuns(store)) {
            std::cout << "  Successfully loaded interpolation data" << std::endl;
            
            // Validate interpolation data matches main data dimensions
            if (data_.has_interpolation) {
                auto interp_domain = data_.latest_interpolation.bboxes_store.domain();
                if (interp_domain.shape()[0] != data_.total_frames) {
                    std::cerr << "  Warning: Interpolation frame count mismatch. "
                              << "Expected " << data_.total_frames 
                              << " but got " << interp_domain.shape()[0] << std::endl;
                    data_.has_interpolation = false;
                }
            }
        } else {
            std::cout << "  No interpolation data available in 'interpolation_runs'" << std::endl;
        }
        
        std::cout << "Successfully loaded zarr file: " << filepath << std::endl;
        std::cout << "  Total frames: " << data_.total_frames << std::endl;
        std::cout << "  Max detections per frame: " << data_.max_detections << std::endl;
        std::cout << "  FPS: " << data_.fps << std::endl;
        std::cout << "  Layout: "
                  << (data_.layout == ZarrLayoutType::kPaletteRuns
                          ? "Palette detection_runs"
                          : "Legacy dense arrays")
                  << std::endl;
        if (!data_.detect_run_name.empty()) {
            std::cout << "  Detect run: " << data_.detect_run_name << std::endl;
        }
        if (data_.image_width > 0 && data_.image_height > 0) {
            std::cout << "  Resolution: " << data_.image_width << "x"
                      << data_.image_height << std::endl;
        }
        std::cout << "  Has scores: " << (data_.has_scores ? "Yes" : "No") << std::endl;
        std::cout << "  Has class IDs: " << (data_.has_class_ids ? "Yes" : "No") << std::endl;
        std::cout << "  Has interpolation: " << (data_.has_interpolation ? "Yes" : "No") << std::endl;
        
        if (data_.has_interpolation) {
            std::cout << "  Interpolation method: " << data_.latest_interpolation.method << std::endl;
            std::cout << "  Interpolation created: " << data_.latest_interpolation.created_at << std::endl;
        }
        
        return true;
        
    } catch (const std::exception& e) {
        error_message = std::string("Exception loading zarr: ") + e.what();
        // Clear any partially loaded data
        data_ = ZarrDetectionData();
        return false;
    }
}

bool ZarrDetectionLoader::readInt32Array(const ts::kvstore::KvStore& store,
                                        const std::string& path,
                                        std::vector<int32_t>& out) {
    try {
        json spec;
        spec["driver"] = "zarr";
        auto store_spec = store.spec();
        if (!store_spec.ok()) {
            return false;
        }
        spec["kvstore"] = store_spec.value().ToJson().value();
        spec["path"] = path;

        auto open_result = ts::Open<int32_t, 1>(
            spec,
            ts::OpenMode::open,
            ts::ReadWriteMode::read,
            context_
        ).result();

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

        size_t length = static_cast<size_t>(array.shape()[0]);
        out.resize(length);
        const auto* data = static_cast<const int32_t*>(array.data());
        std::copy(data, data + length, out.begin());
        return true;

    } catch (const std::exception& e) {
        std::cerr << "Error reading int32 array at " << path << ": " << e.what() << std::endl;
        return false;
    }
}

bool ZarrDetectionLoader::readInt64Array(const ts::kvstore::KvStore& store,
                                        const std::string& path,
                                        std::vector<int64_t>& out) {
    try {
        json spec;
        spec["driver"] = "zarr";
        auto store_spec = store.spec();
        if (!store_spec.ok()) {
            return false;
        }
        spec["kvstore"] = store_spec.value().ToJson().value();
        spec["path"] = path;

        auto open_result = ts::Open<int64_t, 1>(
            spec,
            ts::OpenMode::open,
            ts::ReadWriteMode::read,
            context_
        ).result();

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

        size_t length = static_cast<size_t>(array.shape()[0]);
        out.resize(length);
        const auto* data = static_cast<const int64_t*>(array.data());
        std::copy(data, data + length, out.begin());
        return true;

    } catch (const std::exception& e) {
        std::cerr << "Error reading int64 array at " << path << ": " << e.what() << std::endl;
        return false;
    }
}

bool ZarrDetectionLoader::readFloatArray(const ts::kvstore::KvStore& store,
                                        const std::string& path,
                                        std::vector<float>& out) {
    try {
        json spec;
        spec["driver"] = "zarr";
        auto store_spec = store.spec();
        if (!store_spec.ok()) {
            return false;
        }
        spec["kvstore"] = store_spec.value().ToJson().value();
        spec["path"] = path;

        auto open_result = ts::Open<float, 1>(
            spec,
            ts::OpenMode::open,
            ts::ReadWriteMode::read,
            context_
        ).result();

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

        size_t length = static_cast<size_t>(array.shape()[0]);
        out.resize(length);
        const auto* data = static_cast<const float*>(array.data());
        std::copy(data, data + length, out.begin());
        return true;

    } catch (const std::exception& e) {
        std::cerr << "Error reading float array at " << path << ": " << e.what() << std::endl;
        return false;
    }
}

bool ZarrDetectionLoader::readFloatMatrix(const ts::kvstore::KvStore& store,
                                         const std::string& path,
                                         std::vector<std::array<float, 4>>& out) {
    try {
        json spec;
        spec["driver"] = "zarr";
        auto store_spec = store.spec();
        if (!store_spec.ok()) {
            return false;
        }
        spec["kvstore"] = store_spec.value().ToJson().value();
        spec["path"] = path;

        auto open_result = ts::Open<float, 2>(
            spec,
            ts::OpenMode::open,
            ts::ReadWriteMode::read,
            context_
        ).result();

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

        size_t rows = static_cast<size_t>(array.shape()[0]);
        out.resize(rows);
        const auto* data = static_cast<const float*>(array.data());
        for (size_t row = 0; row < rows; ++row) {
            out[row][0] = data[row * 4 + 0];
            out[row][1] = data[row * 4 + 1];
            out[row][2] = data[row * 4 + 2];
            out[row][3] = data[row * 4 + 3];
        }
        return true;

    } catch (const std::exception& e) {
        std::cerr << "Error reading float matrix at " << path << ": " << e.what() << std::endl;
        return false;
    }
}

bool ZarrDetectionLoader::readBoolArray(const ts::kvstore::KvStore& store,
                                       const std::string& path,
                                       std::vector<uint8_t>& out) {
    try {
        json spec;
        spec["driver"] = "zarr";
        auto store_spec = store.spec();
        if (!store_spec.ok()) {
            return false;
        }
        spec["kvstore"] = store_spec.value().ToJson().value();
        spec["path"] = path;

        auto open_result = ts::Open<bool, 1>(
            spec,
            ts::OpenMode::open,
            ts::ReadWriteMode::read,
            context_
        ).result();

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

        size_t length = static_cast<size_t>(array.shape()[0]);
        out.resize(length);
        const auto* data = static_cast<const bool*>(array.data());
        for (size_t i = 0; i < length; ++i) {
            out[i] = data[i] ? 1 : 0;
        }
        return true;

    } catch (const std::exception& e) {
        std::cerr << "Error reading bool array at " << path << ": " << e.what() << std::endl;
        return false;
    }
}

bool ZarrDetectionLoader::loadPaletteInterpolationRun(const ts::kvstore::KvStore& store,
                                                      const std::string& run_name,
                                                      const std::string& subgroup) {
    try {
        data_.latest_interpolation = InterpolationRunData();
        auto& interp = data_.latest_interpolation;
        interp.run_name = run_name;
        interp.method = subgroup;
        interp.uses_palette_layout = true;
        interp.has_flat_detections = true;

        // Try to read run-level metadata
        auto run_attrs_result = ts::kvstore::Read(store, "refined_runs/" + run_name + "/.zattrs").result();
        if (run_attrs_result.ok() && run_attrs_result.value().has_value()) {
            auto run_attrs = json::parse(run_attrs_result.value().value.Flatten());
            if (run_attrs.contains("created_at") && run_attrs["created_at"].is_string()) {
                interp.created_at = run_attrs["created_at"].get<std::string>();
            }
            if (run_attrs.contains("completed_at_utc") && run_attrs["completed_at_utc"].is_string()) {
                interp.created_at = run_attrs["completed_at_utc"].get<std::string>();
            }
            if (run_attrs.contains("method") && run_attrs["method"].is_string()) {
                interp.method = run_attrs["method"].get<std::string>();
            }
        }

        std::string base_path = "refined_runs/" + run_name + "/" + subgroup + "/";

        auto subgroup_attrs_result = ts::kvstore::Read(store, base_path + ".zattrs").result();
        if (subgroup_attrs_result.ok() && subgroup_attrs_result.value().has_value()) {
            auto subgroup_attrs = json::parse(subgroup_attrs_result.value().value.Flatten());
            if (subgroup_attrs.contains("created_at") && subgroup_attrs["created_at"].is_string()) {
                interp.created_at = subgroup_attrs["created_at"].get<std::string>();
            }
            if (subgroup_attrs.contains("method") && subgroup_attrs["method"].is_string()) {
                interp.method = subgroup_attrs["method"].get<std::string>();
            }
        }

        if (!readInt32Array(store, base_path + "frame_indices", interp.frame_indices)) {
            std::cerr << "  Failed to read refined frame_indices from '" << subgroup << "'" << std::endl;
            return false;
        }

        if (!readFloatMatrix(store, base_path + "bbox_norm_coords", interp.bbox_norm_coords)) {
            std::cerr << "  Failed to read refined bbox_norm_coords from '" << subgroup << "'" << std::endl;
            return false;
        }

        if (interp.frame_indices.size() != interp.bbox_norm_coords.size()) {
            std::cerr << "  Refined run mismatch between frame_indices (" << interp.frame_indices.size()
                      << ") and bbox_norm_coords (" << interp.bbox_norm_coords.size() << ")" << std::endl;
            return false;
        }

        std::vector<float> scores;
        if (readFloatArray(store, base_path + "scores", scores)) {
            if (scores.size() == interp.frame_indices.size()) {
                interp.flat_scores = std::move(scores);
            } else {
                std::cerr << "  Warning: refined scores length mismatch; ignoring scores" << std::endl;
            }
        }

        std::vector<int32_t> class_ids;
        if (readInt32Array(store, base_path + "class_ids", class_ids)) {
            if (class_ids.size() == interp.frame_indices.size()) {
                interp.flat_class_ids = std::move(class_ids);
            } else {
                std::cerr << "  Warning: refined class_ids length mismatch; ignoring class IDs" << std::endl;
            }
        }

        std::vector<int32_t> detection_source_raw;
        if (readInt32Array(store, base_path + "detection_source", detection_source_raw)) {
            if (detection_source_raw.size() == interp.frame_indices.size()) {
                interp.detection_source.resize(detection_source_raw.size());
                for (size_t i = 0; i < detection_source_raw.size(); ++i) {
                    interp.detection_source[i] = detection_source_raw[i] != 0 ? 1 : 0;
                }
            } else {
                std::cerr << "  Warning: detection_source length mismatch; ignoring" << std::endl;
            }
        }

        std::vector<uint8_t> mask_raw;
        readBoolArray(store, base_path + "interpolation_mask", mask_raw);

        std::vector<int32_t> n_detections_disk;
        readInt32Array(store, base_path + "n_detections", n_detections_disk);

        int32_t max_frame_index = -1;
        for (const auto frame_index : interp.frame_indices) {
            if (frame_index > max_frame_index) {
                max_frame_index = frame_index;
            }
        }

        size_t resolved_frames = data_.total_frames;
        if (!n_detections_disk.empty()) {
            resolved_frames = std::max(resolved_frames, static_cast<size_t>(n_detections_disk.size()));
        }
        if (max_frame_index >= 0) {
            resolved_frames = std::max(resolved_frames, static_cast<size_t>(max_frame_index) + 1);
        }

        if (resolved_frames == 0) {
            std::cerr << "  Could not determine frame count for refined detections" << std::endl;
            return false;
        }

        interp.frame_offsets.assign(resolved_frames + 1, 0);
        for (const auto frame_index : interp.frame_indices) {
            if (frame_index < 0) {
                continue;
            }
            size_t frame = static_cast<size_t>(frame_index);
            if (frame + 1 >= interp.frame_offsets.size()) {
                interp.frame_offsets.resize(frame + 2, 0);
            }
            interp.frame_offsets[frame + 1]++;
        }
        for (size_t i = 0; i + 1 < interp.frame_offsets.size(); ++i) {
            interp.frame_offsets[i + 1] += interp.frame_offsets[i];
        }

        size_t total_detections = interp.frame_indices.size();
        std::vector<std::array<float, 4>> sorted_boxes(total_detections);
        std::vector<float> sorted_scores(!interp.flat_scores.empty() ? total_detections : 0);
        std::vector<int32_t> sorted_class_ids(!interp.flat_class_ids.empty() ? total_detections : 0);
        std::vector<uint8_t> sorted_detection_source(!interp.detection_source.empty() ? total_detections : 0);
        std::vector<uint8_t> sorted_mask_detection(
            (!mask_raw.empty() && mask_raw.size() == total_detections) ? total_detections : 0);
        std::vector<int32_t> sorted_frame_indices(total_detections, 0);

        std::vector<size_t> write_cursor(interp.frame_offsets.size() > 0
            ? interp.frame_offsets.size() - 1
            : 0, 0);
        for (size_t frame = 0; frame < write_cursor.size(); ++frame) {
            write_cursor[frame] = interp.frame_offsets[frame];
        }

        for (size_t det = 0; det < total_detections; ++det) {
            int32_t frame_index = interp.frame_indices[det];
            if (frame_index < 0) {
                continue;
            }
            size_t frame = static_cast<size_t>(frame_index);
            if (frame >= write_cursor.size()) {
                continue;
            }
            size_t dest = write_cursor[frame]++;
            if (dest >= sorted_boxes.size()) {
                continue;
            }
            sorted_boxes[dest] = interp.bbox_norm_coords[det];
            sorted_frame_indices[dest] = frame_index;

            if (!interp.flat_scores.empty()) {
                sorted_scores[dest] = interp.flat_scores[det];
            }
            if (!interp.flat_class_ids.empty()) {
                sorted_class_ids[dest] = interp.flat_class_ids[det];
            }
            if (!interp.detection_source.empty()) {
                sorted_detection_source[dest] = interp.detection_source[det];
            }
            if (!sorted_mask_detection.empty()) {
                sorted_mask_detection[dest] = mask_raw[det] ? 1 : 0;
            }
        }

        interp.bbox_norm_coords = std::move(sorted_boxes);
        interp.frame_indices = std::move(sorted_frame_indices);
        if (!interp.flat_scores.empty()) {
            interp.flat_scores = std::move(sorted_scores);
        }
        if (!interp.flat_class_ids.empty()) {
            interp.flat_class_ids = std::move(sorted_class_ids);
        }
        if (!interp.detection_source.empty()) {
            interp.detection_source = std::move(sorted_detection_source);
        }

        interp.frame_mask.assign(
            interp.frame_offsets.size() > 0 ? interp.frame_offsets.size() - 1 : 0,
            0
        );

        if (!interp.detection_source.empty()) {
            for (size_t frame = 0; frame + 1 < interp.frame_offsets.size(); ++frame) {
                size_t start = interp.frame_offsets[frame];
                size_t end = interp.frame_offsets[frame + 1];
                for (size_t idx = start; idx < end && idx < interp.detection_source.size(); ++idx) {
                    if (interp.detection_source[idx] != 0) {
                        interp.frame_mask[frame] = 1;
                        break;
                    }
                }
            }
        } else if (!sorted_mask_detection.empty()) {
            for (size_t frame = 0; frame + 1 < interp.frame_offsets.size(); ++frame) {
                size_t start = interp.frame_offsets[frame];
                size_t end = interp.frame_offsets[frame + 1];
                for (size_t idx = start; idx < end && idx < sorted_mask_detection.size(); ++idx) {
                    if (sorted_mask_detection[idx] != 0) {
                        interp.frame_mask[frame] = 1;
                        break;
                    }
                }
            }
        } else if (!mask_raw.empty() && mask_raw.size() == interp.frame_mask.size()) {
            interp.frame_mask.assign(
                mask_raw.begin(),
                mask_raw.begin() + interp.frame_mask.size()
            );
        }

        interp.is_loaded = true;
        data_.has_interpolation = true;

        std::cout << "  Loaded refined run '" << run_name
                  << "' subgroup '" << subgroup << "' with "
                  << total_detections << " detections" << std::endl;

        return true;

    } catch (const std::exception& e) {
        std::cerr << "  Error loading refined interpolation run: " << e.what() << std::endl;
        return false;
    }
}

bool ZarrDetectionLoader::loadDetectionRuns(const ts::kvstore::KvStore& store) {
    static const std::vector<std::string> kGroups = {
        "detection_runs",
        "detect_runs"
    };

    for (const auto& group : kGroups) {
        if (loadDetectionRunFromGroup(store, group)) {
            data_.layout = ZarrLayoutType::kPaletteRuns;
            data_.coordinates_normalized = true;
            return true;
        }
    }
    return false;
}

bool ZarrDetectionLoader::loadDetectionRunFromGroup(
    const ts::kvstore::KvStore& store,
    const std::string& group_path) {

    auto group_attrs = readJsonAttrs(store, group_path + "/.zattrs");
    if (!group_attrs.has_value()) {
        return false;
    }

    std::string latest_run = extractLatestRunName(*group_attrs);
    if (latest_run.empty()) {
        return false;
    }

    std::string base_path = group_path + "/" + latest_run + "/";
    std::cout << "  Loading detection run '" << latest_run
              << "' from " << group_path << std::endl;

    size_t resolved_frames = data_.total_frames;
    std::vector<int32_t> frame_indices;
    std::vector<std::array<float, 4>> boxes;
    std::vector<float> scores;
    std::vector<int32_t> class_ids;
    std::vector<int32_t> n_detections;
    std::vector<size_t> frame_offsets;
    std::vector<uint8_t> detection_source;
    bool has_scores = false;
    bool has_class_ids = false;

    if (!loadFlattenedRun(store,
                          base_path,
                          &frame_indices,
                          boxes,
                          scores,
                          class_ids,
                          n_detections,
                          frame_offsets,
                          nullptr,
                          has_scores,
                          has_class_ids,
                          resolved_frames)) {
        return false;
    }

    data_.frame_indices = std::move(frame_indices);
    data_.bbox_norm_coords = std::move(boxes);
    data_.flat_scores = std::move(scores);
    data_.flat_class_ids = std::move(class_ids);
    data_.frame_offsets = std::move(frame_offsets);
    data_.n_detections = std::move(n_detections);
    data_.has_scores = has_scores;
    data_.has_class_ids = has_class_ids;
    data_.total_frames = resolved_frames;
    data_.max_detections = 0;
    for (const auto count : data_.n_detections) {
        if (count >= 0) {
            data_.max_detections = std::max(
                data_.max_detections,
                static_cast<size_t>(count)
            );
        }
    }

    data_.detect_run_name = latest_run;

    if (auto run_attrs = readJsonAttrs(store, base_path + ".zattrs")) {
        populateDetectionMetadata(*run_attrs, data_);
    }

    std::cout << "  Detection run '" << data_.detect_run_name << "' loaded with "
              << data_.bbox_norm_coords.size() << " detections over "
              << data_.total_frames << " frames" << std::endl;
    return true;
}

bool ZarrDetectionLoader::loadFlattenedRun(
    const ts::kvstore::KvStore& store,
    const std::string& base_path,
    std::vector<int32_t>* frame_indices_out,
    std::vector<std::array<float, 4>>& boxes_out,
    std::vector<float>& scores_out,
    std::vector<int32_t>& class_ids_out,
    std::vector<int32_t>& n_detections_out,
    std::vector<size_t>& frame_offsets_out,
    std::vector<uint8_t>* detection_source_out,
    bool& has_scores_out,
    bool& has_class_ids_out,
    size_t& resolved_frames_out) {

    has_scores_out = false;
    has_class_ids_out = false;

    std::vector<int32_t> frame_indices_local;
    bool has_frame_indices = arrayExists(store, base_path + "frame_indices");
    if (has_frame_indices) {
        if (!readInt32Array(store, base_path + "frame_indices", frame_indices_local)) {
            return false;
        }
    }

    std::vector<std::array<float, 4>> boxes_local;
    if (arrayExists(store, base_path + "bbox_norm_coords")) {
        if (!readFloatMatrix(store, base_path + "bbox_norm_coords", boxes_local)) {
            return false;
        }
    } else if (arrayExists(store, base_path + "bboxes")) {
        if (!readFloatMatrix(store, base_path + "bboxes", boxes_local)) {
            return false;
        }
    } else {
        std::cerr << "  No bounding box array found under " << base_path << std::endl;
        return false;
    }

    size_t total_detections = boxes_local.size();
    if (total_detections == 0) {
        std::cerr << "  No detections available under " << base_path << std::endl;
    }

    std::vector<int32_t> n_detections_disk;
    if (arrayExists(store, base_path + "n_detections")) {
        if (readInt32Array(store, base_path + "n_detections", n_detections_disk)) {
            // success
        }
    }

    if (!has_frame_indices && n_detections_disk.empty()) {
        std::cerr << "  Missing both frame_indices and n_detections for " << base_path << std::endl;
        return false;
    }

    if (has_frame_indices && frame_indices_local.size() != total_detections) {
        std::cerr << "  frame_indices length mismatch at " << base_path << std::endl;
        return false;
    }

    // Optional scores
    scores_out.clear();
    if (arrayExists(store, base_path + "scores")) {
        if (readFloatArray(store, base_path + "scores", scores_out) &&
            scores_out.size() == total_detections) {
            has_scores_out = true;
        } else {
            std::cerr << "  Warning: scores mismatch under " << base_path << ", ignoring" << std::endl;
            scores_out.clear();
            has_scores_out = false;
        }
    }

    // Optional class IDs
    class_ids_out.clear();
    if (arrayExists(store, base_path + "class_ids")) {
        if (readInt32Array(store, base_path + "class_ids", class_ids_out) &&
            class_ids_out.size() == total_detections) {
            has_class_ids_out = true;
        } else {
            std::cerr << "  Warning: class_ids mismatch under " << base_path << ", ignoring" << std::endl;
            class_ids_out.clear();
            has_class_ids_out = false;
        }
    }

    // Optional detection_source
    if (detection_source_out) {
        std::vector<int32_t> detection_source_raw;
        if (readInt32Array(store, base_path + "detection_source", detection_source_raw) &&
            detection_source_raw.size() == total_detections) {
            detection_source_out->resize(detection_source_raw.size());
            for (size_t i = 0; i < detection_source_raw.size(); ++i) {
                (*detection_source_out)[i] = detection_source_raw[i] != 0 ? 1 : 0;
            }
        } else {
            detection_source_out->clear();
        }
    }

    // Determine frame counts and offsets
    std::vector<size_t> frame_offsets_local;
    if (has_frame_indices) {
        int32_t max_frame_index = -1;
        for (const auto idx : frame_indices_local) {
            if (idx > max_frame_index) {
                max_frame_index = idx;
            }
        }

        resolved_frames_out = std::max(
            resolved_frames_out,
            max_frame_index >= 0 ? static_cast<size_t>(max_frame_index + 1) : size_t(0)
        );

        if (!n_detections_disk.empty()) {
            resolved_frames_out = std::max(
                resolved_frames_out,
                static_cast<size_t>(n_detections_disk.size())
            );
        }

        if (resolved_frames_out == 0) {
            std::cerr << "  Unable to resolve frame count for " << base_path << std::endl;
            return false;
        }

        frame_offsets_local.assign(resolved_frames_out + 1, 0);
        for (const auto frame_index : frame_indices_local) {
            if (frame_index < 0) {
                continue;
            }
            size_t frame = static_cast<size_t>(frame_index);
            if (frame + 1 >= frame_offsets_local.size()) {
                frame_offsets_local.resize(frame + 2, 0);
            }
            frame_offsets_local[frame + 1]++;
        }
        for (size_t i = 0; i + 1 < frame_offsets_local.size(); ++i) {
            frame_offsets_local[i + 1] += frame_offsets_local[i];
        }

        std::vector<std::array<float, 4>> sorted_boxes(total_detections);
        std::vector<float> sorted_scores(has_scores_out ? total_detections : 0);
        std::vector<int32_t> sorted_class_ids(has_class_ids_out ? total_detections : 0);
        std::vector<size_t> write_cursor(frame_offsets_local.size() > 0
            ? frame_offsets_local.size() - 1
            : 0, 0);
        for (size_t frame = 0; frame < write_cursor.size(); ++frame) {
            write_cursor[frame] = frame_offsets_local[frame];
        }

        for (size_t det = 0; det < total_detections; ++det) {
            int32_t frame_index = frame_indices_local[det];
            if (frame_index < 0) {
                continue;
            }
            size_t frame = static_cast<size_t>(frame_index);
            if (frame >= write_cursor.size()) {
                continue;
            }
            size_t dest = write_cursor[frame]++;
            if (dest >= sorted_boxes.size()) {
                continue;
            }
            sorted_boxes[dest] = boxes_local[det];
            if (has_scores_out) {
                sorted_scores[dest] = scores_out[det];
            }
            if (has_class_ids_out) {
                sorted_class_ids[dest] = class_ids_out[det];
            }
        }

        boxes_out = std::move(sorted_boxes);
        if (has_scores_out) {
            scores_out = std::move(sorted_scores);
        }
        if (has_class_ids_out) {
            class_ids_out = std::move(sorted_class_ids);
        }

        if (frame_indices_out) {
            *frame_indices_out = std::move(frame_indices_local);
            std::vector<int32_t>& indices_ref = *frame_indices_out;
            for (size_t frame = 0; frame + 1 < frame_offsets_local.size(); ++frame) {
                size_t start = frame_offsets_local[frame];
                size_t end = frame_offsets_local[frame + 1];
                for (size_t idx = start; idx < end && idx < indices_ref.size(); ++idx) {
                    indices_ref[idx] = static_cast<int32_t>(frame);
                }
            }
        }
    } else {
        size_t frame_count = n_detections_disk.size();
        resolved_frames_out = std::max(resolved_frames_out, frame_count);

        frame_offsets_local.assign(frame_count + 1, 0);
        size_t running = 0;
        for (size_t frame = 0; frame < frame_count; ++frame) {
            int32_t count = std::max(n_detections_disk[frame], 0);
            running += static_cast<size_t>(count);
            frame_offsets_local[frame + 1] = running;
        }

        if (running != total_detections) {
            std::cerr << "  Warning: detection count mismatch under " << base_path
                      << " (expected " << running << ", have " << total_detections << ")"
                      << std::endl;
        }

        boxes_out = std::move(boxes_local);

        if (frame_indices_out) {
            frame_indices_out->clear();
            frame_indices_out->reserve(total_detections);
            for (size_t frame = 0; frame < frame_count; ++frame) {
                int32_t count = std::max(n_detections_disk[frame], 0);
                for (int32_t i = 0; i < count; ++i) {
                    frame_indices_out->push_back(static_cast<int32_t>(frame));
                }
            }
        }
    }

    if (n_detections_disk.empty()) {
        n_detections_out.assign(resolved_frames_out, 0);
        for (size_t frame = 0; frame + 1 < frame_offsets_local.size(); ++frame) {
            size_t start = frame_offsets_local[frame];
            size_t end = frame_offsets_local[frame + 1];
            size_t count = (end >= start) ? (end - start) : 0;
            n_detections_out[frame] = static_cast<int32_t>(count);
        }
    } else {
        n_detections_out = std::move(n_detections_disk);
    }

    frame_offsets_out = std::move(frame_offsets_local);

    return true;
}

bool ZarrDetectionLoader::loadRefinedDetectRuns(const ts::kvstore::KvStore& store) {
    auto group_attrs = readJsonAttrs(store, "refined_detect_runs/.zattrs");
    if (!group_attrs.has_value()) {
        return false;
    }

    std::string latest_run = extractLatestRunName(*group_attrs);
    if (latest_run.empty()) {
        return false;
    }

    std::string run_base = "refined_detect_runs/" + latest_run + "/";
    std::cout << "  Loading refined detection run '" << latest_run << "'" << std::endl;

    InterpolationRunData refined;
    refined.run_name = latest_run;
    refined.uses_palette_layout = true;
    refined.has_flat_detections = false;

    if (auto run_attrs = readJsonAttrs(store, run_base + ".zattrs")) {
        populateInterpolationMetadata(*run_attrs, refined);
    }

    static const std::vector<std::string> kSubgroups = {
        "interpolated/",
        "refined/",
        "filtered/",
        ""
    };

    for (const auto& subgroup : kSubgroups) {
        std::string base_path = run_base + subgroup;
        if (!(arrayExists(store, base_path + "bbox_norm_coords") ||
              arrayExists(store, base_path + "bboxes"))) {
            continue;
        }

        size_t resolved_frames = data_.total_frames;
        std::vector<int32_t> frame_indices;
        std::vector<std::array<float, 4>> boxes;
        std::vector<float> scores;
        std::vector<int32_t> class_ids;
        std::vector<int32_t> n_detections;
        std::vector<size_t> frame_offsets;
        std::vector<uint8_t> detection_source;
        bool has_scores = false;
        bool has_class_ids = false;

        if (!loadFlattenedRun(store,
                              base_path,
                              &frame_indices,
                              boxes,
                              scores,
                              class_ids,
                              n_detections,
                              frame_offsets,
                              &detection_source,
                              has_scores,
                              has_class_ids,
                              resolved_frames)) {
            continue;
        }

        refined.has_flat_detections = true;
        refined.frame_indices = std::move(frame_indices);
        refined.bbox_norm_coords = std::move(boxes);
        refined.flat_scores = std::move(scores);
        refined.flat_class_ids = std::move(class_ids);
        refined.frame_offsets = std::move(frame_offsets);
        refined.detection_source = std::move(detection_source);
        refined.has_stimulus_alignment = false;
        refined.has_flat_detections = true;
        refined.is_loaded = true;
        refined.uses_palette_layout = true;

        if (refined.method.empty()) {
            if (!subgroup.empty()) {
                refined.method = subgroup.substr(0, subgroup.size() - 1);
            } else if (refined.method.empty()) {
                refined.method = "refined_detect";
            }
        }

        size_t frame_count = refined.frame_offsets.size() > 0
            ? refined.frame_offsets.size() - 1
            : resolved_frames;
        refined.frame_mask.assign(frame_count, 0);

        if (!refined.detection_source.empty()) {
            for (size_t frame = 0; frame + 1 < refined.frame_offsets.size(); ++frame) {
                size_t start = refined.frame_offsets[frame];
                size_t end = refined.frame_offsets[frame + 1];
                for (size_t idx = start; idx < end && idx < refined.detection_source.size(); ++idx) {
                    if (refined.detection_source[idx] != 0) {
                        refined.frame_mask[frame] = 1;
                        break;
                    }
                }
            }
        }

        data_.latest_interpolation = std::move(refined);
        data_.has_interpolation = true;

        std::string subgroup_label = subgroup.empty()
            ? "root"
            : subgroup.substr(0, subgroup.size() - 1);

        std::cout << "  Refined detect run '" << latest_run
                  << "' (" << subgroup_label << ") loaded with "
                  << data_.latest_interpolation.bbox_norm_coords.size()
                  << " detections" << std::endl;
        return true;
    }

    std::cout << "  Refined detect run '" << latest_run
              << "' does not contain interpolated outputs" << std::endl;
    return false;
}

bool ZarrDetectionLoader::loadStimulusAlignment(const ts::kvstore::KvStore& store) {
    auto group_attrs = readJsonAttrs(store, "analysis/stimulus_runs/.zattrs");
    if (!group_attrs.has_value()) {
        return false;
    }

    std::string latest_run = extractLatestRunName(*group_attrs);
    if (latest_run.empty()) {
        return false;
    }

    std::string run_base = "analysis/stimulus_runs/" + latest_run + "/";
    std::cout << "  Loading stimulus alignment run '" << latest_run << "'" << std::endl;

    std::string stimulus_created_at;
    int64_t camera_frame_offset = 0;
    std::vector<int32_t> camera_to_metadata_index;
    std::vector<uint8_t> metadata_mask;

    if (auto run_attrs = readJsonAttrs(store, run_base + ".zattrs")) {
        if (run_attrs->contains("created_at_utc") && (*run_attrs)["created_at_utc"].is_string()) {
            stimulus_created_at = (*run_attrs)["created_at_utc"].get<std::string>();
        } else if (run_attrs->contains("created_at") && (*run_attrs)["created_at"].is_string()) {
            stimulus_created_at = (*run_attrs)["created_at"].get<std::string>();
        }
    }

    if (auto align_attrs = readJsonAttrs(store, run_base + "frame_alignment/.zattrs")) {
        if (align_attrs->contains("camera_frame_offset") &&
            (*align_attrs)["camera_frame_offset"].is_number_integer()) {
            camera_frame_offset = (*align_attrs)["camera_frame_offset"].get<int64_t>();
        }
    }

    std::vector<uint8_t> camera_mask;
    bool has_camera_mask = readBoolArray(
        store,
        run_base + "frame_alignment/camera_interpolation_mask",
        camera_mask
    );

    std::vector<int64_t> camera_to_metadata_raw;
    bool has_camera_mapping = readInt64Array(
        store,
        run_base + "frame_alignment/camera_to_metadata_index",
        camera_to_metadata_raw
    );

    bool has_metadata_mask = readBoolArray(
        store,
        run_base + "interpolation_mask",
        metadata_mask
    );

    if (!has_camera_mask && !has_camera_mapping) {
        std::cout << "  Stimulus run '" << latest_run
                  << "' missing frame alignment data" << std::endl;
        return false;
    }

    if (!has_camera_mask && !(has_camera_mapping && has_metadata_mask)) {
        std::cout << "  Stimulus run '" << latest_run
                  << "' missing interpolation mask" << std::endl;
        return false;
    }

    if (has_camera_mapping) {
        camera_to_metadata_index.reserve(camera_to_metadata_raw.size());
        for (auto value : camera_to_metadata_raw) {
            camera_to_metadata_index.push_back(static_cast<int32_t>(value));
        }
    }

    size_t frame_count = 0;
    std::vector<uint8_t> stored_mask;
    if (has_camera_mask) {
        frame_count = camera_mask.size();
        stored_mask = camera_mask;
    } else if (has_camera_mapping) {
        frame_count = camera_to_metadata_raw.size();
    }

    if (frame_count == 0) {
        std::cout << "  Stimulus alignment has zero frames" << std::endl;
        return false;
    }

    std::vector<uint8_t> new_mask(frame_count, 0);
    if (has_camera_mask) {
        for (size_t i = 0; i < frame_count; ++i) {
            bool original = camera_mask[i] != 0;
            new_mask[i] = original ? 0 : 1;
        }
    } else if (has_camera_mapping && has_metadata_mask) {
        for (size_t i = 0; i < frame_count; ++i) {
            int64_t meta_idx = camera_to_metadata_raw[i];
            if (meta_idx >= 0 && static_cast<size_t>(meta_idx) < metadata_mask.size()) {
                bool original = metadata_mask[static_cast<size_t>(meta_idx)] != 0;
                new_mask[i] = original ? 0 : 1;
            }
        }
    }

    auto& interp = data_.latest_interpolation;
    if (interp.frame_mask.size() < frame_count) {
        interp.frame_mask.resize(frame_count, 0);
    }
    for (size_t i = 0; i < frame_count; ++i) {
        if (i < interp.frame_mask.size()) {
            interp.frame_mask[i] = std::max(interp.frame_mask[i], new_mask[i]);
        }
    }

    interp.stimulus_run_name = latest_run;
    interp.stimulus_created_at = stimulus_created_at;
    interp.camera_frame_offset = camera_frame_offset;
    interp.camera_to_metadata_index = std::move(camera_to_metadata_index);
    if (has_metadata_mask) {
        stored_mask = metadata_mask;
    }
    interp.stimulus_interpolation_mask = std::move(stored_mask);
    interp.has_stimulus_alignment = true;
    data_.has_interpolation = true;
    std::cout << "  Stimulus alignment '" << latest_run
              << "' loaded with frame mask for " << frame_count << " frames" << std::endl;
    return true;
}

bool ZarrDetectionLoader::loadStandardFormat(const ts::kvstore::KvStore& store) {
    try {
        // Load metadata first
        if (!loadMetadata(store)) {
            std::cerr << "Warning: Could not load metadata from .zattrs" << std::endl;
        }

        data_.layout = ZarrLayoutType::kLegacyGrid;
        data_.coordinates_normalized = false;
        data_.detect_run_name.clear();
        data_.frame_indices.clear();
        data_.frame_offsets.clear();
        data_.flat_scores.clear();
        data_.flat_class_ids.clear();
        data_.has_scores = false;
        data_.has_class_ids = false;

        // Load bounding boxes first to get dimensions (required)
        if (!loadBoundingBoxes(store, "bboxes")) {
            std::cerr << "Error: Could not load bboxes array" << std::endl;
            return false;
        }

        // Try to load n_detections array
        bool has_n_detections = false;
        
        // Check if n_detections exists
        auto n_det_check = ts::kvstore::Read(store, "n_detections/.zarray").result();
        if (n_det_check.ok()) {
            // n_detections array exists, load it
            if (loadNDetections(store, "n_detections")) {
                has_n_detections = true;
                std::cout << "  Loaded n_detections array" << std::endl;
            } else {
                std::cerr << "Warning: n_detections exists but failed to load" << std::endl;
            }
        }
        
        if (!has_n_detections) {
            std::cout << "  No n_detections array found, initializing..." << std::endl;
            
            data_.n_detections.clear();
            data_.n_detections.resize(data_.total_frames, 0);
            
            for (size_t frame_id = 0; frame_id < std::min(size_t(10), data_.total_frames); ++frame_id) {
                try {
                    auto bbox_slice = data_.bboxes_store | 
                        ts::Dims(0).IndexSlice(static_cast<tensorstore::Index>(frame_id));
                    auto bbox_result = ts::Read(bbox_slice).result();
                    
                    if (bbox_result.ok()) {
                        auto bbox_array = bbox_result.value();
                        auto* bbox_data = static_cast<const float*>(bbox_array.data());
                        
                        bool has_valid_detection = true;
                        for (int i = 0; i < 4; ++i) {
                            if (std::abs(bbox_data[i] + 1.0f) < 1e-6f) {
                                has_valid_detection = false;
                                break;
                            }
                        }
                        
                        if (has_valid_detection) {
                            data_.n_detections[frame_id] = 1;
                        }
                    }
                } catch (...) {
                    data_.n_detections[frame_id] = 0;
                }
            }
            
            if (data_.total_frames > 10) {
                std::cout << "  Note: Only scanned first 10 frames for valid detections." << std::endl;
                std::cout << "  Assuming remaining frames have valid detections." << std::endl;
                for (size_t i = 10; i < data_.total_frames; ++i) {
                    data_.n_detections[i] = 1;
                }
            }
        }

        if (!data_.n_detections.empty()) {
            auto max_iter = std::max_element(data_.n_detections.begin(), data_.n_detections.end());
            int32_t actual_max = (max_iter != data_.n_detections.end()) ? *max_iter : 0;
            if (actual_max != data_.max_detections) {
                std::cout << "  Note: Actual max detections (" << actual_max 
                          << ") differs from array dimension (" << data_.max_detections << ")" << std::endl;
            }
        }

        auto scores_check = ts::kvstore::Read(store, "scores/.zarray").result();
        if (scores_check.ok()) {
            if (loadScores(store, "scores")) {
                std::cout << "  Loaded scores array" << std::endl;
                data_.has_scores = true;
            } else {
                std::cerr << "Warning: scores array exists but failed to load" << std::endl;
            }
        } else {
            std::cout << "  No scores array found (optional)" << std::endl;
        }

        auto class_check = ts::kvstore::Read(store, "class_ids/.zarray").result();
        if (class_check.ok()) {
            if (loadClassIDs(store, "class_ids")) {
                std::cout << "  Loaded class_ids array" << std::endl;
                data_.has_class_ids = true;
            } else {
                std::cerr << "Warning: class_ids array exists but failed to load" << std::endl;
            }
        } else {
            std::cout << "  No class_ids array found (optional)" << std::endl;
        }

        int64_t total_detections = 0;
        for (const auto& n : data_.n_detections) {
            total_detections += n;
        }

        std::cout << "Successfully loaded Zarr detections:" << std::endl;
        std::cout << "  Total frames: " << data_.total_frames << std::endl;
        std::cout << "  Max detections per frame: " << data_.max_detections << std::endl;
        std::cout << "  Total detections: " << total_detections << std::endl;
        std::cout << "  Has scores: " << data_.has_scores << std::endl;
        std::cout << "  Has class IDs: " << data_.has_class_ids << std::endl;

        return true;

    } catch (const std::exception& e) {
        std::cerr << "Error in loadStandardFormat: " << e.what() << std::endl;
        return false;
    }
}

bool ZarrDetectionLoader::loadBoundingBoxes(const ts::kvstore::KvStore& store,
                                           const std::string& path) {
    try {
        json spec;
        spec["driver"] = "zarr";
        spec["kvstore"] = store.spec().value().ToJson().value();
        spec["path"] = path;

        auto open_result = ts::Open<float, 3>(
            spec,
            ts::OpenMode::open,
            ts::ReadWriteMode::read,
            context_
        ).result();

        if (!open_result.ok()) {
            return false;
        }
        data_.bboxes_store = open_result.value();

        auto domain = data_.bboxes_store.domain();
        data_.total_frames = domain.shape()[0];
        data_.max_detections = domain.shape()[1];

        return true;

    } catch (const std::exception& e) {
        std::cerr << "Error loading bboxes from " << path << ": " << e.what() << std::endl;
        return false;
    }
}

bool ZarrDetectionLoader::loadScores(const ts::kvstore::KvStore& store,
                                    const std::string& path) {
    try {
        json spec;
        spec["driver"] = "zarr";
        spec["kvstore"] = store.spec().value().ToJson().value();
        spec["path"] = path;

        auto open_result = ts::Open<float, 2>(
            spec,
            ts::OpenMode::open,
            ts::ReadWriteMode::read,
            context_
        ).result();

        if (!open_result.ok()) {
            return false;
        }

        data_.scores_store = open_result.value();
        data_.has_scores = true;
        return true;

    } catch (...) {
        return false;
    }
}

bool ZarrDetectionLoader::loadClassIDs(const ts::kvstore::KvStore& store,
                                      const std::string& path) {
    try {
        json spec;
        spec["driver"] = "zarr";
        spec["kvstore"] = store.spec().value().ToJson().value();
        spec["path"] = path;

        auto open_result = ts::Open<int32_t, 2>(
            spec,
            ts::OpenMode::open,
            ts::ReadWriteMode::read,
            context_
        ).result();

        if (!open_result.ok()) {
            return false;
        }

        data_.class_ids_store = open_result.value();
        data_.has_class_ids = true;
        return true;

    } catch (...) {
        return false;
    }
}

bool ZarrDetectionLoader::loadNDetections(const ts::kvstore::KvStore& store,
                                         const std::string& path) {
    try {
        json spec;
        spec["driver"] = "zarr";
        spec["kvstore"] = store.spec().value().ToJson().value();
        spec["path"] = path;

        auto open_result = ts::Open<int32_t, 1>(
            spec,
            ts::OpenMode::open,
            ts::ReadWriteMode::read,
            context_
        ).result();

        if (!open_result.ok()) {
            return false;
        }

        auto n_det_store = open_result.value();
        auto read_result = ts::Read(n_det_store).result();
        if (!read_result.ok()) {
            return false;
        }

        auto n_det_array = read_result.value();
        data_.total_frames = n_det_array.shape()[0];

        data_.n_detections.clear();
        data_.n_detections.reserve(data_.total_frames);
        for (int i = 0; i < data_.total_frames; ++i) {
            data_.n_detections.push_back(n_det_array.data()[i]);
        }

        return true;

    } catch (const std::exception& e) {
        std::cerr << "Error loading n_detections: " << e.what() << std::endl;
        return false;
    }
}

bool ZarrDetectionLoader::loadMetadata(const ts::kvstore::KvStore& store) {
    try {
        bool metadata_found = false;

        auto attrs_result = ts::kvstore::Read(store, ".zattrs").result();
        if (attrs_result.ok() && attrs_result.value().has_value()) {
            auto attrs = json::parse(attrs_result.value().value.Flatten());
            metadata_found = true;

            if (attrs.contains("video_path") && attrs["video_path"].is_string()) {
                data_.video_path = attrs["video_path"].get<std::string>();
            }
            if (attrs.contains("fps") && attrs["fps"].is_number()) {
                data_.fps = attrs["fps"].get<double>();
            }
            if (attrs.contains("model_path") && attrs["model_path"].is_string()) {
                data_.model_path = attrs["model_path"].get<std::string>();
            }
            if (attrs.contains("total_frames") && attrs["total_frames"].is_number()) {
                data_.total_frames = static_cast<size_t>(attrs["total_frames"].get<double>());
            }
            if (attrs.contains("width") && attrs["width"].is_number()) {
                data_.image_width = static_cast<int>(attrs["width"].get<double>());
            }
            if (attrs.contains("height") && attrs["height"].is_number()) {
                data_.image_height = static_cast<int>(attrs["height"].get<double>());
            }
            if (attrs.contains("frame_width") && attrs["frame_width"].is_number()) {
                data_.image_width = static_cast<int>(attrs["frame_width"].get<double>());
            }
            if (attrs.contains("frame_height") && attrs["frame_height"].is_number()) {
                data_.image_height = static_cast<int>(attrs["frame_height"].get<double>());
            }
        }

        auto store_spec = store.spec();
        if (store_spec.ok() &&
            (data_.image_width <= 0 || data_.image_height <= 0 || data_.total_frames == 0)) {
            json video_spec;
            video_spec["driver"] = "zarr";
            video_spec["kvstore"] = store_spec.value().ToJson().value();
            video_spec["path"] = "raw_video/images_full";

            auto video_result = ts::Open<uint8_t, 3>(
                video_spec,
                ts::OpenMode::open,
                ts::ReadWriteMode::read,
                context_
            ).result();

            if (video_result.ok()) {
                auto domain = video_result.value().domain();
                if (domain.rank() >= 3) {
                    if (data_.total_frames == 0) {
                        data_.total_frames = static_cast<size_t>(domain.shape()[0]);
                    }
                    if (data_.image_height <= 0) {
                        data_.image_height = static_cast<int>(domain.shape()[1]);
                    }
                    if (data_.image_width <= 0) {
                        data_.image_width = static_cast<int>(domain.shape()[2]);
                    }
                    metadata_found = true;
                }
            } else {
                // Try downsampled images as a fallback
                video_spec["path"] = "raw_video/images_ds";
                video_result = ts::Open<uint8_t, 3>(
                    video_spec,
                    ts::OpenMode::open,
                    ts::ReadWriteMode::read,
                    context_
                ).result();

                if (video_result.ok()) {
                    auto domain = video_result.value().domain();
                    if (domain.rank() >= 3) {
                        if (data_.total_frames == 0) {
                            data_.total_frames = static_cast<size_t>(domain.shape()[0]);
                        }
                        if (data_.image_height <= 0) {
                            data_.image_height = static_cast<int>(domain.shape()[1]);
                        }
                        if (data_.image_width <= 0) {
                            data_.image_width = static_cast<int>(domain.shape()[2]);
                        }
                        metadata_found = true;
                    }
                }
            }
        }

        return metadata_found;

    } catch (...) {
        return false;
    }
}

std::vector<LoggedBoundingBox> ZarrDetectionLoader::getBoundingBoxesForFrame(size_t frame_id) const {
    std::vector<LoggedBoundingBox> result;
    
    if (frame_id >= data_.total_frames) {
        return result;
    }
    
    auto detections = getRawDetections(frame_id, false);
    
    for (size_t i = 0; i < detections.boxes.size(); ++i) {
        LoggedBoundingBox box;
        
        box.x_min = detections.boxes[i][0];
        box.y_min = detections.boxes[i][1];
        
        float x_max = detections.boxes[i][2];
        float y_max = detections.boxes[i][3];
        box.width = x_max - box.x_min;
        box.height = y_max - box.y_min;
        
        box.payload_frame_id = frame_id;
        box.payload_camera_id = 0;
        box.box_index_in_payload = static_cast<uint8_t>(i);
        
        if (i < detections.class_ids.size()) {
            box.class_id = static_cast<uint16_t>(detections.class_ids[i]);
        } else {
            box.class_id = 0;
        }
        
        if (i < detections.scores.size()) {
            box.confidence = detections.scores[i];
        } else {
            box.confidence = 1.0f;
        }
        
        int64_t timestamp_ns = static_cast<int64_t>(frame_id / data_.fps * 1e9);
        box.payload_timestamp_ns_epoch = timestamp_ns;
        box.received_timestamp_ns_epoch = timestamp_ns;
        
        result.push_back(box);
    }
    
    return result;
}

int32_t ZarrDetectionLoader::getDetectionsForFrame(size_t frame_id) const {
    if (frame_id >= data_.total_frames) {
        return 0;
    }
    return data_.n_detections[frame_id];
}

LoggedBoundingBox ZarrDetectionLoader::convertToLoggedBox(
    const std::array<float, 4>& box,
    float score,
    int32_t class_id,
    size_t frame_id,
    size_t box_index,
    bool is_interpolated
) const {
    LoggedBoundingBox result;
    
    result.x_min = box[0];
    result.y_min = box[1];
    result.width = box[2] - box[0];
    result.height = box[3] - box[1];
    
    result.payload_frame_id = frame_id;
    result.payload_camera_id = 0;
    result.box_index_in_payload = static_cast<uint8_t>(box_index);
    
    result.class_id = static_cast<uint16_t>(class_id);
    result.confidence = score;
    
    int64_t timestamp_ns = static_cast<int64_t>(frame_id / data_.fps * 1e9);
    result.payload_timestamp_ns_epoch = timestamp_ns;
    result.received_timestamp_ns_epoch = timestamp_ns;
    
    return result;
}

std::optional<std::string> ZarrDetectionLoader::findZarrDetectionFile(const std::string& directory) {
    namespace fs = std::filesystem;

    try {
        for (const auto& entry : fs::directory_iterator(directory)) {
            if (entry.is_directory()) {
                std::string filename = entry.path().filename().string();

                if (filename.find(".zarr") != std::string::npos ||
                    filename.find(".zr3") != std::string::npos) {

                    if (filename.find("detection") != std::string::npos ||
                        filename.find("chaser") != std::string::npos) {
                        return entry.path().string();
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error searching for zarr files: " << e.what() << std::endl;
    }

    return std::nullopt;
}

bool loadZarrDetectionFromDirectory(
    const std::string& dir_path,
    ZarrDetectionLoader& loader,
    std::string& error_message
) {
    auto zarr_file = ZarrDetectionLoader::findZarrDetectionFile(dir_path);

    if (!zarr_file.has_value()) {
        error_message = "No zarr detection file found in directory";
        return false;
    }

    std::cout << "Found zarr file: " << zarr_file.value() << std::endl;
    return loader.loadZarrFile(zarr_file.value(), error_message);
}

bool ZarrDetectionLoader::loadInterpolationRuns(const ts::kvstore::KvStore& store) {
    try {
        data_.latest_interpolation = InterpolationRunData();
        data_.has_interpolation = false;

        bool refined_loaded = false;
        bool stimulus_loaded = false;

        if (data_.layout == ZarrLayoutType::kPaletteRuns) {
            refined_loaded = loadRefinedDetectRuns(store);
        }

        if (loadStimulusAlignment(store)) {
            stimulus_loaded = true;
        }

        if (refined_loaded || stimulus_loaded) {
            data_.has_interpolation = true;
            return true;
        }

        // Fallback to legacy refined/interpolation runs
        data_.latest_interpolation = InterpolationRunData();

        if (data_.layout == ZarrLayoutType::kPaletteRuns) {
            auto refined_attrs_result = ts::kvstore::Read(store, "refined_runs/.zattrs").result();
            if (refined_attrs_result.ok() && refined_attrs_result.value().has_value()) {
                auto attrs_json = json::parse(refined_attrs_result.value().value.Flatten());
                std::string latest_run = "";
                if (attrs_json.contains("latest")) {
                    latest_run = attrs_json["latest"].get<std::string>();
                } else if (attrs_json.contains("latest_completed")) {
                    latest_run = attrs_json["latest_completed"].get<std::string>();
                }

                if (!latest_run.empty()) {
                    std::string base_path = "refined_runs/" + latest_run + "/";
                    std::string subgroup;

                    auto interpolated_check = ts::kvstore::Read(
                        store,
                        base_path + "interpolated/frame_indices/.zarray"
                    ).result();
                    if (interpolated_check.ok()) {
                        subgroup = "interpolated";
                    } else {
                        auto filtered_check = ts::kvstore::Read(
                            store,
                            base_path + "filtered/frame_indices/.zarray"
                        ).result();
                        if (filtered_check.ok()) {
                            subgroup = "filtered";
                        }
                    }

                    if (!subgroup.empty()) {
                        std::cout << "  Found refined run: " << latest_run
                                  << " (" << subgroup << ")" << std::endl;
                        if (loadPaletteInterpolationRun(store, latest_run, subgroup)) {
                            return true;
                        }
                    } else {
                        std::cout << "  Refined run " << latest_run
                                  << " does not contain 'interpolated' or 'filtered' outputs" << std::endl;
                    }
                }
            }
            // Fall through to legacy loaders if refined data not available
        }

        // The actual interpolation run we know exists is:
        // /interpolation_runs/interp_linear_20250820_125819/
        // Let's check for it directly first
        
        std::string known_run = "interp_linear_20250820_125819";
        std::string test_path = "interpolation_runs/" + known_run + "/bboxes/.zarray";
        
        auto test_result = ts::kvstore::Read(store, test_path).result();
        if (test_result.ok()) {
            std::cout << "  Found interpolation run: " << known_run << std::endl;
            std::cout << "  Loading run: " << known_run << std::endl;
            return loadLatestInterpolationRun(store, known_run);
        }
        
        // If that didn't work, try scanning for other patterns
        std::vector<std::string> run_names;
        
        // Common prefixes for interpolation runs
        std::vector<std::string> prefixes = {
            "interp_linear_",
            "interp_cubic_",
            "interp_nearest_",
            "interp_"
        };
        
        // Scan for runs with these patterns
        // Since we know the format is typically prefix + YYYYMMDD_HHMMSS
        // Let's try dates from today backwards
        auto now = std::chrono::system_clock::now();
        
        for (int days_back = 0; days_back < 365; ++days_back) {  // Check up to a year back
            auto check_time = now - std::chrono::hours(24 * days_back);
            auto check_time_t = std::chrono::system_clock::to_time_t(check_time);
            struct tm* tm_check = std::localtime(&check_time_t);
            
            // Try different times of day
            for (int hour = 23; hour >= 0; hour -= 4) {
                for (int min = 59; min >= 0; min -= 30) {
                    for (int sec = 59; sec >= 0; sec -= 30) {
                        // Try each prefix
                        for (const auto& prefix : prefixes) {
                            char run_name[100];
                            snprintf(run_name, sizeof(run_name), 
                                    "%s%04d%02d%02d_%02d%02d%02d",
                                    prefix.c_str(),
                                    tm_check->tm_year + 1900,
                                    tm_check->tm_mon + 1,
                                    tm_check->tm_mday,
                                    hour, min, sec);
                            
                            // Check if this run exists by looking for its bboxes array
                            std::string bbox_path = std::string("interpolation_runs/") + 
                                                   run_name + "/bboxes/.zarray";
                            auto result = ts::kvstore::Read(store, bbox_path).result();
                            
                            if (result.ok()) {
                                run_names.push_back(run_name);
                                std::cout << "    Found interpolation run: " << run_name << std::endl;
                                
                                // If we found one, that's probably enough
                                goto found_some;
                            }
                        }
                    }
                }
            }
            
            // Stop early if we found something
            if (!run_names.empty()) {
                break;
            }
        }
        
        found_some:
        if (run_names.empty()) {
            std::cout << "  No interpolation runs found" << std::endl;
            return false;
        }
        
        // Sort to get the latest (timestamp format means alphabetical = chronological)
        std::sort(run_names.begin(), run_names.end());
        std::string latest_run = run_names.back();
        
        std::cout << "  Found " << run_names.size() << " interpolation run(s)" << std::endl;
        std::cout << "  Loading latest run: " << latest_run << std::endl;
        
        // Load the latest interpolation run
        return loadLatestInterpolationRun(store, latest_run);
        
    } catch (const std::exception& e) {
        std::cerr << "Error loading interpolation runs: " << e.what() << std::endl;
        return false;
    }
}


bool ZarrDetectionLoader::loadLatestInterpolationRun(const ts::kvstore::KvStore& store, 
                                                     const std::string& run_name) {
    try {
        data_.latest_interpolation.uses_palette_layout = false;
        std::string base_path = "interpolation_runs/" + run_name + "/";
        
        // Try to load metadata, but don't fail if it's missing or empty
        auto attrs_result = ts::kvstore::Read(store, base_path + ".zattrs").result();
        if (attrs_result.ok() && attrs_result.value().has_value()) {
            // Convert Cord to string using Flatten()
            std::string attrs_str = std::string(attrs_result.value().value.Flatten());
            
            // Check if the string is not empty before parsing
            if (!attrs_str.empty()) {
                try {
                    auto attrs_json = nlohmann::json::parse(attrs_str);
                    
                    data_.latest_interpolation.run_name = run_name;
                    data_.latest_interpolation.created_at = attrs_json.value("created_at", "");
                    data_.latest_interpolation.method = attrs_json.value("method", "linear");
                    
                    std::cout << "    Created: " << data_.latest_interpolation.created_at << std::endl;
                    std::cout << "    Method: " << data_.latest_interpolation.method << std::endl;
                } catch (const nlohmann::json::parse_error& e) {
                    std::cout << "    Warning: Could not parse .zattrs (may be empty): " << e.what() << std::endl;
                    // Continue with defaults
                    data_.latest_interpolation.run_name = run_name;
                    data_.latest_interpolation.method = "linear";  // Reasonable default
                }
            } else {
                std::cout << "    Warning: Empty .zattrs file, using defaults" << std::endl;
                data_.latest_interpolation.run_name = run_name;
                data_.latest_interpolation.method = "linear";
            }
        } else {
            std::cout << "    No .zattrs found, using defaults" << std::endl;
            data_.latest_interpolation.run_name = run_name;
            // Extract method from run name if possible (e.g., "interp_linear_20250820_125819")
            if (run_name.find("_linear_") != std::string::npos) {
                data_.latest_interpolation.method = "linear";
            } else if (run_name.find("_cubic_") != std::string::npos) {
                data_.latest_interpolation.method = "cubic";
            } else if (run_name.find("_nearest_") != std::string::npos) {
                data_.latest_interpolation.method = "nearest";
            } else {
                data_.latest_interpolation.method = "unknown";
            }
        }
        
        // Now load the actual data arrays - these are required
        
        // Load interpolated bboxes
        json spec;
        spec["driver"] = "zarr";
        spec["kvstore"] = store.spec().value().ToJson().value();
        spec["path"] = base_path + "bboxes";
        
        std::cout << "    Loading interpolated bboxes from: " << base_path + "bboxes" << std::endl;
        
        auto bbox_result = ts::Open<float, 3>(
            spec,
            ts::OpenMode::open,
            ts::ReadWriteMode::read,
            context_
        ).result();
        
        if (!bbox_result.ok()) {
            std::cerr << "Failed to load interpolated bboxes: " << bbox_result.status().ToString() << std::endl;
            return false;
        }
        
        data_.latest_interpolation.bboxes_store = bbox_result.value();
        std::cout << "    Successfully loaded interpolated bboxes" << std::endl;
        
        // Load interpolation mask (true = interpolated, false = original)
        spec["path"] = base_path + "interpolation_mask";
        
        std::cout << "    Loading interpolation mask from: " << base_path + "interpolation_mask" << std::endl;
        
        auto mask_result = ts::Open<bool, 1>(
            spec,
            ts::OpenMode::open,
            ts::ReadWriteMode::read,
            context_
        ).result();
        
        if (!mask_result.ok()) {
            std::cerr << "Failed to load interpolation mask: " << mask_result.status().ToString() << std::endl;
            return false;
        }
        
        data_.latest_interpolation.interpolation_mask = mask_result.value();
        data_.latest_interpolation.is_loaded = true;
        data_.has_interpolation = true;
        
        std::cout << "    Successfully loaded interpolation mask" << std::endl;
        
        // Get dimensions for validation
        auto bbox_domain = data_.latest_interpolation.bboxes_store.domain();
        auto mask_domain = data_.latest_interpolation.interpolation_mask.domain();
        
        size_t bbox_frames = bbox_domain.shape()[0];
        size_t mask_frames = mask_domain.shape()[0];
        
        std::cout << "    Bbox frames: " << bbox_frames << std::endl;
        std::cout << "    Mask frames: " << mask_frames << std::endl;
        
        if (bbox_frames != mask_frames) {
            std::cerr << "    Warning: Frame count mismatch between bboxes and mask!" << std::endl;
        }
        
        // Try to count interpolated frames (but don't fail if it doesn't work)
        try {
            auto mask_array = ts::Read(data_.latest_interpolation.interpolation_mask).result().value();
            auto* mask_data = static_cast<const bool*>(mask_array.data());
            
            size_t interpolated_count = 0;
            for (size_t i = 0; i < mask_frames; ++i) {
                if (mask_data[i]) {
                    interpolated_count++;
                }
            }
            
            std::cout << "    Interpolated frames: " << interpolated_count 
                      << " / " << mask_frames << std::endl;
        } catch (const std::exception& e) {
            std::cout << "    Could not count interpolated frames: " << e.what() << std::endl;
        }
        
        std::cout << "    Successfully loaded interpolation run: " << run_name << std::endl;
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "Error loading interpolation run " << run_name 
                  << ": " << e.what() << std::endl;
        return false;
    }
}

bool ZarrDetectionLoader::isFrameInterpolated(size_t frame_id) const {
    if (!data_.has_interpolation) {
        return false;
    }

    if (!data_.latest_interpolation.frame_mask.empty() &&
        frame_id < data_.latest_interpolation.frame_mask.size()) {
        return data_.latest_interpolation.frame_mask[frame_id] != 0;
    }

    if (frame_id >= data_.total_frames) {
        return false;
    }

    if (data_.layout == ZarrLayoutType::kPaletteRuns &&
        data_.latest_interpolation.uses_palette_layout) {
        return false;
    }
    
    try {
        auto ts_frame_id = static_cast<tensorstore::Index>(frame_id);
        
        auto mask_future = ts::Read(
            data_.latest_interpolation.interpolation_mask | 
            ts::Dims(0).IndexSlice(ts_frame_id)
        );
        
        auto mask_array = mask_future.result().value();
        auto* mask_data = static_cast<const bool*>(mask_array.data());
        
        return mask_data[0];
        
    } catch (const std::exception& e) {
        std::cerr << "Error checking interpolation status for frame " 
                  << frame_id << ": " << e.what() << std::endl;
        return false;
    }
}

std::vector<LoggedBoundingBox> ZarrDetectionLoader::getBoundingBoxesForFrame(
    size_t frame_id, bool use_interpolated) const {
    
    if (use_interpolated && data_.has_interpolation) {
        auto detections = getRawDetections(frame_id, true);
        return convertDetectionsToLoggedBoxes(detections, frame_id);
    } else {
        return getBoundingBoxesForFrame(frame_id);
    }
}

ZarrDetectionLoader::FrameDetections ZarrDetectionLoader::getRawDetections(
    size_t frame_id, bool use_interpolated) const {
    
    FrameDetections result;
    result.frame_id = frame_id;
    
    if (frame_id >= data_.total_frames) {
        return result;
    }

    if (data_.layout == ZarrLayoutType::kPaletteRuns) {
        const bool has_palette_interp =
            data_.latest_interpolation.uses_palette_layout &&
            data_.latest_interpolation.is_loaded;
        const bool want_interpolated = use_interpolated && has_palette_interp;

        const auto& offsets = want_interpolated
            ? data_.latest_interpolation.frame_offsets
            : data_.frame_offsets;
        const auto& boxes_source = want_interpolated
            ? data_.latest_interpolation.bbox_norm_coords
            : data_.bbox_norm_coords;
        const auto& scores_source = want_interpolated
            ? data_.latest_interpolation.flat_scores
            : data_.flat_scores;
        const auto& class_source = want_interpolated
            ? data_.latest_interpolation.flat_class_ids
            : data_.flat_class_ids;

        const bool has_scores = want_interpolated
            ? !data_.latest_interpolation.flat_scores.empty()
            : data_.has_scores;
        const bool has_class_ids = want_interpolated
            ? !data_.latest_interpolation.flat_class_ids.empty()
            : data_.has_class_ids;

        if (offsets.empty() || frame_id + 1 >= offsets.size()) {
            return result;
        }

        size_t start = offsets[frame_id];
        size_t end = offsets[frame_id + 1];

        for (size_t idx = start; idx < end && idx < boxes_source.size(); ++idx) {
            auto pixel_box = normalizedBoxToPixels(
                boxes_source[idx],
                data_.image_width,
                data_.image_height
            );
            result.boxes.push_back(pixel_box);

            if (has_scores && idx < scores_source.size()) {
                result.scores.push_back(scores_source[idx]);
            } else {
                result.scores.push_back(1.0f);
            }

            if (has_class_ids && idx < class_source.size()) {
                result.class_ids.push_back(class_source[idx]);
            } else {
                result.class_ids.push_back(0);
            }
        }

        bool frame_interp_flag = false;
        if (frame_id < data_.latest_interpolation.frame_mask.size()) {
            frame_interp_flag = data_.latest_interpolation.frame_mask[frame_id] != 0;
        }

        result.is_interpolated = frame_interp_flag;
        return result;
    }
    
    try {
        auto ts_frame_id = static_cast<tensorstore::Index>(frame_id);
        
        bool use_interp = use_interpolated && data_.has_interpolation;
        ts::TensorStore<float, 3>* bbox_source = nullptr;
        
        if (use_interp) {
            bbox_source = const_cast<ts::TensorStore<float, 3>*>(
                &data_.latest_interpolation.bboxes_store);
            result.is_interpolated = isFrameInterpolated(frame_id);
        } else {
            bbox_source = const_cast<ts::TensorStore<float, 3>*>(&data_.bboxes_store);
            result.is_interpolated = false;
        }
        
        auto bbox_future = ts::Read(
            *bbox_source | 
            ts::Dims(0).IndexSlice(ts_frame_id)
        );
        
        auto bbox_array = bbox_future.result().value();
        auto* bbox_data = static_cast<const float*>(bbox_array.data());
        
        // FIX: For interpolated data, we need to check the actual array dimensions
        // or scan for valid boxes since n_detections might not apply
        int valid_detections;
        if (use_interp) {
            // For interpolated data, check the shape of the array
            // The second dimension should be max_detections
            auto shape = bbox_array.shape();
            int max_dets = shape[0];  // This is the max detections dimension
            
            // Count valid detections by checking for non-negative values
            valid_detections = 0;
            for (int det_idx = 0; det_idx < max_dets; ++det_idx) {
                float first_coord = bbox_data[det_idx * 4];
                if (first_coord >= 0) {
                    valid_detections++;
                } else {
                    break;  // Assuming detections are packed at the beginning
                }
            }
            
            std::cout << "Interpolated frame " << frame_id << ": found " 
                      << valid_detections << " valid detections" << std::endl;
        } else {
            // Use original n_detections for non-interpolated frames
            valid_detections = data_.n_detections[frame_id];
        }
        
        for (int det_idx = 0; det_idx < valid_detections; ++det_idx) {
            std::array<float, 4> box;
            for (int coord = 0; coord < 4; ++coord) {
                box[coord] = bbox_data[det_idx * 4 + coord];
            }
            
            if (box[0] >= 0) {
                result.boxes.push_back(box);
                
                if (!use_interp && data_.has_scores) {
                    auto score_future = ts::Read(
                        data_.scores_store | 
                        ts::Dims(0).IndexSlice(ts_frame_id)
                    );
                    auto score_array = score_future.result().value();
                    auto* score_data = static_cast<const float*>(score_array.data());
                    result.scores.push_back(score_data[det_idx]);
                } else {
                    result.scores.push_back(1.0f);
                }
                
                if (!use_interp && data_.has_class_ids) {
                    auto class_future = ts::Read(
                        data_.class_ids_store | 
                        ts::Dims(0).IndexSlice(ts_frame_id)
                    );
                    auto class_array = class_future.result().value();
                    auto* class_data = static_cast<const int32_t*>(class_array.data());
                    result.class_ids.push_back(class_data[det_idx]);
                } else {
                    result.class_ids.push_back(0);
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error reading frame " << frame_id << ": " << e.what() << std::endl;
    }

    result.is_interpolated = isFrameInterpolated(frame_id);
    
    return result;
}

std::vector<LoggedBoundingBox> ZarrDetectionLoader::convertDetectionsToLoggedBoxes(
    const FrameDetections& detections, size_t frame_id) const {
    
    std::vector<LoggedBoundingBox> result;
    
    for (size_t i = 0; i < detections.boxes.size(); ++i) {
        LoggedBoundingBox box = convertToLoggedBox(
            detections.boxes[i],
            i < detections.scores.size() ? detections.scores[i] : 1.0f,
            i < detections.class_ids.size() ? detections.class_ids[i] : 0,
            frame_id,
            i,
            detections.is_interpolated
        );
        result.push_back(box);
    }
    
    return result;
}
