#include "zarr_loader.h"
#include <iostream>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <absl/strings/cord.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <utility>
#include <limits>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <tensorstore/cast.h>
#include <tensorstore/driver/zarr/dtype.h>
#include <tensorstore/kvstore/operations.h>

using json = nlohmann::json;

namespace {
constexpr bool kChaserDebugLoggingEnabled =
#if defined(CRIMSON_CHASER_DEBUG_LOGS)
    true;
#else
    false;
#endif
;

std::string appendPath(const std::string& base, const std::string& suffix) {
    if (base.empty()) {
        return suffix;
    }
    if (base.back() == '/') {
        return base + suffix;
    }
    return base + "/" + suffix;
}

std::optional<json> readAttrsAny(const ts::kvstore::KvStore& store,
                                 const std::string& path) {
    auto json_key = appendPath(path, "zarr.json");
    auto json_result = ts::kvstore::Read(store, json_key).result();
    if (json_result.ok() && json_result.value().has_value()) {
        try {
            std::string payload;
            absl::CopyCordToString(json_result.value().value, &payload);
            if (!payload.empty()) {
                json meta = json::parse(payload);
                if (meta.contains("attributes") && meta["attributes"].is_object()) {
                    return meta["attributes"];
                }
                return meta;
            }
        } catch (const json::parse_error& e) {
            std::cerr << "Failed to parse JSON at " << json_key << ": " << e.what() << std::endl;
        } catch (...) {
            // Ignore malformed attribute blobs.
        }
    } else {
        if (!json_result.ok()) {
            std::cout << "  [AttrProbe] Read error for " << json_key << ": "
                      << json_result.status() << std::endl;
        } else {
            std::cout << "  [AttrProbe] No value at " << json_key << std::endl;
        }
    }
    return std::nullopt;
}

template <typename T, int Rank>
ts::Result<ts::TensorStore<T, Rank>> openArrayAny(
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

    auto try_open = [&](const char* driver) {
        json spec = {
            {"driver", driver},
            {"kvstore", kv_json_or.value()},
            {"path", path}
        };
        return ts::Open<T, Rank>(
                   spec,
                   ts::OpenMode::open,
                   ts::ReadWriteMode::read,
                   context)
            .result();
    };

    auto v3_result = try_open("zarr3");
    if (v3_result.ok()) {
        return v3_result;
    }
    auto v2_result = try_open("zarr");
    if (v2_result.ok()) {
        return v2_result;
    }
    return v2_result;
}

bool arrayExists(const ts::kvstore::KvStore& store, const std::string& path) {
    auto v3 = ts::kvstore::Read(store, appendPath(path, "zarr.json")).result();
    if (v3.ok() && v3.value().has_value()) {
        return true;
    }
    auto result = ts::kvstore::Read(store, appendPath(path, ".zarray")).result();
    return result.ok() && result.value().has_value();
}

#pragma pack(push, 1)
struct StimulusEventRowV3 {
    int64_t timestamp_ns_epoch = 0;
    int64_t timestamp_ns_session = 0;
    int32_t event_type_id = 0;
    int32_t current_step_index = 0;
    uint64_t stimulus_frame_num = 0;
    uint64_t camera_frame_id = 0;
    char name_or_context[256];
    int32_t stimulus_mode_id = 0;
    char details_json[1024];
};
#pragma pack(pop)

std::string stringFromFixedBuffer(const char* buffer, size_t size) {
    const char* end = static_cast<const char*>(
        std::memchr(buffer, '\0', size));
    if (!end) {
        return std::string(buffer, buffer + size);
    }
    return std::string(buffer, end);
}

int32_t clampToInt32(int64_t value) {
    if (value < std::numeric_limits<int32_t>::min()) {
        return std::numeric_limits<int32_t>::min();
    }
    if (value > std::numeric_limits<int32_t>::max()) {
        return std::numeric_limits<int32_t>::max();
    }
    return static_cast<int32_t>(value);
}

int32_t clampUint64ToInt32(uint64_t value) {
    if (value > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
        return -1;
    }
    return static_cast<int32_t>(value);
}

template <typename Source>
int32_t convertToInt32WithClamp(Source value, bool& overflow_flag) {
    if constexpr (std::is_same_v<Source, bool>) {
        return value ? 1 : 0;
    } else if constexpr (std::is_signed_v<Source>) {
        int64_t as64 = static_cast<int64_t>(value);
        if (as64 < std::numeric_limits<int32_t>::min()) {
            overflow_flag = true;
            return std::numeric_limits<int32_t>::min();
        }
        if (as64 > std::numeric_limits<int32_t>::max()) {
            overflow_flag = true;
            return std::numeric_limits<int32_t>::max();
        }
        return static_cast<int32_t>(as64);
    } else {
        if (value > static_cast<Source>(std::numeric_limits<int32_t>::max())) {
            overflow_flag = true;
            return std::numeric_limits<int32_t>::max();
        }
        return static_cast<int32_t>(value);
    }
}

template <typename Source>
int64_t convertToInt64WithClamp(Source value, bool& overflow_flag) {
    if constexpr (std::is_same_v<Source, bool>) {
        return value ? 1 : 0;
    } else if constexpr (std::is_signed_v<Source>) {
        return static_cast<int64_t>(value);
    } else {
        if (value > static_cast<Source>(std::numeric_limits<int64_t>::max())) {
            overflow_flag = true;
            return std::numeric_limits<int64_t>::max();
        }
        return static_cast<int64_t>(value);
    }
}

std::vector<std::string> collect_runs_fs(const std::string& root_path,
                                            const std::string& group_dir,
                                            std::initializer_list<std::string> required_arrays) {
    namespace fs = std::filesystem;
    fs::path base = fs::path(root_path) / group_dir;
    std::vector<std::string> candidates;
    if (!fs::exists(base) || !fs::is_directory(base)) {
        return candidates;
    }

    for (const auto& entry : fs::directory_iterator(base)) {
        if (!entry.is_directory()) {
            continue;
        }
        const auto name = entry.path().filename().string();
        bool ok = true;
        for (const auto& req : required_arrays) {
            if (!fs::exists(entry.path() / req / "zarr.json")) {
                ok = false;
                break;
            }
        }
        if (ok) {
            candidates.push_back(name);
        }
    }

    std::sort(candidates.begin(), candidates.end());
    return candidates;
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
        active_dataset_ = DetectionDataset::RawDetect;
        
        std::cout << "Opening Zarr store: " << filepath << std::endl;

        // Validate filepath exists
        if (!std::filesystem::exists(filepath)) {
            error_message = "Zarr file/directory does not exist: " + filepath;
            return false;
        }

        root_path_ = filepath;

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

        // Debug probes to help diagnose layout selection.
        {
            auto probe = ts::kvstore::Read(store, "detect_runs/zarr.json").result();
            if (probe.ok()) {
                const bool kv_has_value = probe.value().has_value();
                std::string payload;
                bool have_payload = false;

                if (kv_has_value) {
                    std::cout << "  probe detect_runs/zarr.json: FOUND" << std::endl;
                    absl::CopyCordToString(probe.value().value, &payload);
                    have_payload = true;
                } else {
                    std::filesystem::path fs_path =
                        std::filesystem::path(filepath) / "detect_runs" / "zarr.json";
                    if (std::filesystem::exists(fs_path)) {
                        std::cout << "  probe detect_runs/zarr.json: FOUND via filesystem fallback" << std::endl;
                        std::ifstream file(fs_path);
                        if (file) {
                            payload.assign((std::istreambuf_iterator<char>(file)),
                                           std::istreambuf_iterator<char>());
                            have_payload = !payload.empty();
                        }
                        if (!have_payload) {
                            std::cout << "    fallback read: empty payload" << std::endl;
                        }
                    } else {
                        std::cout << "  probe detect_runs/zarr.json: MISSING" << std::endl;
                    }
                }

                if (have_payload) {
                    try {
                        auto json_meta = json::parse(payload);
                        std::string latest_run = "<unset>";
                        if (json_meta.contains("attributes") &&
                            json_meta["attributes"].is_object() &&
                            json_meta["attributes"].contains("latest") &&
                            json_meta["attributes"]["latest"].is_string()) {
                            latest_run = json_meta["attributes"]["latest"].get<std::string>();
                        }
                        std::cout << "    latest=" << latest_run << std::endl;
                        if (latest_run != "<unset>") {
                            std::string base = "detect_runs/" + latest_run + "/";
                            auto fi = ts::kvstore::Read(store, base + "frame_indices/zarr.json").result();
                            auto boxes = ts::kvstore::Read(store, base + "bbox_norm_coords/zarr.json").result();
                            std::cout << "    " << base << "frame_indices/zarr.json: "
                                      << (fi.ok() && fi.value().has_value() ? "FOUND" : "MISSING") << std::endl;
                            std::cout << "    " << base << "bbox_norm_coords/zarr.json: "
                                      << (boxes.ok() && boxes.value().has_value() ? "FOUND" : "MISSING") << std::endl;
                        }
                    } catch (const std::exception& e) {
                        std::cout << "    parse error: " << e.what() << std::endl;
                    }
                }
            } else {
                std::cout << "  probe detect_runs/zarr.json: ERROR "
                          << probe.status().ToString() << std::endl;
            }
        }

        bool loaded_palette_layout = false;

        try {
            loaded_palette_layout = loadDetectionRuns(store);
        } catch (const std::exception& palette_error) {
            error_message = std::string("Failed to load detect_runs layout: ") +
                            palette_error.what();
            return false;
        }

        if (!loaded_palette_layout) {
            error_message = "detect_runs layout not found; legacy dense bboxes are no longer supported";
            return false;
        }

        if (!loadRefinedDetectionsAsPrimary(store)) {
            std::cout << "  Refined detect runs not found or could not be loaded; "
                      << "using base detect_runs data" << std::endl;
        }

        if (loadKeypointHeadingData(store)) {
            std::cout << "  Loaded keypoint headings from '"
                      << data_.keypoints_run_name << "'" << std::endl;
        } else {
            std::cout << "  No keypoint heading data available" << std::endl;
        }

        // Populate metadata; continue even if it fails (broken archives may lack it)
        if (!loadMetadata(store)) {
            std::cout << "  Warning: Could not read raw_video metadata" << std::endl;
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
                if (!data_.latest_interpolation.uses_palette_layout &&
                    data_.latest_interpolation.bboxes_store.valid()) {
                    auto interp_domain = data_.latest_interpolation.bboxes_store.domain();
                    if (interp_domain.rank() > 0 &&
                        interp_domain.shape()[0] != data_.total_frames) {
                        std::cerr << "  Warning: Interpolation frame count mismatch. "
                                  << "Expected " << data_.total_frames 
                                  << " but got " << interp_domain.shape()[0] << std::endl;
                        data_.has_interpolation = false;
                    }
                } else if (data_.latest_interpolation.uses_palette_layout &&
                           !data_.latest_interpolation.frame_offsets.empty()) {
                    size_t palette_frames =
                        data_.latest_interpolation.frame_offsets.size() > 0
                            ? data_.latest_interpolation.frame_offsets.size() - 1
                            : 0;
                    if (palette_frames != data_.total_frames) {
                        std::cerr << "  Warning: Interpolation frame count mismatch. "
                                  << "Expected " << data_.total_frames
                                  << " but got " << palette_frames << std::endl;
                        data_.has_interpolation = false;
                    }
                }
            }
            computeActiveDatasetInterpolationFlags();
        } else {
            std::cout << "  No interpolation data available in 'interpolation_runs'" << std::endl;
        }

        if (loadMovementData(store)) {
            size_t dataset_count = getMovementSeriesCount();
            size_t selected_index = getSelectedMovementSeriesIndex();
            const auto* series = getMovementSeries(selected_index);
            if (series) {
                std::cout << "  Loaded " << dataset_count << " movement dataset"
                          << (dataset_count == 1 ? "" : "s") << "; default '"
                          << series->category << "/" << series->run_name
                          << "' (track " << series->track_id << ")" << std::endl;
            } else {
                std::cout << "  Loaded movement analysis data" << std::endl;
            }
        } else {
            std::cout << "  No movement analysis data available" << std::endl;
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
        active_dataset_ = DetectionDataset::RawDetect;
        return false;
    }
}

bool ZarrDetectionLoader::readInt32Array(const ts::kvstore::KvStore& store,
                                        const std::string& path,
                                        std::vector<int32_t>& out) {
    try {
        std::vector<std::string> failure_messages;
        auto attempt = [&](auto type_token, const char* label) -> bool {
            using Source = decltype(type_token);
            auto open_result = openArrayAny<Source, 1>(store, path, context_);
            if (!open_result.ok()) {
                std::ostringstream oss;
                oss << "open as " << label << " failed: " << open_result.status();
                failure_messages.push_back(oss.str());
                return false;
            }

            auto array_result = ts::Read(open_result.value()).result();
            if (!array_result.ok()) {
                std::ostringstream oss;
                oss << "read as " << label << " failed: " << array_result.status();
                failure_messages.push_back(oss.str());
                return false;
            }

            auto array = array_result.value();
            if (array.rank() != 1) {
                std::ostringstream oss;
                oss << "rank mismatch for " << label << " (expected 1, got " << array.rank() << ")";
                failure_messages.push_back(oss.str());
                return false;
            }

            size_t length = static_cast<size_t>(array.shape()[0]);
            out.resize(length);
            const auto* data = static_cast<const Source*>(array.data());
            bool overflow = false;
            for (size_t i = 0; i < length; ++i) {
                out[i] = convertToInt32WithClamp<Source>(data[i], overflow);
            }
        if (kChaserDebugLoggingEnabled) {
            std::cout << "  [ReadInt32Array] '" << path << "' read as " << label
                      << " (" << length << " rows"
                      << (overflow ? ", overflow clamp applied" : "") << ")"
                      << std::endl;
        }
        return true;
        };

        if (attempt(int32_t{}, "int32_t") ||
            attempt(uint32_t{}, "uint32_t") ||
            attempt(int64_t{}, "int64_t") ||
            attempt(uint64_t{}, "uint64_t") ||
            attempt(int16_t{}, "int16_t") ||
            attempt(uint16_t{}, "uint16_t") ||
            attempt(int8_t{}, "int8_t") ||
            attempt(uint8_t{}, "uint8_t")) {
            return true;
        }

        if (kChaserDebugLoggingEnabled) {
            std::cout << "  [ReadInt32Array] '" << path << "' failed after dtype attempts:"
                      << std::endl;
            for (const auto& msg : failure_messages) {
                std::cout << "    - " << msg << std::endl;
            }
        }
        return false;
    } catch (const std::exception& e) {
        std::cerr << "Error reading int32 array at " << path << ": " << e.what() << std::endl;
        return false;
    }
}

bool ZarrDetectionLoader::readInt64Array(const ts::kvstore::KvStore& store,
                                        const std::string& path,
                                        std::vector<int64_t>& out) {
    try {
        std::vector<std::string> failure_messages;
        auto attempt = [&](auto type_token, const char* label) -> bool {
            using Source = decltype(type_token);
            auto open_result = openArrayAny<Source, 1>(store, path, context_);
            if (!open_result.ok()) {
                std::ostringstream oss;
                oss << "open as " << label << " failed: " << open_result.status();
                failure_messages.push_back(oss.str());
                return false;
            }

            auto array_result = ts::Read(open_result.value()).result();
            if (!array_result.ok()) {
                std::ostringstream oss;
                oss << "read as " << label << " failed: " << array_result.status();
                failure_messages.push_back(oss.str());
                return false;
            }

            auto array = array_result.value();
            if (array.rank() != 1) {
                std::ostringstream oss;
                oss << "rank mismatch for " << label << " (expected 1, got " << array.rank() << ")";
                failure_messages.push_back(oss.str());
                return false;
            }

            size_t length = static_cast<size_t>(array.shape()[0]);
            out.resize(length);
            const auto* data = static_cast<const Source*>(array.data());
            bool overflow = false;
            for (size_t i = 0; i < length; ++i) {
                out[i] = convertToInt64WithClamp<Source>(data[i], overflow);
            }
        if (kChaserDebugLoggingEnabled) {
            std::cout << "  [ReadInt64Array] '" << path << "' read as " << label
                      << " (" << length << " rows"
                      << (overflow ? ", overflow clamp applied" : "") << ")"
                      << std::endl;
        }
            return true;
        };

        if (attempt(int64_t{}, "int64_t") ||
            attempt(uint64_t{}, "uint64_t") ||
            attempt(int32_t{}, "int32_t") ||
            attempt(uint32_t{}, "uint32_t") ||
            attempt(int16_t{}, "int16_t") ||
            attempt(uint16_t{}, "uint16_t") ||
            attempt(int8_t{}, "int8_t") ||
            attempt(uint8_t{}, "uint8_t")) {
            return true;
        }

        if (kChaserDebugLoggingEnabled) {
            std::cout << "  [ReadInt64Array] '" << path << "' failed after dtype attempts:"
                      << std::endl;
            for (const auto& msg : failure_messages) {
                std::cout << "    - " << msg << std::endl;
            }
        }
        return false;
    } catch (const std::exception& e) {
        std::cerr << "Error reading int64 array at " << path << ": " << e.what() << std::endl;
        return false;
    }
}

bool ZarrDetectionLoader::readFloatArray(const ts::kvstore::KvStore& store,
                                        const std::string& path,
                                        std::vector<float>& out) {
    try {
        auto open_result = openArrayAny<float, 1>(store, path, context_);

        if (open_result.ok()) {
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
        }

        // Fallback for float64 datasets
        auto open_double = openArrayAny<double, 1>(store, path, context_);

        if (!open_double.ok()) {
            return false;
        }

        auto array_double = ts::Read(open_double.value()).result();
        if (!array_double.ok()) {
            return false;
        }

        auto array = array_double.value();
        if (array.rank() != 1) {
            return false;
        }

        size_t length = static_cast<size_t>(array.shape()[0]);
        out.resize(length);
        const auto* data = static_cast<const double*>(array.data());
        for (size_t i = 0; i < length; ++i) {
            out[i] = static_cast<float>(data[i]);
        }
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
        auto open_result = openArrayAny<float, 2>(store, path, context_);

        if (open_result.ok()) {
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
        }

        // Fallback for float64 datasets
        auto open_double = openArrayAny<double, 2>(store, path, context_);

        if (!open_double.ok()) {
            return false;
        }

        auto array_result = ts::Read(open_double.value()).result();
        if (!array_result.ok()) {
            return false;
        }

        auto array = array_result.value();
        if (array.rank() != 2 || array.shape()[1] != 4) {
            return false;
        }

        size_t rows = static_cast<size_t>(array.shape()[0]);
        out.resize(rows);
        const auto* data = static_cast<const double*>(array.data());
        for (size_t row = 0; row < rows; ++row) {
            out[row][0] = static_cast<float>(data[row * 4 + 0]);
            out[row][1] = static_cast<float>(data[row * 4 + 1]);
            out[row][2] = static_cast<float>(data[row * 4 + 2]);
            out[row][3] = static_cast<float>(data[row * 4 + 3]);
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
        auto open_result = openArrayAny<bool, 1>(store, path, context_);

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

bool ZarrDetectionLoader::readStringArray(const ts::kvstore::KvStore& store,
                                         const std::string& path,
                                         std::vector<std::string>& out) {
    try {
        std::vector<std::string> failure_messages;

        auto read_as_strings = [&]() -> bool {
            auto open_result = openArrayAny<std::string, 1>(store, path, context_);
            if (!open_result.ok()) {
                std::ostringstream oss;
                oss << "open as std::string failed: " << open_result.status();
                failure_messages.push_back(oss.str());
                return false;
            }

            auto array_result = ts::Read(open_result.value()).result();
            if (!array_result.ok()) {
                std::ostringstream oss;
                oss << "read as std::string failed: " << array_result.status();
                failure_messages.push_back(oss.str());
                return false;
            }

            auto array = array_result.value();
            if (array.rank() != 1) {
                std::ostringstream oss;
                oss << "rank mismatch for std::string (expected 1, got " << array.rank() << ")";
                failure_messages.push_back(oss.str());
                return false;
            }

            size_t length = static_cast<size_t>(array.shape()[0]);
            out.resize(length);
            for (size_t i = 0; i < length; ++i) {
                out[i] = array(static_cast<ts::Index>(i));
            }
            std::cout << "  [ReadStringArray] '" << path << "' read as std::string ("
                      << length << " rows)" << std::endl;
            return true;
        };

        auto read_as_bytes2d = [&](auto type_token, const char* label) -> bool {
            using Element = decltype(type_token);
            auto open_result = openArrayAny<Element, 2>(store, path, context_);
            if (!open_result.ok()) {
                std::ostringstream oss;
                oss << "open as " << label << " failed: " << open_result.status();
                failure_messages.push_back(oss.str());
                return false;
            }

            auto array_result = ts::Read(open_result.value()).result();
            if (!array_result.ok()) {
                std::ostringstream oss;
                oss << "read as " << label << " failed: " << array_result.status();
                failure_messages.push_back(oss.str());
                return false;
            }

            auto array = array_result.value();
            if (array.rank() != 2) {
                std::ostringstream oss;
                oss << "rank mismatch for " << label << " (expected 2, got " << array.rank() << ")";
                failure_messages.push_back(oss.str());
                return false;
            }

            size_t rows = static_cast<size_t>(array.shape()[0]);
            size_t width = static_cast<size_t>(array.shape()[1]);
            out.resize(rows);
            const auto* data = static_cast<const Element*>(array.data());
            for (size_t row = 0; row < rows; ++row) {
                const auto* row_ptr = data + row * width;
                size_t length = 0;
                while (length < width && row_ptr[length] != static_cast<Element>(0)) {
                    ++length;
                }
                out[row] = std::string(reinterpret_cast<const char*>(row_ptr),
                                       reinterpret_cast<const char*>(row_ptr + length));
            }
            std::cout << "  [ReadStringArray] '" << path << "' read as " << label
                      << " (" << rows << " rows, width " << width << ")" << std::endl;
            return true;
        };

        if (read_as_strings() ||
            read_as_bytes2d(uint8_t{}, "uint8_t[rows, width]") ||
            read_as_bytes2d(char{}, "char[rows, width]")) {
            return true;
        }

        std::cout << "  [ReadStringArray] '" << path << "' failed after dtype attempts:"
                  << std::endl;
        for (const auto& msg : failure_messages) {
            std::cout << "    - " << msg << std::endl;
        }
        return false;

    } catch (const std::exception& e) {
        std::cerr << "Error reading string array at " << path << ": " << e.what() << std::endl;
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
        std::string run_prefix = "refined_runs/" + run_name;
        if (auto run_attrs = readAttrsAny(store, run_prefix)) {
            if (run_attrs->contains("created_at") && (*run_attrs)["created_at"].is_string()) {
                interp.created_at = (*run_attrs)["created_at"].get<std::string>();
            }
            if (run_attrs->contains("completed_at_utc") && (*run_attrs)["completed_at_utc"].is_string()) {
                interp.created_at = (*run_attrs)["completed_at_utc"].get<std::string>();
            }
            if (run_attrs->contains("method") && (*run_attrs)["method"].is_string()) {
                interp.method = (*run_attrs)["method"].get<std::string>();
            }
        }

        std::string base_path = run_prefix + "/" + subgroup + "/";
        if (auto subgroup_attrs = readAttrsAny(store, base_path)) {
            if (subgroup_attrs->contains("created_at") && (*subgroup_attrs)["created_at"].is_string()) {
                interp.created_at = (*subgroup_attrs)["created_at"].get<std::string>();
            }
            if (subgroup_attrs->contains("method") && (*subgroup_attrs)["method"].is_string()) {
                interp.method = (*subgroup_attrs)["method"].get<std::string>();
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
    const std::string kPaletteGroup = "detect_runs";

    if (loadDetectionRunFromGroup(store, kPaletteGroup)) {
        data_.layout = ZarrLayoutType::kPaletteRuns;
        data_.coordinates_normalized = true;
        return true;
    }
    return false;
}

bool ZarrDetectionLoader::loadDetectionRunFromGroup(
    const ts::kvstore::KvStore& store,
    const std::string& group_path) {

    std::string latest_run;
    if (auto group_attrs = readAttrsAny(store, group_path)) {
        latest_run = extractLatestRunName(*group_attrs);
    }

    if (latest_run.empty()) {
        if (root_path_.empty()) {
            return false;
        }
        auto candidates = collect_runs_fs(
            root_path_,
            group_path,
            {"frame_indices", "bbox_norm_coords"});
        candidates.erase(
            std::remove_if(
                candidates.begin(),
                candidates.end(),
                [](const std::string& name) {
                    return name.rfind("detect_", 0) != 0 &&
                           name.rfind("detection_", 0) != 0;
                }),
            candidates.end());
        if (candidates.empty()) {
            return false;
        }
        latest_run = candidates.back();
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
    data_.coordinates_normalized = true;
    data_.detection_source_flags.clear();

    if (data_.n_detections.empty()) {
        computeDetectionsFromOffsets(data_.frame_offsets, data_.n_detections);
    }

    data_.total_frames = data_.n_detections.size();
    if (data_.total_frames == 0 && data_.frame_offsets.size() > 1) {
        data_.total_frames = data_.frame_offsets.size() - 1;
    }

    data_.max_detections = 0;
    if (!data_.frame_offsets.empty()) {
        for (size_t frame = 0; frame + 1 < data_.frame_offsets.size(); ++frame) {
            size_t count = data_.frame_offsets[frame + 1] - data_.frame_offsets[frame];
            data_.max_detections = std::max(data_.max_detections, count);
        }
    }
    for (const auto count : data_.n_detections) {
        if (count >= 0) {
            data_.max_detections = std::max(
                data_.max_detections,
                static_cast<size_t>(count)
            );
        }
    }

    data_.detect_run_name = latest_run;

    if (auto attrs = readAttrsAny(store, base_path)) {
        populateDetectionMetadata(*attrs, data_);
    }

    InterpolationRunData raw_stage;
    raw_stage.run_name = latest_run;
    raw_stage.stage_label.clear();
    raw_stage.method = data_.detect_run_method.empty() ? "detect_runs" : data_.detect_run_method;
    raw_stage.created_at = data_.detect_run_created_at;
    raw_stage.source_detection_run = data_.detect_run_source;
    raw_stage.provenance_json = data_.detect_run_provenance_json;
    raw_stage.uses_palette_layout = true;
    raw_stage.has_flat_detections = true;
    raw_stage.frame_indices = data_.frame_indices;
    raw_stage.bbox_norm_coords = data_.bbox_norm_coords;
    raw_stage.flat_scores = data_.flat_scores;
    raw_stage.flat_class_ids = data_.flat_class_ids;
    raw_stage.frame_offsets = data_.frame_offsets;
    raw_stage.n_detections = data_.n_detections;
    raw_stage.has_scores = data_.has_scores;
    raw_stage.has_class_ids = data_.has_class_ids;
    cacheDetectionStage(std::move(raw_stage), DetectionDataset::RawDetect);
    computeActiveDatasetInterpolationFlags();
    active_dataset_ = DetectionDataset::RawDetect;

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
    bool has_frame_indices = readInt32Array(store, base_path + "frame_indices", frame_indices_local);

    std::vector<std::array<float, 4>> boxes_local;
    if (!readFloatMatrix(store, base_path + "bbox_norm_coords", boxes_local)) {
        if (!readFloatMatrix(store, base_path + "bboxes", boxes_local)) {
            std::cerr << "  No bounding box array found under " << base_path << std::endl;
            return false;
        }
    }

    size_t total_detections = boxes_local.size();
    if (total_detections == 0) {
        std::cerr << "  No detections available under " << base_path << std::endl;
    }

    std::vector<int32_t> n_detections_disk;
    readInt32Array(store, base_path + "n_detections", n_detections_disk);

    if (!has_frame_indices && n_detections_disk.empty()) {
        std::cerr << "  Missing both frame_indices and n_detections for " << base_path << std::endl;
        return false;
    }

    if (has_frame_indices && frame_indices_local.size() != total_detections) {
        std::cerr << "  frame_indices length mismatch at " << base_path << std::endl;
        return false;
    }

    scores_out.clear();
    if (readFloatArray(store, base_path + "scores", scores_out) &&
        scores_out.size() == total_detections) {
        has_scores_out = true;
    } else {
        scores_out.clear();
        has_scores_out = false;
    }

    class_ids_out.clear();
    if (readInt32Array(store, base_path + "class_ids", class_ids_out) &&
        class_ids_out.size() == total_detections) {
        has_class_ids_out = true;
    } else {
        class_ids_out.clear();
        has_class_ids_out = false;
    }

    if (detection_source_out) {
        std::vector<int32_t> detection_source_raw;
        if (readInt32Array(store, base_path + "detection_source", detection_source_raw) &&
            detection_source_raw.size() == total_detections) {
            detection_source_out->assign(detection_source_raw.size(), 0);
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

bool ZarrDetectionLoader::loadRefinedDetectionsAsPrimary(const ts::kvstore::KvStore& store) {
    data_.has_refined_filtered_dataset = false;
    data_.has_refined_interpolated_dataset = false;
    data_.has_refined_root_dataset = false;

    auto group_attrs = readAttrsAny(store, "refined_detect_runs");

    std::string latest_run;
    if (group_attrs.has_value()) {
        latest_run = extractLatestRunName(*group_attrs);
    }

    if (latest_run.empty()) {
        if (root_path_.empty()) {
            return false;
        }
        auto filtered_candidates = collect_runs_fs(
            root_path_,
            "refined_detect_runs",
            {"filtered/frame_indices", "filtered/bbox_norm_coords"});
        auto refined_candidates = collect_runs_fs(
            root_path_,
            "refined_detect_runs",
            {"refined/frame_indices", "refined/bbox_norm_coords"});
        auto root_candidates = collect_runs_fs(
            root_path_,
            "refined_detect_runs",
            {"frame_indices", "bbox_norm_coords"});

        std::vector<std::string> candidates;
        candidates.insert(candidates.end(),
                          filtered_candidates.begin(), filtered_candidates.end());
        candidates.insert(candidates.end(),
                          refined_candidates.begin(), refined_candidates.end());
        candidates.insert(candidates.end(),
                          root_candidates.begin(), root_candidates.end());

        candidates.erase(
            std::remove_if(
                candidates.begin(),
                candidates.end(),
                [](const std::string& name) {
                    return name.rfind("refined_detect_", 0) != 0;
                }),
            candidates.end());

        if (candidates.empty()) {
            return false;
        }
        std::sort(candidates.begin(), candidates.end());
        latest_run = candidates.back();
    }

    if (latest_run.empty()) {
        return false;
    }

    struct StageOption {
        const char* subdir;
        const char* label;
        DetectionDataset dataset_type;
    };
    const StageOption kStageOptions[] = {
        {"interpolated/", "interpolated", DetectionDataset::RefinedInterpolated},
        {"filtered/", "filtered", DetectionDataset::RefinedFiltered},
        {"refined/", "refined", DetectionDataset::RefinedRoot},
        {"", "root", DetectionDataset::RefinedRoot}
    };

    std::string run_base = "refined_detect_runs/" + latest_run + "/";
    std::optional<json> run_attrs = readAttrsAny(store, run_base);

    InterpolationRunData stage_template;
    stage_template.run_name = latest_run;
    stage_template.uses_palette_layout = true;
    stage_template.has_flat_detections = true;
    if (run_attrs) {
        if ((*run_attrs).contains("method") && (*run_attrs)["method"].is_string()) {
            stage_template.method = (*run_attrs)["method"].get<std::string>();
        }
        if ((*run_attrs).contains("created_at") && (*run_attrs)["created_at"].is_string()) {
            stage_template.created_at = (*run_attrs)["created_at"].get<std::string>();
        } else if ((*run_attrs).contains("created_at_utc") && (*run_attrs)["created_at_utc"].is_string()) {
            stage_template.created_at = (*run_attrs)["created_at_utc"].get<std::string>();
        }
        if ((*run_attrs).contains("source_detection_run") && (*run_attrs)["source_detection_run"].is_string()) {
            stage_template.source_detection_run = (*run_attrs)["source_detection_run"].get<std::string>();
        }
        if ((*run_attrs).contains("provenance_json") && (*run_attrs)["provenance_json"].is_string()) {
            stage_template.provenance_json = (*run_attrs)["provenance_json"].get<std::string>();
        }
    }

    bool loaded_any = false;

    for (const auto& stage : kStageOptions) {
        std::string base_path = run_base + stage.subdir;
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

        InterpolationRunData stage_data = stage_template;
        stage_data.stage_label = stage.label;
        if (stage_data.method.empty()) {
            stage_data.method = stage.label;
        }
        stage_data.frame_indices = std::move(frame_indices);
        stage_data.bbox_norm_coords = std::move(boxes);
        stage_data.flat_scores = std::move(scores);
        stage_data.flat_class_ids = std::move(class_ids);
        stage_data.frame_offsets = std::move(frame_offsets);
        stage_data.n_detections = std::move(n_detections);
        stage_data.detection_source = std::move(detection_source);
        stage_data.has_scores = has_scores;
        stage_data.has_class_ids = has_class_ids;
        cacheDetectionStage(std::move(stage_data), stage.dataset_type);
        loaded_any = true;

        const auto& stored = (stage.dataset_type == DetectionDataset::RefinedInterpolated)
                                 ? data_.refined_interpolated_dataset
                                 : (stage.dataset_type == DetectionDataset::RefinedFiltered)
                                       ? data_.refined_filtered_dataset
                                       : data_.refined_root_dataset;
        std::cout << "  Refined detect run '" << latest_run << "' stage '"
                  << stage.label << "' loaded (" << stored.bbox_norm_coords.size()
                  << " detections)" << std::endl;
    }

    if (!loaded_any) {
        return false;
    }

    if (data_.has_refined_interpolated_dataset &&
        applyDetectionDataset(data_.refined_interpolated_dataset,
                              DetectionDataset::RefinedInterpolated)) {
        std::cout << "  Using refined detect run '" << data_.detect_run_name
                  << "' as primary detections (interpolated)" << std::endl;
        return true;
    }

    if (data_.has_refined_filtered_dataset &&
        applyDetectionDataset(data_.refined_filtered_dataset,
                              DetectionDataset::RefinedFiltered)) {
        std::cout << "  Using refined detect run '" << data_.detect_run_name
                  << "' as primary detections (filtered)" << std::endl;
        return true;
    }

    if (data_.has_refined_root_dataset &&
        applyDetectionDataset(data_.refined_root_dataset,
                              DetectionDataset::RefinedRoot)) {
        std::cout << "  Using refined detect run '" << data_.detect_run_name
                  << "' as primary detections (refined)" << std::endl;
        return true;
    }

    return false;
}

bool ZarrDetectionLoader::loadRefinedDetectRuns(const ts::kvstore::KvStore& store) {
    auto group_attrs = readAttrsAny(store, "refined_detect_runs");

    std::string latest_run;
    if (group_attrs.has_value()) {
        latest_run = extractLatestRunName(*group_attrs);
    }

    if (latest_run.empty()) {
        if (root_path_.empty()) {
            return false;
        }
        auto candidates = collect_runs_fs(
            root_path_,
            "refined_detect_runs",
            {"interpolated/frame_indices", "interpolated/bbox_norm_coords"});
        if (candidates.empty()) {
            auto filtered = collect_runs_fs(
                root_path_,
                "refined_detect_runs",
                {"filtered/frame_indices", "filtered/bbox_norm_coords"});
            candidates.insert(candidates.end(), filtered.begin(), filtered.end());
        }
        candidates.erase(
            std::remove_if(
                candidates.begin(),
                candidates.end(),
                [](const std::string& name) {
                    return name.rfind("refined_detect_", 0) != 0;
                }),
            candidates.end());
        std::sort(candidates.begin(), candidates.end());
        if (candidates.empty()) {
            return false;
        }
        latest_run = candidates.back();
    }

    std::string run_base = "refined_detect_runs/" + latest_run + "/";
    std::cout << "  Loading refined detection run '" << latest_run << "'" << std::endl;

    InterpolationRunData refined;
    refined.run_name = latest_run;
    refined.uses_palette_layout = true;
    refined.has_flat_detections = false;

    if (auto run_attrs = readAttrsAny(store, run_base)) {
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

bool ZarrDetectionLoader::loadKeypointHeadingData(const ts::kvstore::KvStore& store) {
    data_.flat_headings_deg.clear();
    data_.flat_swim_bladder_px.clear();
    data_.flat_heading_valid.clear();
    data_.has_heading_data = false;
    data_.keypoints_run_name.clear();
    data_.keypoints_source_crop_run.clear();
    data_.flat_keypoints_px.clear();
    data_.keypoint_labels.clear();
    data_.keypoints_per_detection = 0;
    data_.has_keypoints = false;
    data_.mask_roi_indices.clear();
    data_.roi_offset_x.clear();
    data_.roi_offset_y.clear();
    data_.roi_width_px.clear();
    data_.roi_height_px.clear();
    data_.eye_mask_feret_axes_major.clear();
    data_.eye_mask_feret_axes_minor.clear();
    data_.eye_masks_have_feret_axes = false;
    data_.has_eye_masks = false;
    data_.eye_angle_run_name.clear();
    data_.eye_angle_frame_indices.clear();
    data_.eye_angle_valid_mask.clear();
    data_.eye_angle_left_deg.clear();
    data_.eye_angle_right_deg.clear();
    data_.eye_angle_indices_by_frame.clear();
    data_.has_eye_angles = false;
    data_.eye_masks_loaded = false;
    data_.eye_masks_run_name.clear();
    data_.eye_masks_store = ts::TensorStore<uint8_t, 4>();
    data_.eye_mask_roi_count = 0;
    data_.eye_mask_height = 0;
    data_.eye_mask_width = 0;

    if (data_.layout != ZarrLayoutType::kPaletteRuns) {
        return false;
    }

    if (data_.frame_offsets.size() < 2) {
        return false;
    }

    const size_t total_detections = data_.bbox_norm_coords.size();
    if (total_detections == 0) {
        return false;
    }

    std::string latest_run;
    if (auto group_attrs = readAttrsAny(store, "keypoints_runs")) {
        latest_run = extractLatestRunName(*group_attrs);
    }

    if (latest_run.empty()) {
        if (root_path_.empty()) {
            return false;
        }
        auto candidates = collect_runs_fs(
            root_path_,
            "keypoints_runs",
            {"frame_indices", "keypoints_roi", "heading"});
        if (candidates.empty()) {
            return false;
        }
        latest_run = candidates.back();
    }

    std::string run_base = "keypoints_runs/" + latest_run + "/";

    std::vector<int32_t> kp_frame_indices;
    if (!readInt32Array(store, run_base + "frame_indices", kp_frame_indices)) {
        return false;
    }
    if (kp_frame_indices.empty()) {
        return false;
    }
    const size_t roi_count = kp_frame_indices.size();

    std::vector<float> heading_values;
    if (!readFloatArray(store, run_base + "heading", heading_values)) {
        heading_values.assign(roi_count, 0.0f);
    } else if (heading_values.size() != roi_count) {
        heading_values.resize(roi_count, 0.0f);
    }

    std::vector<uint8_t> detection_success;
    if (!readBoolArray(store, run_base + "detection_success", detection_success) ||
        detection_success.size() != roi_count) {
        detection_success.assign(roi_count, 1);
    }

    size_t num_keypoints = 0;
    size_t coord_dim = 0;
    std::vector<float> kp_values;
    enum class KeypointSpace {
        kImage,
        kRoi,
        kNormalized
    };
    KeypointSpace keypoint_space = KeypointSpace::kImage;

    auto load_keypoints_dataset = [&](const std::string& dataset_path,
                                      KeypointSpace space) -> bool {
        auto store_float = openArrayAny<float, 3>(store, dataset_path, context_);
        if (store_float.ok()) {
            auto array_result = ts::Read(store_float.value()).result();
            if (!array_result.ok()) {
                return false;
            }
            auto array = array_result.value();
            auto shape = array.shape();
            if (shape.size() != 3 ||
                shape[0] != static_cast<tensorstore::Index>(roi_count)) {
                return false;
            }
            num_keypoints = static_cast<size_t>(shape[1]);
            coord_dim = static_cast<size_t>(shape[2]);
            if (num_keypoints == 0 || coord_dim < 2) {
                return false;
            }
            size_t total_values = roi_count * num_keypoints * coord_dim;
            kp_values.resize(total_values);
            const float* data_ptr = static_cast<const float*>(array.data());
            std::copy(data_ptr, data_ptr + total_values, kp_values.begin());
            keypoint_space = space;
            return true;
        }

        auto store_double = openArrayAny<double, 3>(store, dataset_path, context_);
        if (!store_double.ok()) {
            return false;
        }
        auto array_result = ts::Read(store_double.value()).result();
        if (!array_result.ok()) {
            return false;
        }
        auto array = array_result.value();
        auto shape = array.shape();
        if (shape.size() != 3 ||
            shape[0] != static_cast<tensorstore::Index>(roi_count)) {
            return false;
        }
        num_keypoints = static_cast<size_t>(shape[1]);
        coord_dim = static_cast<size_t>(shape[2]);
        if (num_keypoints == 0 || coord_dim < 2) {
            return false;
        }
        size_t total_values = roi_count * num_keypoints * coord_dim;
        kp_values.resize(total_values);
        const double* data_ptr = static_cast<const double*>(array.data());
        for (size_t i = 0; i < total_values; ++i) {
            kp_values[i] = static_cast<float>(data_ptr[i]);
        }
        keypoint_space = space;
        return true;
    };

    if (!load_keypoints_dataset(run_base + "keypoints_img", KeypointSpace::kImage) &&
        !load_keypoints_dataset(run_base + "keypoints_roi", KeypointSpace::kRoi) &&
        !load_keypoints_dataset(run_base + "keypoints_norm", KeypointSpace::kNormalized)) {
        return false;
    }

    auto run_attrs = readAttrsAny(store, run_base);
    size_t swim_index = 0;
    bool swim_label_found = false;
    std::vector<std::string> keypoint_labels_loaded;
    if (run_attrs.has_value() &&
        run_attrs->contains("keypoint_labels") &&
        (*run_attrs)["keypoint_labels"].is_array()) {
        const auto& labels = (*run_attrs)["keypoint_labels"];
        keypoint_labels_loaded.reserve(labels.size());
        for (size_t i = 0; i < labels.size(); ++i) {
            if (!labels[i].is_string()) {
                continue;
            }
            std::string label = labels[i].get<std::string>();
            keypoint_labels_loaded.push_back(label);
            if (!swim_label_found) {
                std::string lowered = label;
                std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                               [](unsigned char c) {
                                   return static_cast<char>(std::tolower(c));
                               });
                if (lowered.find("bladder") != std::string::npos ||
                    lowered.find("swim") != std::string::npos) {
                    swim_index = keypoint_labels_loaded.size() - 1;
                    swim_label_found = true;
                }
            }
        }
    }

    if (num_keypoints > 0) {
        data_.keypoint_labels.clear();
        if (!keypoint_labels_loaded.empty()) {
            if (keypoint_labels_loaded.size() == num_keypoints) {
                data_.keypoint_labels = keypoint_labels_loaded;
            } else {
                data_.keypoint_labels.reserve(num_keypoints);
                for (size_t i = 0; i < num_keypoints; ++i) {
                    if (i < keypoint_labels_loaded.size()) {
                        data_.keypoint_labels.push_back(keypoint_labels_loaded[i]);
                    } else {
                        data_.keypoint_labels.push_back("kp" + std::to_string(i));
                    }
                }
            }
        }
        if (data_.keypoint_labels.empty()) {
            data_.keypoint_labels.reserve(num_keypoints);
            static const std::array<std::string, 3> kDefaultLabels = {
                "swim_bladder", "left_eye", "right_eye"};
            for (size_t i = 0; i < num_keypoints; ++i) {
                if (i < kDefaultLabels.size()) {
                    data_.keypoint_labels.push_back(kDefaultLabels[i]);
                } else {
                    data_.keypoint_labels.push_back("kp" + std::to_string(i));
                }
            }
        }
        if (swim_index >= num_keypoints) {
            swim_index = 0;
        }
    } else {
        data_.keypoint_labels.clear();
        swim_index = 0;
    }

    auto normalizeCropName = [](const std::string& name) -> std::string {
        constexpr std::string_view prefix = "crop_runs/";
        if (name.rfind(prefix.data(), 0) == 0) {
            return name.substr(prefix.size());
        }
        return name;
    };

    std::vector<std::string> crop_candidates;
    auto addCandidate = [&](const std::string& candidate) {
        if (candidate.empty()) {
            return;
        }
        std::string normalized = normalizeCropName(candidate);
        if (std::find(crop_candidates.begin(), crop_candidates.end(), normalized) == crop_candidates.end()) {
            crop_candidates.push_back(normalized);
        }
    };

    if (run_attrs.has_value() && run_attrs->contains("source_crop_run") &&
        (*run_attrs)["source_crop_run"].is_string()) {
        addCandidate((*run_attrs)["source_crop_run"].get<std::string>());
    }

    if (auto crop_group_attrs = readAttrsAny(store, "crop_runs")) {
        std::string latest = extractLatestRunName(*crop_group_attrs);
        if (!latest.empty()) {
            addCandidate(latest);
        }
    }

    if (!root_path_.empty()) {
        auto fs_candidates = collect_runs_fs(
            root_path_,
            "crop_runs",
            {"roi_coordinates_full"});
        for (const auto& name : fs_candidates) {
            addCandidate(name);
        }
    }

    std::vector<float> roi_offsets;
    bool roi_ok = false;
    std::string crop_run;
    float roi_height_px = 0.0f;
    float roi_width_px = 0.0f;
    bool roi_size_available = false;

    auto updateRoiSizeFromAttrs = [&](const std::string& crop_base) {
        if (roi_size_available) {
            return;
        }
        if (auto crop_attrs = readAttrsAny(store, crop_base)) {
            const auto& attrs = *crop_attrs;
            if (attrs.contains("roi_size") && attrs["roi_size"].is_array()) {
                const auto& roi_size_attr = attrs["roi_size"];
                if (roi_size_attr.size() >= 2) {
                    if (roi_size_attr[0].is_number() && roi_size_attr[1].is_number()) {
                        roi_height_px = static_cast<float>(roi_size_attr[0].get<double>());
                        roi_width_px = static_cast<float>(roi_size_attr[1].get<double>());
                        if (roi_height_px > 0.0f && roi_width_px > 0.0f) {
                            roi_size_available = true;
                        }
                    }
                }
            }
        }
        if (!roi_size_available) {
            auto roi_image_store =
                openArrayAny<uint8_t, 3>(store, crop_base + "roi_images", context_);
            if (roi_image_store.ok()) {
                auto domain = roi_image_store.value().domain();
                auto shape = domain.shape();
                if (shape.size() == 3) {
                    float inferred_height = static_cast<float>(shape[1]);
                    float inferred_width = static_cast<float>(shape[2]);
                    if (inferred_height > 0.0f && inferred_width > 0.0f) {
                        roi_height_px = inferred_height;
                        roi_width_px = inferred_width;
                        roi_size_available = true;
                    }
                }
            }
        }
    };

    auto tryLoadCropRun = [&](const std::string& candidate) -> bool {
        if (candidate.empty()) {
            return false;
        }
        std::string crop_base = "crop_runs/" + candidate + "/";
        auto roi_float_store =
            openArrayAny<float, 2>(store, crop_base + "roi_coordinates_full", context_);
        if (roi_float_store.ok()) {
            auto roi_array_result = ts::Read(roi_float_store.value()).result();
            if (roi_array_result.ok()) {
                auto roi_array = roi_array_result.value();
                auto roi_shape = roi_array.shape();
                if (roi_shape.size() == 2 &&
                    roi_shape[0] == static_cast<tensorstore::Index>(roi_count)) {
                    size_t cols = static_cast<size_t>(roi_shape[1]);
                    if (cols >= 2) {
                        roi_offsets.assign(roi_count * 2, 0.0f);
                        const float* roi_ptr = static_cast<const float*>(roi_array.data());
                        for (size_t i = 0; i < roi_count; ++i) {
                            roi_offsets[i * 2 + 0] = roi_ptr[i * cols + 0]; // x / column
                            roi_offsets[i * 2 + 1] = roi_ptr[i * cols + 1]; // y / row
                        }
                        updateRoiSizeFromAttrs(crop_base);
                        return true;
                    }
                }
            }
        }

        auto roi_int_store =
            openArrayAny<int32_t, 2>(store, crop_base + "roi_coordinates_full", context_);
        if (roi_int_store.ok()) {
            auto roi_array_result = ts::Read(roi_int_store.value()).result();
            if (roi_array_result.ok()) {
                auto roi_array = roi_array_result.value();
                auto roi_shape = roi_array.shape();
                if (roi_shape.size() == 2 &&
                    roi_shape[0] == static_cast<tensorstore::Index>(roi_count)) {
                    size_t cols = static_cast<size_t>(roi_shape[1]);
                    if (cols >= 2) {
                        roi_offsets.assign(roi_count * 2, 0.0f);
                        const int32_t* roi_ptr = static_cast<const int32_t*>(roi_array.data());
                        for (size_t i = 0; i < roi_count; ++i) {
                            roi_offsets[i * 2 + 0] = static_cast<float>(roi_ptr[i * cols + 0]);
                            roi_offsets[i * 2 + 1] = static_cast<float>(roi_ptr[i * cols + 1]);
                        }
                        updateRoiSizeFromAttrs(crop_base);
                        return true;
                    }
                }
            }
        }
        return false;
    };

    for (const auto& candidate : crop_candidates) {
        if (tryLoadCropRun(candidate)) {
            crop_run = candidate;
            roi_ok = true;
            break;
        }
    }

    if (!roi_ok && !crop_candidates.empty()) {
        std::cerr << "[HEADING_WARNING] Failed to load ROI offsets from crop_runs for any candidate ("
                  << crop_candidates.size() << " tried)." << std::endl;
    }

    if (!roi_ok) {
        roi_offsets.clear();
    }

    const float nan_value = std::numeric_limits<float>::quiet_NaN();
    data_.flat_headings_deg.assign(total_detections, 0.0f);
    data_.flat_swim_bladder_px.assign(
        total_detections, std::array<float, 2>{nan_value, nan_value});
    data_.flat_heading_valid.assign(total_detections, 0);
    data_.mask_roi_indices.assign(total_detections, -1);
    data_.roi_offset_x.assign(total_detections, nan_value);
    data_.roi_offset_y.assign(total_detections, nan_value);
    data_.roi_width_px.assign(total_detections, 0.0f);
    data_.roi_height_px.assign(total_detections, 0.0f);
    if (num_keypoints > 0) {
        data_.keypoints_per_detection = num_keypoints;
        data_.flat_keypoints_px.assign(total_detections * num_keypoints * 2, nan_value);
        data_.has_keypoints = true;
    } else {
        data_.keypoints_per_detection = 0;
        data_.flat_keypoints_px.clear();
        data_.has_keypoints = false;
    }

    std::vector<size_t> frame_cursor(
        data_.frame_offsets.size() > 0 ? data_.frame_offsets.size() - 1 : 0, 0);
    size_t filled = 0;
    size_t finite_keypoint_count = 0;

    for (size_t roi_index = 0; roi_index < roi_count; ++roi_index) {
        int32_t frame = kp_frame_indices[roi_index];
        if (frame < 0) {
            continue;
        }
        if (static_cast<size_t>(frame) >= frame_cursor.size()) {
            continue;
        }

        size_t start = data_.frame_offsets[frame];
        size_t end = data_.frame_offsets[frame + 1];
        if (start >= end) {
            frame_cursor[frame]++;
            continue;
        }

        size_t offset = frame_cursor[frame];
        if (start + offset >= end) {
            frame_cursor[frame]++;
            continue;
        }

        size_t det_index = start + offset;
        frame_cursor[frame]++;

        data_.mask_roi_indices[det_index] = (roi_ok && roi_offsets.size() >= (roi_index * 2 + 2))
                                                ? static_cast<int32_t>(roi_index)
                                                : -1;

        std::array<float, 4> pixel_box = {nan_value, nan_value, nan_value, nan_value};
        bool have_pixel_box = false;
        if (det_index < data_.bbox_norm_coords.size() &&
            data_.image_width > 0 && data_.image_height > 0) {
            pixel_box = normalizedBoxToPixels(
                data_.bbox_norm_coords[det_index],
                data_.image_width,
                data_.image_height);
            have_pixel_box = true;
        }

        float pixel_box_width = have_pixel_box ? std::max(0.0f, pixel_box[2] - pixel_box[0]) : 0.0f;
        float pixel_box_height = have_pixel_box ? std::max(0.0f, pixel_box[3] - pixel_box[1]) : 0.0f;
        float roi_width_for_det = roi_size_available ? roi_width_px : pixel_box_width;
        float roi_height_for_det = roi_size_available ? roi_height_px : pixel_box_height;

        bool success = detection_success[roi_index] != 0;
        data_.flat_heading_valid[det_index] = success ? 1 : 0;
        data_.flat_headings_deg[det_index] =
            roi_index < heading_values.size() ? heading_values[roi_index] : 0.0f;

        float offset_x_det = nan_value;
        float offset_y_det = nan_value;
        if (roi_ok && roi_offsets.size() >= (roi_index * 2 + 2)) {
            float raw_offset_x = roi_offsets[roi_index * 2 + 0];
            float raw_offset_y = roi_offsets[roi_index * 2 + 1];
            if (std::isfinite(raw_offset_x) && std::isfinite(raw_offset_y)) {
                offset_x_det = raw_offset_x;
                offset_y_det = raw_offset_y;
            }
        }

        bool assigned_anchor = false;
        float assigned_x = nan_value;
        float assigned_y = nan_value;
        float raw_anchor_x = nan_value;
        float raw_anchor_y = nan_value;

        if (!kp_values.empty() && num_keypoints > 0 && coord_dim >= 2) {
            size_t stride = num_keypoints * coord_dim;
            size_t flat_stride = num_keypoints * 2;
            size_t flat_base = det_index * flat_stride;
            bool can_store_flat =
                data_.has_keypoints &&
                (flat_base + flat_stride) <= data_.flat_keypoints_px.size();

            auto convertRawToImage = [&](float raw_x, float raw_y) -> std::array<float, 2> {
                std::array<float, 2> result = {nan_value, nan_value};
                if (!std::isfinite(raw_x) || !std::isfinite(raw_y)) {
                    return result;
                }

                if (keypoint_space == KeypointSpace::kImage) {
                    result[0] = raw_x;
                    result[1] = raw_y;
                    return result;
                }

                float local_x = raw_x;
                float local_y = raw_y;

                if (keypoint_space == KeypointSpace::kNormalized) {
                    float scale_x = roi_width_for_det;
                    float scale_y = roi_height_for_det;
                    bool used_fallback_scale = false;
                    if (scale_x <= 0.0f || scale_y <= 0.0f) {
                        scale_x = pixel_box_width;
                        scale_y = pixel_box_height;
                        used_fallback_scale = true;
                    }
                    if (scale_x <= 0.0f || scale_y <= 0.0f) {
                        return result;
                    }
                    local_x = raw_x * scale_x;
                    local_y = raw_y * scale_y;
                    if (used_fallback_scale) {
                        roi_width_for_det = scale_x;
                        roi_height_for_det = scale_y;
                    }
                }

                float offset_x = offset_x_det;
                float offset_y = offset_y_det;
                bool has_offset = std::isfinite(offset_x) && std::isfinite(offset_y);
                if (!has_offset) {
                    if (std::isfinite(pixel_box[0]) && std::isfinite(pixel_box[1])) {
                        offset_x = pixel_box[0];
                        offset_y = pixel_box[1];
                        has_offset = true;
                    }
                }

                if (keypoint_space == KeypointSpace::kRoi ||
                    keypoint_space == KeypointSpace::kNormalized) {
                    if (!has_offset) {
                        return result;
                    }
                    result[0] = offset_x + local_x;
                    result[1] = offset_y + local_y;
                    return result;
                }

                result[0] = local_x;
                result[1] = local_y;
                return result;
            };

            for (size_t kp_idx = 0; kp_idx < num_keypoints; ++kp_idx) {
                size_t kp_base = roi_index * stride + kp_idx * coord_dim;
                float raw_x = nan_value;
                float raw_y = nan_value;
                if (kp_base + 1 < kp_values.size()) {
                    raw_x = kp_values[kp_base + 0];
                    raw_y = kp_values[kp_base + 1];
                }

                std::array<float, 2> converted = convertRawToImage(raw_x, raw_y);
                if (can_store_flat) {
                    data_.flat_keypoints_px[flat_base + kp_idx * 2 + 0] = converted[0];
                    data_.flat_keypoints_px[flat_base + kp_idx * 2 + 1] = converted[1];
                }
                if (kp_idx == swim_index) {
                    raw_anchor_x = raw_x;
                    raw_anchor_y = raw_y;
                    if (std::isfinite(converted[0]) && std::isfinite(converted[1])) {
                        assigned_x = converted[0];
                        assigned_y = converted[1];
                        assigned_anchor = true;
                    }
                }
                if (can_store_flat && std::isfinite(converted[0]) && std::isfinite(converted[1])) {
                    finite_keypoint_count++;
                }
            }
        }

        if (!assigned_anchor && have_pixel_box) {
            float fallback_x = 0.5f * (pixel_box[0] + pixel_box[2]);
            float fallback_y = 0.5f * (pixel_box[1] + pixel_box[3]);
            if (std::isfinite(fallback_x) && std::isfinite(fallback_y)) {
                assigned_x = fallback_x;
                assigned_y = fallback_y;
                assigned_anchor = true;
            }
        }

        if (assigned_anchor) {
            data_.flat_swim_bladder_px[det_index] = {assigned_x, assigned_y};
        }

        if (std::isfinite(offset_x_det) && std::isfinite(offset_y_det)) {
            data_.roi_offset_x[det_index] = offset_x_det;
            data_.roi_offset_y[det_index] = offset_y_det;
        }

        if (roi_width_for_det > 0.0f && roi_height_for_det > 0.0f) {
            data_.roi_width_px[det_index] = roi_width_for_det;
            data_.roi_height_px[det_index] = roi_height_for_det;
        } else if (have_pixel_box) {
            data_.roi_width_px[det_index] = std::max(0.0f, pixel_box[2] - pixel_box[0]);
            data_.roi_height_px[det_index] = std::max(0.0f, pixel_box[3] - pixel_box[1]);
        }

        constexpr int kMaxDetailLogs = 12;
        static int detail_log_count = 0;
        if (detail_log_count < kMaxDetailLogs && kChaserDebugLoggingEnabled) {
            std::cout << "[HEADING_LOAD_DETAIL] roi=" << roi_index
                      << " frame=" << frame
                      << " det_index=" << det_index
                      << " success=" << static_cast<int>(success)
                      << " raw_kp=(";
            if (std::isfinite(raw_anchor_x) && std::isfinite(raw_anchor_y)) {
                std::cout << raw_anchor_x << ", " << raw_anchor_y << ")";
            } else {
                std::cout << "nan, nan)";
            }
            std::cout << " converted=(";
            if (std::isfinite(assigned_x) && std::isfinite(assigned_y)) {
                std::cout << assigned_x << ", " << assigned_y << ")";
            } else {
                std::cout << "nan, nan)";
            }
            std::cout << " space=";
            switch (keypoint_space) {
                case KeypointSpace::kImage:
                    std::cout << "image";
                    break;
                case KeypointSpace::kRoi:
                    std::cout << "roi";
                    break;
                case KeypointSpace::kNormalized:
                    std::cout << "normalized";
                    break;
            }
            if (std::isfinite(offset_x_det) && std::isfinite(offset_y_det)) {
                std::cout << " roi_offsets=(y=" << offset_y_det << ", x=" << offset_x_det << ")";
            } else {
                std::cout << " roi_offsets=UNAVAILABLE";
            }
            std::cout << " roi_size=(" << roi_height_for_det << ", " << roi_width_for_det << ")";
            if (assigned_anchor) {
                std::cout << " assigned=yes anchor=(" << assigned_x << ", " << assigned_y << ")";
            } else {
                std::cout << " assigned=no anchor=(nan, nan)";
            }
            std::cout << std::endl;
            detail_log_count++;
        }

        filled++;
    }

    data_.has_heading_data = filled > 0;
    if (data_.keypoints_per_detection > 0) {
        data_.has_keypoints = finite_keypoint_count > 0;
        if (!data_.has_keypoints) {
            data_.flat_keypoints_px.clear();
            data_.keypoints_per_detection = 0;
        }
    } else {
        data_.has_keypoints = false;
    }
    if (data_.has_heading_data) {
        data_.keypoints_run_name = latest_run;
        data_.keypoints_source_crop_run = crop_run;

        constexpr size_t kMaxLogEntries = 8;
        if (kChaserDebugLoggingEnabled) {
            for (size_t i = 0; i < std::min(kMaxLogEntries, data_.flat_swim_bladder_px.size()); ++i) {
                const auto& anchor = data_.flat_swim_bladder_px[i];
                const float heading = i < data_.flat_headings_deg.size() ? data_.flat_headings_deg[i] : 0.0f;
                const uint8_t valid = i < data_.flat_heading_valid.size() ? data_.flat_heading_valid[i] : 0;
                std::cout << "[HEADING_LOAD_DEBUG] idx " << i
                          << " anchor=(" << anchor[0] << ", " << anchor[1]
                          << ") heading=" << heading
                          << " valid=" << static_cast<int>(valid)
                          << " finite=" << (std::isfinite(anchor[0]) && std::isfinite(anchor[1]))
                          << std::endl;
            }
        }
    }

    if (!data_.has_eye_masks) {
        loadRefinedEyeMaskData(store, roi_count);
    }
    if (!data_.has_eye_angles) {
        loadEyeAngleData(store, roi_count);
    }

    return data_.has_heading_data;
}

bool ZarrDetectionLoader::loadRefinedEyeMaskData(const ts::kvstore::KvStore& store,
                                                 size_t roi_count) {
    data_.eye_masks_run_name.clear();
    data_.eye_masks_loaded = false;
    data_.has_eye_masks = false;
    data_.eye_masks_store = ts::TensorStore<uint8_t, 4>();
    data_.eye_mask_roi_count = 0;
    data_.eye_mask_height = 0;
    data_.eye_mask_width = 0;
    data_.eye_mask_feret_axes_major.clear();
    data_.eye_mask_feret_axes_minor.clear();
    data_.eye_masks_have_feret_axes = false;

    if (data_.layout != ZarrLayoutType::kPaletteRuns) {
        return false;
    }

    std::string latest_run;
    if (auto group_attrs = readAttrsAny(store, "refined_eye_masks_runs")) {
        latest_run = extractLatestRunName(*group_attrs);
    }

    if (latest_run.empty() && !root_path_.empty()) {
        auto fs_candidates = collect_runs_fs(
            root_path_,
            "refined_eye_masks_runs",
            {"masks_roi"});
        if (!fs_candidates.empty()) {
            latest_run = fs_candidates.back();
        }
    }

    if (latest_run.empty()) {
        return false;
    }

    std::string run_base = "refined_eye_masks_runs/" + latest_run + "/";

    std::vector<int32_t> mask_frame_indices;
    if (!readInt32Array(store, run_base + "frame_indices", mask_frame_indices)) {
        std::cout << "[EYE_MASK_WARNING] refined_eye_masks run '" << latest_run
                  << "' missing frame_indices; skipping mask overlay." << std::endl;
        return false;
    }
    if (mask_frame_indices.size() != roi_count) {
        std::cout << "[EYE_MASK_WARNING] refined_eye_masks run '" << latest_run
                  << "' frame_indices size (" << mask_frame_indices.size()
                  << ") does not match keypoint ROI count (" << roi_count << ")." << std::endl;
        if (mask_frame_indices.empty()) {
            return false;
        }
    }

    auto masks_store_result =
        openArrayAny<uint8_t, 4>(store, run_base + "masks_roi", context_);
    if (!masks_store_result.ok()) {
        std::cout << "[EYE_MASK_WARNING] Failed to open refined eye masks for run '"
                  << latest_run << "': " << masks_store_result.status().ToString() << std::endl;
        return false;
    }

    auto domain = masks_store_result.value().domain();
    auto shape = domain.shape();
    if (shape.size() != 4) {
        std::cout << "[EYE_MASK_WARNING] Unexpected masks_roi rank in run '"
                  << latest_run << "' (expected 4, got " << shape.size() << ")." << std::endl;
        return false;
    }

    size_t roi_dim = static_cast<size_t>(shape[0]);
    size_t channel_dim = static_cast<size_t>(shape[1]);
    size_t mask_rows = static_cast<size_t>(shape[2]);
    size_t mask_cols = static_cast<size_t>(shape[3]);

    if (roi_dim == 0 || channel_dim == 0 || mask_rows == 0 || mask_cols == 0) {
        std::cout << "[EYE_MASK_WARNING] masks_roi dataset '" << latest_run
                  << "' has empty dimensions; skipping." << std::endl;
        return false;
    }

    data_.mask_chunk_cache.clear();
    data_.eye_masks_store = masks_store_result.value();
    data_.eye_masks_run_name = latest_run;
    data_.eye_masks_loaded = true;
    data_.has_eye_masks = true;
    data_.eye_mask_roi_count = roi_dim;
    data_.eye_mask_height = mask_rows;
    data_.eye_mask_width = mask_cols;
    data_.eye_mask_chunk_rows = 0;
    auto chunk_layout_result = data_.eye_masks_store.chunk_layout();
    if (chunk_layout_result.ok()) {
        const auto& chunk_layout = chunk_layout_result.value();
        auto chunk_shape = chunk_layout.read_chunk_shape();
        if (!chunk_shape.empty()) {
            auto chunk_size = chunk_shape[0];
            if (chunk_size > 0) {
                data_.eye_mask_chunk_rows =
                    static_cast<size_t>(chunk_size);
            }
        }
    }
    if (data_.eye_mask_chunk_rows == 0) {
        data_.eye_mask_chunk_rows = std::min<size_t>(roi_dim, 512);
    }

    auto loadFeretAxes = [&](const std::string& dataset_name,
                             std::vector<std::array<std::array<float, 4>, 2>>& target) -> bool {
        target.clear();
        auto axes_store =
            openArrayAny<float, 3>(store, run_base + dataset_name, context_);
        if (!axes_store.ok()) {
            return false;
        }
        auto axes_result = ts::Read(axes_store.value()).result();
        if (!axes_result.ok()) {
            std::cout << "[EYE_MASK_WARNING] Failed to read " << dataset_name
                      << " for run '" << latest_run << "': "
                      << axes_result.status().ToString() << std::endl;
            return false;
        }

        auto axes_array = axes_result.value();
        auto axes_shape = axes_array.shape();
        if (axes_shape.size() != 3 ||
            axes_shape[0] != static_cast<ts::Index>(roi_dim) ||
            axes_shape[1] < 1 || axes_shape[2] < 4) {
            std::cout << "[EYE_MASK_WARNING] Unexpected shape for " << dataset_name
                      << " in run '" << latest_run << "' (expected "
                      << roi_dim << "x2x4, got ";
            for (size_t i = 0; i < axes_shape.size(); ++i) {
                std::cout << axes_shape[i] << (i + 1 < axes_shape.size() ? "x" : "");
            }
            std::cout << ")." << std::endl;
            return false;
        }

        const float nan_value = std::numeric_limits<float>::quiet_NaN();
        target.resize(roi_dim);
        for (auto& roi_entry : target) {
            roi_entry = {std::array<float, 4>{nan_value, nan_value, nan_value, nan_value},
                         std::array<float, 4>{nan_value, nan_value, nan_value, nan_value}};
        }

        const size_t eye_dim = std::min<size_t>(2, static_cast<size_t>(axes_shape[1]));
        const size_t axis_len = std::min<size_t>(4, static_cast<size_t>(axes_shape[2]));

        for (size_t roi = 0; roi < roi_dim; ++roi) {
            for (size_t eye = 0; eye < eye_dim; ++eye) {
                std::array<float, 4> values = {nan_value, nan_value, nan_value, nan_value};
                bool all_finite = true;
                for (size_t idx = 0; idx < axis_len; ++idx) {
                    float value = axes_array( static_cast<ts::Index>(roi),
                                             static_cast<ts::Index>(eye),
                                             static_cast<ts::Index>(idx));
                    values[idx] = value;
                    if (!std::isfinite(value)) {
                        all_finite = false;
                        break;
                    }
                }
                if (!all_finite) {
                    continue;
                }
                float dx = values[0] - values[2];
                float dy = values[1] - values[3];
                if (std::fabs(dx) < 1e-5f && std::fabs(dy) < 1e-5f) {
                    continue;
                }
                target[roi][eye] = values;
            }
        }
        return true;
    };

    bool feret_major_ok =
        loadFeretAxes("feret_axes_major", data_.eye_mask_feret_axes_major);
    bool feret_minor_ok =
        loadFeretAxes("feret_axes_minor", data_.eye_mask_feret_axes_minor);
    data_.eye_masks_have_feret_axes = feret_major_ok && feret_minor_ok;

    std::cout << "  Refined eye mask run '" << latest_run
              << "' loaded (" << channel_dim << " channels, "
              << mask_cols << "x" << mask_rows << " masks)" << std::endl;
    return true;
}

bool ZarrDetectionLoader::loadEyeAngleData(const ts::kvstore::KvStore& store,
                                           size_t roi_count) {
    data_.eye_angle_run_name.clear();
    data_.eye_angle_frame_indices.clear();
    data_.eye_angle_valid_mask.clear();
    data_.eye_angle_left_deg.clear();
    data_.eye_angle_right_deg.clear();
    data_.eye_angle_indices_by_frame.clear();
    data_.has_eye_angles = false;

    std::string latest_run;
    if (auto group_attrs = readAttrsAny(store, "analysis/eye_angle_runs")) {
        latest_run = extractLatestRunName(*group_attrs);
    }

    if (latest_run.empty() && !root_path_.empty()) {
        auto fs_candidates = collect_runs_fs(
            root_path_, "analysis/eye_angle_runs",
            {"angles/roi/left_feret_minor_signed_deg"});
        if (!fs_candidates.empty()) {
            latest_run = fs_candidates.back();
        }
    }

    if (latest_run.empty()) {
        return false;
    }

    std::string base = "analysis/eye_angle_runs/" + latest_run + "/angles/roi/";

    std::vector<float> left_angles;
    if (!readFloatArray(store, base + "left_feret_minor_signed_deg", left_angles)) {
        std::cout << "[EYE_ANGLE_WARNING] Failed to read left_feret_minor_signed_deg for run '"
                  << latest_run << "'" << std::endl;
        return false;
    }
    std::vector<float> right_angles;
    if (!readFloatArray(store, base + "right_feret_minor_signed_deg", right_angles)) {
        std::cout << "[EYE_ANGLE_WARNING] Failed to read right_feret_minor_signed_deg for run '"
                  << latest_run << "'" << std::endl;
        return false;
    }

    if (left_angles.empty() || right_angles.empty()) {
        return false;
    }

    std::vector<int32_t> frame_indices;
    readInt32Array(store, base + "frame_indices", frame_indices);

    std::vector<uint8_t> valid_mask;
    if (!readBoolArray(store, base + "valid_mask", valid_mask)) {
        valid_mask.assign(left_angles.size(), 1);
    }

    size_t count = std::min(left_angles.size(), right_angles.size());
    if (!frame_indices.empty()) {
        count = std::min(count, frame_indices.size());
    }
    if (!valid_mask.empty()) {
        count = std::min(count, valid_mask.size());
    }

    if (count == 0) {
        return false;
    }

    left_angles.resize(count);
    right_angles.resize(count);

    if (frame_indices.empty()) {
        frame_indices.assign(count, -1);
    } else if (frame_indices.size() != count) {
        frame_indices.resize(count, -1);
    }

    if (valid_mask.empty()) {
        valid_mask.assign(count, 1);
    } else if (valid_mask.size() != count) {
        valid_mask.resize(count, 1);
    }

    data_.eye_angle_run_name = latest_run;
    data_.eye_angle_left_deg = std::move(left_angles);
    data_.eye_angle_right_deg = std::move(right_angles);
    data_.eye_angle_frame_indices = std::move(frame_indices);
    data_.eye_angle_valid_mask = std::move(valid_mask);

    size_t max_frame_index = 0;
    for (auto frame : data_.eye_angle_frame_indices) {
        if (frame >= 0) {
            max_frame_index = std::max(max_frame_index,
                                       static_cast<size_t>(frame));
        }
    }
    size_t desired_size = std::max({max_frame_index + 1,
                                    data_.total_frames,
                                    roi_count});
    data_.eye_angle_indices_by_frame.clear();
    data_.eye_angle_indices_by_frame.resize(desired_size);
    for (size_t i = 0; i < data_.eye_angle_frame_indices.size(); ++i) {
        int32_t frame = data_.eye_angle_frame_indices[i];
        if (frame < 0) continue;
        size_t frame_index = static_cast<size_t>(frame);
        if (frame_index >= data_.eye_angle_indices_by_frame.size()) {
            data_.eye_angle_indices_by_frame.resize(frame_index + 1);
        }
        data_.eye_angle_indices_by_frame[frame_index].push_back(i);
    }

    data_.has_eye_angles = true;
    std::cout << "  Eye angle run '" << latest_run << "' loaded ("
              << data_.eye_angle_left_deg.size()
              << " ROI entries)" << std::endl;
    if (roi_count > 0 && data_.eye_angle_left_deg.size() != roi_count) {
        std::cout << "    [EYE_ANGLE_WARNING] ROI count mismatch: angles="
                  << data_.eye_angle_left_deg.size()
                  << ", expected " << roi_count << std::endl;
    }
    return true;
}

const ZarrDetectionData::EyeMaskChunkCacheEntry*
ZarrDetectionLoader::findEyeMaskChunk(size_t chunk_id) const {
    for (auto& entry : data_.mask_chunk_cache) {
        if (entry.chunk_id == chunk_id) {
            return &entry;
        }
    }
    return nullptr;
}

bool ZarrDetectionLoader::ensureEyeMaskChunk(size_t chunk_id,
                                             bool allow_prefetch) const {
    if (!data_.eye_masks_loaded || data_.eye_mask_roi_count == 0) {
        return false;
    }

    auto& cache = data_.mask_chunk_cache;
    auto it = std::find_if(cache.begin(), cache.end(),
                           [&](const ZarrDetectionData::EyeMaskChunkCacheEntry& entry) {
                               return entry.chunk_id == chunk_id;
                           });
    if (it != cache.end()) {
        if (std::next(it) != cache.end()) {
            ZarrDetectionData::EyeMaskChunkCacheEntry entry = std::move(*it);
            cache.erase(it);
            cache.push_back(std::move(entry));
        }
        return true;
    }

    size_t chunk_rows =
        data_.eye_mask_chunk_rows > 0 ? data_.eye_mask_chunk_rows : 512;
    size_t chunk_start = chunk_id * chunk_rows;
    if (chunk_start >= data_.eye_mask_roi_count) {
        return false;
    }
    size_t chunk_end =
        std::min(chunk_start + chunk_rows, data_.eye_mask_roi_count);

    auto slice = data_.eye_masks_store |
                 ts::Dims(0).HalfOpenInterval(
                     static_cast<ts::Index>(chunk_start),
                     static_cast<ts::Index>(chunk_end));
    auto read_result = ts::Read(slice).result();
    if (!read_result.ok()) {
        std::cerr << "[EYE_MASK_WARNING] Failed to read mask chunk "
                  << chunk_id << ": " << read_result.status().ToString()
                  << std::endl;
        return false;
    }

    auto array = read_result.value();
    auto shape = array.shape();
    if (shape.size() != 4) {
        std::cerr << "[EYE_MASK_WARNING] Unexpected mask chunk rank ("
                  << shape.size() << ")" << std::endl;
        return false;
    }

    size_t chunk_len = static_cast<size_t>(shape[0]);
    size_t channels = static_cast<size_t>(shape[1]);
    size_t rows = static_cast<size_t>(shape[2]);
    size_t cols = static_cast<size_t>(shape[3]);

    ZarrDetectionData::EyeMaskChunkCacheEntry entry;
    entry.chunk_id = chunk_id;
    entry.chunk_start = chunk_start;
    entry.chunk_length = chunk_len;
    entry.pixel_indices.resize(chunk_len);

    const uint8_t* base_ptr =
        static_cast<const uint8_t*>(array.byte_strided_origin_pointer());
    auto byte_strides = array.byte_strides();
    if (byte_strides.size() != 4) {
        std::cerr << "[EYE_MASK_WARNING] Unexpected mask chunk stride rank ("
                  << byte_strides.size() << ")" << std::endl;
        return false;
    }

    const ts::Index stride_roi = byte_strides[0];
    const ts::Index stride_channel = byte_strides[1];
    const ts::Index stride_row = byte_strides[2];
    const ts::Index stride_col = byte_strides[3];

    for (size_t roi = 0; roi < chunk_len; ++roi) {
        const auto roi_offset =
            stride_roi * static_cast<ts::Index>(roi);
        const uint8_t* roi_ptr = base_ptr + roi_offset;
        for (size_t channel = 0; channel < std::min<size_t>(channels, 2); ++channel) {
            auto& indices_vec = entry.pixel_indices[roi][channel];
            indices_vec.clear();
            indices_vec.reserve(256);

            const auto channel_offset =
                stride_channel * static_cast<ts::Index>(channel);
            const uint8_t* channel_ptr = roi_ptr + channel_offset;
            for (size_t r = 0; r < rows; ++r) {
                const auto row_offset =
                    stride_row * static_cast<ts::Index>(r);
                const uint8_t* row_ptr = channel_ptr + row_offset;
                for (size_t c = 0; c < cols; ++c) {
                    const auto col_offset =
                        stride_col * static_cast<ts::Index>(c);
                    const uint8_t* elem_ptr = row_ptr + col_offset;
                    if (*elem_ptr != 0) {
                        indices_vec.push_back(
                            static_cast<uint16_t>(r * cols + c));
                    }
                }
            }
        }
    }

    if (cache.size() >= kEyeMaskChunkCacheCapacity) {
        cache.erase(cache.begin());
    }
    cache.push_back(std::move(entry));

    if (allow_prefetch) {
        prefetchAdjacentEyeMaskChunks(chunk_id);
    }
    return true;
}

void ZarrDetectionLoader::prefetchAdjacentEyeMaskChunks(size_t chunk_id) const {
    size_t chunk_rows =
        data_.eye_mask_chunk_rows > 0 ? data_.eye_mask_chunk_rows : 512;
    if (chunk_id > 0) {
        ensureEyeMaskChunk(chunk_id - 1, /*allow_prefetch=*/false);
    }
    if ((chunk_id + 1) * chunk_rows < data_.eye_mask_roi_count) {
        ensureEyeMaskChunk(chunk_id + 1, /*allow_prefetch=*/false);
    }
}

bool ZarrDetectionLoader::populateEyeMaskEntry(
    size_t roi_index, FrameDetections::EyeMask& out_mask) const {
    if (!data_.eye_masks_loaded || roi_index >= data_.eye_mask_roi_count) {
        return false;
    }
    size_t chunk_rows =
        data_.eye_mask_chunk_rows > 0 ? data_.eye_mask_chunk_rows : 512;
    size_t chunk_id = roi_index / chunk_rows;
    if (!ensureEyeMaskChunk(chunk_id)) {
        return false;
    }
    const auto* entry = findEyeMaskChunk(chunk_id);
    if (entry == nullptr) {
        return false;
    }
    size_t local_index = roi_index - entry->chunk_start;
    if (local_index >= entry->pixel_indices.size()) {
        return false;
    }

    out_mask.rows = static_cast<int>(data_.eye_mask_height);
    out_mask.cols = static_cast<int>(data_.eye_mask_width);
    out_mask.valid = false;
    out_mask.has_feret_axes = false;
    const float angle_nan = std::numeric_limits<float>::quiet_NaN();
    out_mask.feret_minor_angle_deg[0] = angle_nan;
    out_mask.feret_minor_angle_deg[1] = angle_nan;
    out_mask.feret_angle_valid = {0, 0};
    out_mask.has_eye_angles = false;
    out_mask.roi_index = static_cast<int32_t>(roi_index);
    for (size_t channel = 0; channel < 2; ++channel) {
        out_mask.pixel_indices[channel] =
            entry->pixel_indices[local_index][channel];
        if (!out_mask.pixel_indices[channel].empty()) {
            out_mask.valid = true;
        }

        if (roi_index < data_.eye_mask_feret_axes_major.size()) {
            const auto& axis_vals =
                data_.eye_mask_feret_axes_major[roi_index][channel];
            bool axis_valid = true;
            for (float value : axis_vals) {
                if (!std::isfinite(value)) {
                    axis_valid = false;
                    break;
                }
            }
            if (axis_valid) {
                float dx = axis_vals[0] - axis_vals[2];
                float dy = axis_vals[1] - axis_vals[3];
                if (std::fabs(dx) > 1e-5f || std::fabs(dy) > 1e-5f) {
                    auto& segment = out_mask.feret_major[channel];
                    segment.valid = true;
                    segment.x0 = axis_vals[0];
                    segment.y0 = axis_vals[1];
                    segment.x1 = axis_vals[2];
                    segment.y1 = axis_vals[3];
                    out_mask.has_feret_axes = true;
                }
            }
        }
        if (roi_index < data_.eye_mask_feret_axes_minor.size()) {
            const auto& axis_vals =
                data_.eye_mask_feret_axes_minor[roi_index][channel];
            bool axis_valid = true;
            for (float value : axis_vals) {
                if (!std::isfinite(value)) {
                    axis_valid = false;
                    break;
                }
            }
            if (axis_valid) {
                float dx = axis_vals[0] - axis_vals[2];
                float dy = axis_vals[1] - axis_vals[3];
                if (std::fabs(dx) > 1e-5f || std::fabs(dy) > 1e-5f) {
                    auto& segment = out_mask.feret_minor[channel];
                    segment.valid = true;
                    segment.x0 = axis_vals[0];
                    segment.y0 = axis_vals[1];
                    segment.x1 = axis_vals[2];
                    segment.y1 = axis_vals[3];
                    out_mask.has_feret_axes = true;
                }
            }
        }
    }
    return out_mask.valid;
}

bool ZarrDetectionLoader::loadStimulusAlignment(const ts::kvstore::KvStore& store) {
    std::optional<std::string> latest_run_opt;
    if (auto group_attrs = readAttrsAny(store, "analysis/stimulus_runs")) {
        auto latest = extractLatestRunName(*group_attrs);
        if (!latest.empty()) {
            std::cout << "  [Stimulus] Attr latest run candidate: '" << latest << "'" << std::endl;
            latest_run_opt = latest;
        } else {
            std::cout << "  [Stimulus] No 'latest' pointer in analysis/stimulus_runs attrs" << std::endl;
        }
    } else {
        std::cout << "  [Stimulus] analysis/stimulus_runs attrs missing" << std::endl;
    }

    if (!latest_run_opt.has_value() && !root_path_.empty()) {
        auto candidates = collect_runs_fs(root_path_, "analysis/stimulus_runs", {});
        if (!candidates.empty()) {
            latest_run_opt = candidates.back();
            std::cout << "  [Stimulus] Falling back to filesystem run '" << *latest_run_opt
                      << "'" << std::endl;
        }
    }

    if (!latest_run_opt.has_value()) {
        return false;
    }
    const std::string& latest_run = *latest_run_opt;

    std::string run_base = "analysis/stimulus_runs/" + latest_run + "/";
    std::cout << "  Loading stimulus alignment run '" << latest_run << "'" << std::endl;
    data_.has_stimulus_alignment_data = false;
    data_.stimulus_camera_frame_offset = 0;
    data_.chaser_transform = ZarrDetectionData::ChaserCoordinateTransform();

    // Always attempt to load stimulus event metadata, even if frame alignment
    // data is missing. This keeps the event timeline available in the UI.
    loadStimulusEventEnums(store);
    bool events_loaded = loadStimulusEventsForRun(store, run_base);
    if (!events_loaded) {
        std::cout << "  Stimulus run '" << latest_run
                  << "' does not contain events metadata." << std::endl;
    } else {
        std::cout << "  Stimulus run '" << latest_run << "' events metadata loaded" << std::endl;
    }

    if (!loadChaserStates(store, run_base)) {
        std::cout << "  [Chaser] No chaser tracking data for run '" << latest_run << "'" << std::endl;
    }
    if (!loadChaserBoundingBoxes(store, run_base)) {
        std::cout << "  [ChaserBBox] No tracking bounding boxes for run '" << latest_run << "'" << std::endl;
    }

    std::string stimulus_created_at;
    int64_t camera_frame_offset = 0;
    std::vector<int32_t> camera_to_metadata_index;
    std::vector<uint8_t> metadata_mask;

    if (auto run_attrs = readAttrsAny(store, run_base)) {
        if (run_attrs->contains("created_at_utc") && (*run_attrs)["created_at_utc"].is_string()) {
            stimulus_created_at = (*run_attrs)["created_at_utc"].get<std::string>();
        } else if (run_attrs->contains("created_at") && (*run_attrs)["created_at"].is_string()) {
            stimulus_created_at = (*run_attrs)["created_at"].get<std::string>();
        }

        auto parseCoordinateTransform = [&]() {
            try {
                if (run_attrs->contains("coordinate_transform") &&
                    (*run_attrs)["coordinate_transform"].is_string()) {
                    auto transform_json = json::parse((*run_attrs)["coordinate_transform"].get<std::string>());
                    if (transform_json.contains("texture_dimensions") &&
                        transform_json["texture_dimensions"].is_array()) {
                        const auto& dims = transform_json["texture_dimensions"];
                        if (dims.size() == 2) {
                            data_.chaser_transform.texture_width = dims[0].get<double>();
                            data_.chaser_transform.texture_height = dims[1].get<double>();
                        }
                    }
                    if (transform_json.contains("camera_dimensions") &&
                        transform_json["camera_dimensions"].is_array()) {
                        const auto& dims = transform_json["camera_dimensions"];
                        if (dims.size() == 2) {
                            data_.chaser_transform.camera_width = dims[0].get<double>();
                            data_.chaser_transform.camera_height = dims[1].get<double>();
                        }
                    }
                    if (transform_json.contains("texture_to_camera_scale") &&
                        transform_json["texture_to_camera_scale"].is_number()) {
                        data_.chaser_transform.scale = transform_json["texture_to_camera_scale"].get<double>();
                    }
                }
                if (run_attrs->contains("arena_config_json") &&
                    (*run_attrs)["arena_config_json"].is_string()) {
                    auto arena_json = json::parse((*run_attrs)["arena_config_json"].get<std::string>());
                    if (arena_json.contains("sub_arena_x_px") && arena_json["sub_arena_x_px"].is_number()) {
                        data_.chaser_transform.offset_x_px = arena_json["sub_arena_x_px"].get<double>();
                    }
                    if (arena_json.contains("sub_arena_y_px") && arena_json["sub_arena_y_px"].is_number()) {
                        data_.chaser_transform.offset_y_px = arena_json["sub_arena_y_px"].get<double>();
                    }
                    if (arena_json.contains("sub_arena_width_px") && arena_json["sub_arena_width_px"].is_number()) {
                        double sub_width = arena_json["sub_arena_width_px"].get<double>();
                        if (sub_width > 0.0 && data_.chaser_transform.texture_width > 0.0) {
                            data_.chaser_transform.scale = sub_width / data_.chaser_transform.texture_width;
                        }
                    }
                }
            } catch (const std::exception& e) {
                std::cout << "  [Chaser] Failed to parse coordinate_transform metadata: "
                          << e.what() << std::endl;
            }

            if (data_.chaser_transform.texture_width <= 0.0) {
                data_.chaser_transform.texture_width = 358.0;
            }
            if (data_.chaser_transform.texture_height <= 0.0) {
                data_.chaser_transform.texture_height = 358.0;
            }
            if (data_.chaser_transform.scale <= 0.0) {
                if (data_.chaser_transform.camera_width > 0.0) {
                    data_.chaser_transform.scale =
                        data_.chaser_transform.camera_width / data_.chaser_transform.texture_width;
                } else {
                    data_.chaser_transform.scale = 1.0;
                }
            }
            data_.chaser_transform.valid = true;
        };

        parseCoordinateTransform();
    }

    if (auto align_attrs = readAttrsAny(store, run_base + "frame_alignment")) {
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

    if (!has_camera_mask || camera_mask.empty()) {
        std::cout << "  Stimulus run '" << latest_run
                  << "' missing camera_interpolation_mask data" << std::endl;
        return false;
    }

    if (!has_camera_mapping || camera_to_metadata_raw.empty()) {
        std::cout << "  Stimulus run '" << latest_run
                  << "' missing camera_to_metadata_index data" << std::endl;
        return false;
    }

    if (has_camera_mapping) {
        camera_to_metadata_index.reserve(camera_to_metadata_raw.size());
        for (auto value : camera_to_metadata_raw) {
            camera_to_metadata_index.push_back(static_cast<int32_t>(value));
        }
    }

    size_t frame_count = camera_mask.size();
    std::vector<uint8_t> stored_mask = camera_mask;

    if (frame_count == 0) {
        std::cout << "  Stimulus alignment has zero frames" << std::endl;
        return false;
    }

    std::vector<uint8_t> new_mask(frame_count, 0);
    for (size_t i = 0; i < frame_count; ++i) {
        bool original = camera_mask[i] != 0;
        new_mask[i] = original ? 0 : 1;
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
   data_.has_stimulus_alignment_data = true;
   data_.stimulus_camera_frame_offset = camera_frame_offset;
   std::cout << "  Stimulus alignment '" << latest_run
             << "' loaded with frame mask for " << frame_count << " frames" << std::endl;

    updateChaserCameraFramesFromAlignment();

   return true;
}

void ZarrDetectionLoader::loadStimulusEventEnums(const ts::kvstore::KvStore& store) {
    if (!data_.event_type_names.empty()) {
        return;
    }

    const std::vector<std::pair<std::string, std::string>> candidates = {
        {"analysis/enums/events/event_type_id", "analysis/enums/events/name"},
        {"analysis/enums/events/event_type_id", "analysis/enums/events/value"},
        {"analysis/enums/events/id", "analysis/enums/events/name"},
        {"analysis/enums/events/id", "analysis/enums/events/value"},
        {"analysis/enums/events/ids", "analysis/enums/events/names"}
    };

    for (const auto& candidate : candidates) {
        std::vector<int32_t> ids32;
        if (!readInt32Array(store, candidate.first, ids32)) {
            std::vector<int64_t> ids64;
            if (!readInt64Array(store, candidate.first, ids64)) {
                continue;
            }
            ids32.resize(ids64.size());
            for (size_t i = 0; i < ids64.size(); ++i) {
                ids32[i] = static_cast<int32_t>(ids64[i]);
            }
        }

        std::vector<std::string> names;
        if (!readStringArray(store, candidate.second, names)) {
            continue;
        }

        size_t count = std::min(ids32.size(), names.size());
        if (count == 0) {
            continue;
        }

        data_.event_type_names.clear();
        for (size_t i = 0; i < count; ++i) {
            data_.event_type_names[ids32[i]] = names[i];
        }
        std::cout << "  Loaded " << data_.event_type_names.size()
                  << " stimulus event type labels" << std::endl;
        return;
    }

    std::cout << "  [Stimulus] No stimulus event enum labels loaded from known paths" << std::endl;
}

bool ZarrDetectionLoader::loadStimulusEventsForRun(const ts::kvstore::KvStore& store,
                                                   const std::string& run_base) {
    std::string events_base = run_base + "events/";
    bool has_column_layout = arrayExists(store, events_base + "stimulus_frame_num");
    bool has_structured_layout = arrayExists(store, run_base + "events");

    std::cout << "  [StimulusEvents] Inspecting run '" << run_base << "'" << std::endl;
    std::cout << "    events_base='" << events_base << "'" << std::endl;
    std::cout << "    column layout present: " << (has_column_layout ? "yes" : "no")
              << ", structured layout present: " << (has_structured_layout ? "yes" : "no")
              << std::endl;

    if ((!has_column_layout || !has_structured_layout) && !root_path_.empty()) {
        namespace fs = std::filesystem;
        fs::path root(root_path_);
        if (!has_column_layout) {
            fs::path column_probe =
                root / fs::path(events_base) / "stimulus_frame_num" / "zarr.json";
            if (fs::exists(column_probe)) {
                has_column_layout = true;
                std::cout << "    column layout detected via filesystem probe at "
                          << column_probe << std::endl;
            }
        }
        if (!has_structured_layout) {
            fs::path structured_probe =
                root / fs::path(run_base) / "events" / "zarr.json";
            if (fs::exists(structured_probe)) {
                has_structured_layout = true;
                std::cout << "    structured layout detected via filesystem probe at "
                          << structured_probe << std::endl;
            }
        }
    }

    if (!has_column_layout && !has_structured_layout) {
        std::cout << "  [Stimulus] No events dataset at " << events_base << " or "
                  << run_base << "events" << std::endl;
    } else if (has_column_layout && has_structured_layout) {
        std::cout << "  [Stimulus] Events available in both column and structured layouts; "
                     "using column arrays."
                  << std::endl;
    } else if (has_structured_layout) {
        std::cout << "  [Stimulus] Events stored as structured array." << std::endl;
    } else if (has_column_layout) {
        std::cout << "  [Stimulus] Events stored as column arrays." << std::endl;
    }

    std::vector<int32_t> stimulus_to_camera_map;
    bool has_frame_mapping =
        loadStimulusFrameMetadataMapping(store, run_base, stimulus_to_camera_map) &&
        !stimulus_to_camera_map.empty();

    auto finalize_events = [&](size_t max_frame) {
        std::cout << "  [StimulusEvents] Finalizing events (max_frame=" << max_frame << ")"
                  << std::endl;
        size_t size_needed = std::max<size_t>(data_.total_frames,
                                              max_frame + 1);
        if (data_.stimulus_events_by_frame.size() < size_needed) {
            data_.stimulus_events_by_frame.resize(size_needed);
        }
        for (auto& vec : data_.stimulus_events_by_frame) {
            vec.clear();
        }
        if (data_.stimulus_events_by_camera_frame.size() < data_.total_frames) {
            data_.stimulus_events_by_camera_frame.resize(data_.total_frames);
        }
        for (auto& vec : data_.stimulus_events_by_camera_frame) {
            vec.clear();
        }
        for (size_t idx = 0; idx < data_.stimulus_events.size(); ++idx) {
            int32_t frame = data_.stimulus_events[idx].stimulus_frame_num;
            if (frame < 0) continue;
            size_t frame_index = static_cast<size_t>(frame);
            if (frame_index >= data_.stimulus_events_by_frame.size()) {
                data_.stimulus_events_by_frame.resize(frame_index + 1);
            }
            data_.stimulus_events_by_frame[frame_index].push_back(idx);

            int32_t camera_frame = data_.stimulus_events[idx].camera_frame_id;
            if (camera_frame < 0 && has_frame_mapping &&
                frame_index < stimulus_to_camera_map.size()) {
                camera_frame = stimulus_to_camera_map[frame_index];
            }
            if (camera_frame >= 0) {
                size_t camera_index = static_cast<size_t>(camera_frame);
                if (camera_index >= data_.stimulus_events_by_camera_frame.size()) {
                    data_.stimulus_events_by_camera_frame.resize(camera_index + 1);
                }
                data_.stimulus_events_by_camera_frame[camera_index].push_back(idx);
            }
        }

        data_.has_stimulus_events = !data_.stimulus_events.empty();
        if (data_.has_stimulus_events) {
            std::cout << "  Loaded " << data_.stimulus_events.size()
                      << " stimulus events" << std::endl;
            const auto& sample = data_.stimulus_events.front();
            std::cout << "  [StimulusEvents] Sample entry: stimulus_frame="
                      << sample.stimulus_frame_num << ", camera_frame="
                      << sample.camera_frame_id << ", event_type="
                      << sample.event_type_id << ", name='"
                      << sample.name_or_context << "'" << std::endl;
            size_t missing_camera = 0;
            for (const auto& entry : data_.stimulus_events) {
                if (entry.camera_frame_id < 0) {
                    ++missing_camera;
                }
            }
            if (missing_camera > 0) {
                std::cout << "  [StimulusEvents] Entries lacking camera_frame_id: "
                          << missing_camera << std::endl;
            }
        } else {
            std::cout << "  No stimulus events found for run '" << run_base
                      << "'" << std::endl;
        }
        return data_.has_stimulus_events;
    };

    if (has_column_layout) {
        auto readInt32Or64 = [&](const std::string& path, std::vector<int32_t>& dest) -> bool {
            std::cout << "  [StimulusEvents] Reading column '" << path << "'" << std::endl;
            if (readInt32Array(store, path, dest)) {
                return true;
            }
            std::cout << "  [StimulusEvents] Falling back to int64_t reader for '" << path
                      << "'" << std::endl;
            std::vector<int64_t> tmp64;
            if (!readInt64Array(store, path, tmp64)) {
                std::cout << "  [StimulusEvents] Failed to read '" << path
                          << "' as int64_t" << std::endl;
                return false;
            }
            dest.resize(tmp64.size());
            bool overflow = false;
            for (size_t i = 0; i < tmp64.size(); ++i) {
                int32_t converted = clampToInt32(tmp64[i]);
                if (converted != static_cast<int32_t>(tmp64[i])) {
                    overflow = true;
                }
                dest[i] = converted;
            }
            if (overflow) {
                std::cout << "  [StimulusEvents] '" << path
                          << "' required clamp during int64->int32 conversion" << std::endl;
            }
            return true;
        };

        std::vector<int32_t> stimulus_frames;
        if (!readInt32Or64(events_base + "stimulus_frame_num", stimulus_frames)) {
            return false;
        }
        size_t count = stimulus_frames.size();
        if (count == 0) {
            std::cout << "  [StimulusEvents] Column layout contained zero rows" << std::endl;
            data_.stimulus_events.clear();
            data_.stimulus_events_by_frame.clear();
            data_.stimulus_events_by_camera_frame.clear();
            data_.has_stimulus_events = false;
            return true;
        }

        std::vector<int32_t> camera_frames;
        if (!readInt32Or64(events_base + "camera_frame_id", camera_frames)) {
            std::cout << "  [StimulusEvents] camera_frame_id missing; defaulting to -1"
                      << std::endl;
            camera_frames.assign(count, -1);
        }

        std::vector<int32_t> event_type_ids;
        if (!readInt32Or64(events_base + "event_type_id", event_type_ids)) {
            std::cout << "  [StimulusEvents] event_type_id missing; defaulting to -1"
                      << std::endl;
            event_type_ids.assign(count, -1);
        }

        std::vector<int64_t> timestamps;
        if (!readInt64Array(store, events_base + "timestamp_ns_session", timestamps)) {
            std::cout << "  [StimulusEvents] timestamp_ns_session missing; defaulting to 0"
                      << std::endl;
            timestamps.assign(count, 0);
        }
        if (timestamps.size() != count) {
            timestamps.resize(count, 0);
        }

        std::vector<std::string> names;
        if (!readStringArray(store, events_base + "name_or_context", names)) {
            std::cout << "  [StimulusEvents] name_or_context missing; defaulting to empty string"
                      << std::endl;
            names.assign(count, std::string());
        }

        std::vector<std::string> details;
        if (!readStringArray(store, events_base + "details_json", details)) {
            std::cout << "  [StimulusEvents] details_json missing; defaulting to empty string"
                      << std::endl;
            details.assign(count, std::string());
        }

        auto sync_size = [&](auto& vec, const auto& default_value) {
            if (vec.size() != count) {
                vec.resize(count, default_value);
            }
        };
        sync_size(camera_frames, int32_t{-1});
        sync_size(event_type_ids, int32_t{-1});
        sync_size(names, std::string());
        sync_size(details, std::string());

        data_.stimulus_events.clear();
        data_.stimulus_events.reserve(count);

        size_t max_frame = 0;
        for (size_t i = 0; i < count; ++i) {
            ZarrDetectionData::EventLogEntry entry;
            entry.stimulus_frame_num = stimulus_frames[i];
            entry.camera_frame_id = (i < camera_frames.size()) ? camera_frames[i] : -1;
            entry.timestamp_ns_session = (i < timestamps.size()) ? timestamps[i] : 0;
            entry.event_type_id = (i < event_type_ids.size()) ? event_type_ids[i] : -1;
            if (i < names.size()) entry.name_or_context = names[i];
            if (i < details.size()) entry.details_json = details[i];
            if (entry.stimulus_frame_num >= 0) {
                max_frame = std::max(max_frame,
                                     static_cast<size_t>(entry.stimulus_frame_num));
            }
            data_.stimulus_events.push_back(std::move(entry));
        }
        std::cout << "  [StimulusEvents] Column layout produced "
                  << data_.stimulus_events.size() << " entries" << std::endl;
        return finalize_events(max_frame);
    }

    if (!has_structured_layout) {
        data_.stimulus_events.clear();
        data_.stimulus_events_by_frame.clear();
        data_.stimulus_events_by_camera_frame.clear();
        data_.has_stimulus_events = false;
        std::cout << "  [StimulusEvents] Structured layout absent and column layout failed"
                  << std::endl;
        return false;
    }

    auto open_result = openArrayAny<StimulusEventRowV3, 1>(
        store, run_base + "events", context_);
    if (!open_result.ok()) {
        std::cerr << "Failed to open structured stimulus events array: "
                  << open_result.status() << std::endl;
        return false;
    }

    auto read_result = ts::Read(open_result.value()).result();
    if (!read_result.ok()) {
        std::cerr << "Failed to read structured stimulus events array: "
                  << read_result.status() << std::endl;
        return false;
    }

    auto array = read_result.value();
    if (array.rank() != 1) {
        std::cerr << "Structured stimulus events array has unexpected rank "
                  << array.rank() << std::endl;
        return false;
    }

    size_t count = static_cast<size_t>(array.shape()[0]);
    data_.stimulus_events.clear();
    data_.stimulus_events.reserve(count);
    std::cout << "  [StimulusEvents] Structured layout contains " << count << " rows"
              << std::endl;

    const auto* rows = static_cast<const StimulusEventRowV3*>(array.data());
    size_t max_frame = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto& row = rows[i];
        ZarrDetectionData::EventLogEntry entry;
        entry.stimulus_frame_num = clampUint64ToInt32(row.stimulus_frame_num);
        entry.camera_frame_id = clampUint64ToInt32(row.camera_frame_id);
        entry.timestamp_ns_session = row.timestamp_ns_session;
        entry.event_type_id = clampToInt32(row.event_type_id);
        entry.name_or_context = stringFromFixedBuffer(row.name_or_context, sizeof(row.name_or_context));
        entry.details_json = stringFromFixedBuffer(row.details_json, sizeof(row.details_json));
        if (entry.stimulus_frame_num >= 0) {
            max_frame = std::max(max_frame,
                                 static_cast<size_t>(entry.stimulus_frame_num));
        }
        data_.stimulus_events.push_back(std::move(entry));
    }
    std::cout << "  [StimulusEvents] Structured layout parsed "
              << data_.stimulus_events.size() << " events" << std::endl;

    return finalize_events(max_frame);
}

bool ZarrDetectionLoader::loadChaserBoundingBoxes(
    const ts::kvstore::KvStore& store,
    const std::string& run_base) {
    const std::string base = run_base + "tracking_data/bounding_boxes/";
    data_.chaser_bounding_boxes.clear();
    data_.chaser_bboxes_by_camera_frame.clear();
    data_.has_chaser_bboxes = false;

    auto column_exists = [&](const std::string& name) {
        if (arrayExists(store, base + name)) {
            return true;
        }
        if (root_path_.empty()) {
            return false;
        }
        namespace fs = std::filesystem;
        fs::path probe = fs::path(root_path_) / base / name / "zarr.json";
        return fs::exists(probe);
    };

    auto readIntColumn = [&](const std::vector<std::string>& names,
                             std::vector<int32_t>& dest) -> bool {
        for (const auto& name : names) {
            if (readInt32Array(store, base + name, dest) && !dest.empty()) {
                return true;
            }
            std::vector<int64_t> tmp64;
            if (readInt64Array(store, base + name, tmp64) && !tmp64.empty()) {
                dest.resize(tmp64.size());
                for (size_t i = 0; i < tmp64.size(); ++i) {
                    dest[i] = clampToInt32(tmp64[i]);
                }
                return true;
            }
        }
        return false;
    };

    auto readFloatColumn = [&](const std::vector<std::string>& names,
                               std::vector<float>& dest) -> bool {
        for (const auto& name : names) {
            if (readFloatArray(store, base + name, dest) && !dest.empty()) {
                return true;
            }
        }
        dest.clear();
        return false;
    };

    if (!column_exists("x_px") && !column_exists("x_min")) {
        std::cout << "  [ChaserBBox] No bounding box coordinates found at '" << base << "'" << std::endl;
        return false;
    }

    std::vector<float> x_vals;
    std::vector<float> y_vals;
    std::vector<float> width_vals;
    std::vector<float> height_vals;
    if (!readFloatColumn({"x_px", "x_min"}, x_vals) || x_vals.empty()) {
        std::cout << "  [ChaserBBox] Failed to read x coordinates from '" << base << "'" << std::endl;
        return false;
    }
    if (!readFloatColumn({"y_px", "y_min"}, y_vals) || y_vals.empty()) {
        std::cout << "  [ChaserBBox] Failed to read y coordinates from '" << base << "'" << std::endl;
        return false;
    }
    if (!readFloatColumn({"width_px", "width"}, width_vals) || width_vals.empty()) {
        std::cout << "  [ChaserBBox] Failed to read width from '" << base << "'" << std::endl;
        return false;
    }
    if (!readFloatColumn({"height_px", "height"}, height_vals) || height_vals.empty()) {
        std::cout << "  [ChaserBBox] Failed to read height from '" << base << "'" << std::endl;
        return false;
    }

    size_t count = x_vals.size();
    auto ensure_size = [&](auto& vec, const auto& default_value) {
        if (vec.empty()) {
            vec.assign(count, default_value);
        } else if (vec.size() != count) {
            vec.resize(count, default_value);
        }
    };

    ensure_size(y_vals, std::numeric_limits<float>::quiet_NaN());
    ensure_size(width_vals, std::numeric_limits<float>::quiet_NaN());
    ensure_size(height_vals, std::numeric_limits<float>::quiet_NaN());

    std::vector<float> centroid_x_vals;
    std::vector<float> centroid_y_vals;
    if (!readFloatColumn({"centroid_x", "cx_px"}, centroid_x_vals)) {
        centroid_x_vals.assign(count, std::numeric_limits<float>::quiet_NaN());
    } else {
        ensure_size(centroid_x_vals, std::numeric_limits<float>::quiet_NaN());
    }
    if (!readFloatColumn({"centroid_y", "cy_px"}, centroid_y_vals)) {
        centroid_y_vals.assign(count, std::numeric_limits<float>::quiet_NaN());
    } else {
        ensure_size(centroid_y_vals, std::numeric_limits<float>::quiet_NaN());
    }

    std::vector<float> confidence_vals;
    if (readFloatColumn({"confidence", "score"}, confidence_vals)) {
        ensure_size(confidence_vals, std::numeric_limits<float>::quiet_NaN());
    } else {
        confidence_vals.assign(count, std::numeric_limits<float>::quiet_NaN());
    }

    std::vector<int32_t> camera_frames;
    std::vector<int32_t> stimulus_frames;
    std::vector<int32_t> fish_ids;
    std::vector<int32_t> chaser_indices;

    readIntColumn({"camera_frame_id", "payload_frame_id", "frame_id"}, camera_frames);
    readIntColumn({"stimulus_frame_num", "stimulus_frame_id"}, stimulus_frames);
    readIntColumn({"fish_id", "roi_index", "box_index_in_payload"}, fish_ids);
    readIntColumn({"chaser_index"}, chaser_indices);

    ensure_size(camera_frames, -1);
    ensure_size(stimulus_frames, -1);
    ensure_size(fish_ids, -1);
    ensure_size(chaser_indices, -1);

    std::vector<uint8_t> target_mask_raw;
    bool has_target_mask = false;
    for (const auto& name : {"is_target", "target_flag", "is_target_roi"}) {
        if (readBoolArray(store, base + name, target_mask_raw) && !target_mask_raw.empty()) {
            has_target_mask = true;
            ensure_size(target_mask_raw, static_cast<uint8_t>(0));
            break;
        }
    }
    if (!has_target_mask) {
        target_mask_raw.assign(count, 0);
    }

    data_.chaser_bounding_boxes.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        ZarrDetectionData::ChaserBoundingBoxRecord record;
        record.camera_frame_id = camera_frames[i];
        record.stimulus_frame_num = stimulus_frames[i];
        record.fish_id = fish_ids[i];
        record.chaser_index = chaser_indices[i];
        record.x_px = x_vals[i];
        record.y_px = y_vals[i];
        record.width_px = width_vals[i];
        record.height_px = height_vals[i];
        record.centroid_x = centroid_x_vals[i];
        record.centroid_y = centroid_y_vals[i];
        record.confidence = confidence_vals[i];
        bool target_flag = has_target_mask ? (target_mask_raw[i] != 0) : false;
        if (!target_flag && record.fish_id < 0) {
            target_flag = true;
        }
        record.is_target = target_flag;
        data_.chaser_bounding_boxes.push_back(record);
    }

    bool any_camera_frames = false;
    for (const auto& record : data_.chaser_bounding_boxes) {
        if (record.camera_frame_id >= 0) {
            any_camera_frames = true;
            break;
        }
    }

    if (!any_camera_frames) {
        std::vector<int32_t> stimulus_to_camera;
        if (loadStimulusFrameMetadataMapping(store, run_base, stimulus_to_camera) &&
            !stimulus_to_camera.empty()) {
            size_t updated = 0;
            for (auto& record : data_.chaser_bounding_boxes) {
                int32_t stim_index = record.stimulus_frame_num;
                if (stim_index < 0 || static_cast<size_t>(stim_index) >= stimulus_to_camera.size()) {
                    continue;
                }
                int32_t frame = stimulus_to_camera[stim_index];
                if (frame >= 0) {
                    record.camera_frame_id = frame;
                    ++updated;
                }
            }
            if (updated > 0) {
                std::cout << "  [ChaserBBox] Camera frames inferred from frame metadata ("
                          << updated << " records)" << std::endl;
            }
        } else {
            std::cout << "  [ChaserBBox] Unable to resolve camera frames for run '"
                      << run_base << "'" << std::endl;
        }
    }

    data_.has_chaser_bboxes = !data_.chaser_bounding_boxes.empty();
    if (!data_.has_chaser_bboxes) {
        data_.chaser_bounding_boxes.clear();
        return false;
    }

    rebuildChaserBoundingBoxIndices();

    int32_t min_camera_frame = std::numeric_limits<int32_t>::max();
    int32_t max_camera_frame = std::numeric_limits<int32_t>::min();
    for (const auto& record : data_.chaser_bounding_boxes) {
        if (record.camera_frame_id >= 0) {
            min_camera_frame = std::min(min_camera_frame, record.camera_frame_id);
            max_camera_frame = std::max(max_camera_frame, record.camera_frame_id);
        }
    }

    std::cout << "  [ChaserBBox] Loaded " << data_.chaser_bounding_boxes.size()
              << " bounding box records";
    if (max_camera_frame >= min_camera_frame &&
        min_camera_frame != std::numeric_limits<int32_t>::max()) {
        std::cout << " (camera frames " << min_camera_frame << "-"
                  << max_camera_frame << ")";
    }
    std::cout << std::endl;

    if (!data_.chaser_bounding_boxes.empty()) {
        const auto& sample = data_.chaser_bounding_boxes.front();
        std::cout << "  [ChaserBBox] Sample: camera_frame=" << sample.camera_frame_id
                  << ", stimulus_frame=" << sample.stimulus_frame_num
                  << ", fish_id=" << sample.fish_id
                  << ", x=" << sample.x_px << ", y=" << sample.y_px
                  << ", w=" << sample.width_px << ", h=" << sample.height_px
                  << (sample.is_target ? " [target]" : "")
                  << std::endl;
    }

    return true;
}

bool ZarrDetectionLoader::loadChaserStates(const ts::kvstore::KvStore& store,
                                           const std::string& run_base) {
    const std::string base = run_base + "tracking_data/chaser_states/";
    data_.chaser_states.clear();
    data_.chaser_states_by_camera_frame.clear();
    data_.chaser_states_by_stimulus_frame.clear();
    data_.has_chaser_states = false;

    auto column_exists = [&](const std::string& name) {
        if (arrayExists(store, base + name)) {
            return true;
        }
        if (root_path_.empty()) {
            return false;
        }
        namespace fs = std::filesystem;
        fs::path probe = fs::path(root_path_) / base / name / "zarr.json";
        return fs::exists(probe);
    };

    if (!column_exists("stimulus_frame_num") || !column_exists("chaser_pos_x")) {
        if (!column_exists("chaser_pos_x")) {
            std::cout << "  [Chaser] No chaser state arrays found at '" << base << "'" << std::endl;
        }
        return false;
    }

    auto readInt32Or64Column = [&](const std::vector<std::string>& names,
                                   std::vector<int32_t>& dest) -> bool {
        for (const auto& name : names) {
            if (readInt32Array(store, base + name, dest) && !dest.empty()) {
                return true;
            }
            std::vector<int64_t> tmp64;
            if (readInt64Array(store, base + name, tmp64) && !tmp64.empty()) {
                dest.resize(tmp64.size());
                for (size_t i = 0; i < tmp64.size(); ++i) {
                    dest[i] = clampToInt32(tmp64[i]);
                }
                return true;
            }
        }
        return false;
    };

    std::vector<int32_t> stimulus_frames;
    if (!readInt32Or64Column({"stimulus_frame_num"}, stimulus_frames) || stimulus_frames.empty()) {
        std::cout << "  [Chaser] Failed to read stimulus_frame_num column at '" << base << "'" << std::endl;
        return false;
    }
    size_t count = stimulus_frames.size();

    std::vector<float> chaser_pos_x;
    std::vector<float> chaser_pos_y;
    if (!readFloatArray(store, base + "chaser_pos_x", chaser_pos_x) || chaser_pos_x.empty()) {
        std::cout << "  [Chaser] Missing required column 'chaser_pos_x' at '" << base << "'" << std::endl;
        return false;
    }
    if (!readFloatArray(store, base + "chaser_pos_y", chaser_pos_y) || chaser_pos_y.empty()) {
        std::cout << "  [Chaser] Missing required column 'chaser_pos_y' at '" << base << "'" << std::endl;
        return false;
    }

    auto readOptionalFloatColumn = [&](const std::string& name, std::vector<float>& out) {
        if (!readFloatArray(store, base + name, out)) {
            out.clear();
            return false;
        }
        return true;
    };

    std::vector<float> target_pos_x;
    std::vector<float> target_pos_y;
    readOptionalFloatColumn("target_pos_x", target_pos_x);
    readOptionalFloatColumn("target_pos_y", target_pos_y);

    std::vector<float> chaser_radius_px;
    std::vector<float> distance_to_target_px;
    std::vector<float> target_speed_px_per_s;
    readOptionalFloatColumn("chaser_radius_px", chaser_radius_px);
    readOptionalFloatColumn("distance_to_target_px", distance_to_target_px);
    readOptionalFloatColumn("target_speed_px_per_s", target_speed_px_per_s);

    std::vector<int32_t> camera_frames;
    if (column_exists("camera_frame_id") || column_exists("triggering_camera_frame_id")) {
        readInt32Or64Column({"camera_frame_id", "triggering_camera_frame_id"}, camera_frames);
    }

    std::vector<int32_t> chaser_indices;
    readInt32Or64Column({"chaser_index"}, chaser_indices);

    std::vector<int64_t> timestamps_ns;
    readInt64Array(store, base + "timestamp_ns_session", timestamps_ns);

    std::vector<uint8_t> is_chasing_raw;
    readBoolArray(store, base + "is_chasing", is_chasing_raw);

    auto sync_size = [&](auto& vec, const auto& default_value) {
        if (vec.empty()) {
            vec.assign(count, default_value);
        } else if (vec.size() != count) {
            vec.resize(count, default_value);
        }
    };

    sync_size(chaser_pos_x, std::numeric_limits<float>::quiet_NaN());
    sync_size(chaser_pos_y, std::numeric_limits<float>::quiet_NaN());
    sync_size(target_pos_x, std::numeric_limits<float>::quiet_NaN());
    sync_size(target_pos_y, std::numeric_limits<float>::quiet_NaN());
    sync_size(chaser_radius_px, std::numeric_limits<float>::quiet_NaN());
    sync_size(distance_to_target_px, std::numeric_limits<float>::quiet_NaN());
    sync_size(target_speed_px_per_s, std::numeric_limits<float>::quiet_NaN());
    sync_size(camera_frames, int32_t{-1});
    sync_size(chaser_indices, int32_t{-1});
    sync_size(timestamps_ns, int64_t{0});
    sync_size(is_chasing_raw, uint8_t{0});

    data_.chaser_states.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        ZarrDetectionData::ChaserStateRecord record;
        record.stimulus_frame_num = stimulus_frames[i];
        record.camera_frame_id = camera_frames[i];
        record.chaser_index = chaser_indices[i];
        record.chaser_pos_x = chaser_pos_x[i];
        record.chaser_pos_y = chaser_pos_y[i];
        record.target_pos_x = target_pos_x[i];
        record.target_pos_y = target_pos_y[i];
        record.chaser_radius_px = chaser_radius_px[i];
        record.distance_to_target_px = distance_to_target_px[i];
        record.target_speed_px_per_s = target_speed_px_per_s[i];
        record.timestamp_ns_session = timestamps_ns[i];
        record.is_chasing = is_chasing_raw[i];
        record.texture_space = true;
        if (data_.chaser_transform.valid) {
            auto convert = [&](float tex_x, float tex_y, double& cam_x, double& cam_y) -> bool {
                if (!std::isfinite(tex_x) || !std::isfinite(tex_y)) {
                    return false;
                }
                cam_x = data_.chaser_transform.offset_x_px + static_cast<double>(tex_x) * data_.chaser_transform.scale;
                cam_y = data_.chaser_transform.offset_y_px + static_cast<double>(tex_y) * data_.chaser_transform.scale;
                return std::isfinite(cam_x) && std::isfinite(cam_y);
            };
            if (convert(record.chaser_pos_x, record.chaser_pos_y,
                        record.chaser_camera_x, record.chaser_camera_y)) {
                record.has_camera_coords = true;
            }
            double target_cam_x = std::numeric_limits<double>::quiet_NaN();
            double target_cam_y = std::numeric_limits<double>::quiet_NaN();
            if (convert(record.target_pos_x, record.target_pos_y,
                        target_cam_x, target_cam_y)) {
                record.target_camera_x = target_cam_x;
                record.target_camera_y = target_cam_y;
            }
        }
        data_.chaser_states.push_back(std::move(record));
    }

    data_.has_chaser_states = !data_.chaser_states.empty();
    if (!data_.has_chaser_states) {
        return false;
    }

    bool any_camera_frames = false;
    for (const auto& record : data_.chaser_states) {
        if (record.camera_frame_id >= 0) {
            any_camera_frames = true;
            break;
        }
    }

    if (!any_camera_frames) {
        std::vector<int32_t> stimulus_to_camera;
        if (loadStimulusFrameMetadataMapping(store, run_base, stimulus_to_camera) &&
            !stimulus_to_camera.empty()) {
            size_t updated = 0;
            for (auto& record : data_.chaser_states) {
                int32_t stim_index = record.stimulus_frame_num;
                if (stim_index < 0 || static_cast<size_t>(stim_index) >= stimulus_to_camera.size()) {
                    continue;
                }
                int32_t frame = stimulus_to_camera[stim_index];
                if (frame >= 0) {
                    record.camera_frame_id = frame;
                    ++updated;
                }
            }
            if (updated > 0) {
                std::cout << "  [Chaser] Camera frames inferred from video metadata ("
                          << updated << " records)" << std::endl;
            }
        } else {
            std::cout << "  [Chaser] Unable to resolve camera frames from video_metadata for run '"
                      << run_base << "'" << std::endl;
        }
    }

    rebuildChaserStateIndices();

    int32_t min_camera_frame = std::numeric_limits<int32_t>::max();
    int32_t max_cam_frame_observed = std::numeric_limits<int32_t>::min();
    for (const auto& record : data_.chaser_states) {
        if (record.camera_frame_id >= 0) {
            min_camera_frame = std::min(min_camera_frame, record.camera_frame_id);
            max_cam_frame_observed = std::max(max_cam_frame_observed, record.camera_frame_id);
        }
    }

    std::cout << "  [Chaser] Loaded " << data_.chaser_states.size()
              << " chaser state records";
    if (max_cam_frame_observed >= min_camera_frame && min_camera_frame != std::numeric_limits<int32_t>::max()) {
        std::cout << " (camera frames " << min_camera_frame << "-" << max_cam_frame_observed << ")";
    }
    std::cout << std::endl;

    if (!data_.chaser_states.empty()) {
        const auto& sample = data_.chaser_states.front();
        std::cout << "  [Chaser] Sample: stim_frame=" << sample.stimulus_frame_num
                  << ", camera_frame=" << sample.camera_frame_id
                  << ", chaser=(" << sample.chaser_pos_x << "," << sample.chaser_pos_y << ")"
                  << ", target=(" << sample.target_pos_x << "," << sample.target_pos_y << ")"
                  << std::endl;
    }

    return true;
}

void ZarrDetectionLoader::rebuildChaserStateIndices() {
    data_.chaser_states_by_camera_frame.clear();
    data_.chaser_states_by_stimulus_frame.clear();

    if (data_.chaser_states.empty()) {
        return;
    }

    int32_t max_camera_frame = -1;
    int32_t max_stimulus_frame = -1;
    for (const auto& record : data_.chaser_states) {
        if (record.camera_frame_id >= 0) {
            max_camera_frame = std::max(max_camera_frame, record.camera_frame_id);
        }
        if (record.stimulus_frame_num >= 0) {
            max_stimulus_frame = std::max(max_stimulus_frame, record.stimulus_frame_num);
        }
    }

    if (max_camera_frame >= 0) {
        data_.chaser_states_by_camera_frame.resize(static_cast<size_t>(max_camera_frame) + 1);
    } else {
        data_.chaser_states_by_camera_frame.resize(data_.total_frames);
    }

    if (max_stimulus_frame >= 0) {
        data_.chaser_states_by_stimulus_frame.resize(static_cast<size_t>(max_stimulus_frame) + 1);
    }

    for (size_t idx = 0; idx < data_.chaser_states.size(); ++idx) {
        const auto& record = data_.chaser_states[idx];
        if (record.camera_frame_id >= 0) {
            size_t frame_index = static_cast<size_t>(record.camera_frame_id);
            if (frame_index >= data_.chaser_states_by_camera_frame.size()) {
                data_.chaser_states_by_camera_frame.resize(frame_index + 1);
            }
            data_.chaser_states_by_camera_frame[frame_index].push_back(idx);
        }
        if (record.stimulus_frame_num >= 0) {
            size_t stim_index = static_cast<size_t>(record.stimulus_frame_num);
            if (stim_index >= data_.chaser_states_by_stimulus_frame.size()) {
                data_.chaser_states_by_stimulus_frame.resize(stim_index + 1);
            }
            data_.chaser_states_by_stimulus_frame[stim_index].push_back(idx);
        }
    }
}

void ZarrDetectionLoader::rebuildChaserBoundingBoxIndices() {
    data_.chaser_bboxes_by_camera_frame.clear();

    if (data_.chaser_bounding_boxes.empty()) {
        return;
    }

    int32_t max_camera_frame = -1;
    for (const auto& record : data_.chaser_bounding_boxes) {
        if (record.camera_frame_id >= 0) {
            max_camera_frame = std::max(max_camera_frame, record.camera_frame_id);
        }
    }

    if (max_camera_frame >= 0) {
        data_.chaser_bboxes_by_camera_frame.resize(static_cast<size_t>(max_camera_frame) + 1);
    } else if (data_.total_frames > 0) {
        data_.chaser_bboxes_by_camera_frame.resize(data_.total_frames);
    }

    for (size_t idx = 0; idx < data_.chaser_bounding_boxes.size(); ++idx) {
        const auto& record = data_.chaser_bounding_boxes[idx];
        if (record.camera_frame_id < 0) {
            continue;
        }
        size_t frame_index = static_cast<size_t>(record.camera_frame_id);
        if (frame_index >= data_.chaser_bboxes_by_camera_frame.size()) {
            data_.chaser_bboxes_by_camera_frame.resize(frame_index + 1);
        }
        data_.chaser_bboxes_by_camera_frame[frame_index].push_back(idx);
    }
}

void ZarrDetectionLoader::updateChaserCameraFramesFromAlignment() {
    if (!data_.has_chaser_states) {
        return;
    }
    const auto& mapping = data_.latest_interpolation.camera_to_metadata_index;
    if (mapping.empty()) {
        return;
    }

    size_t stim_count = 0;
    for (const auto& record : data_.chaser_states) {
        if (record.stimulus_frame_num >= 0) {
            stim_count = std::max(stim_count,
                                  static_cast<size_t>(record.stimulus_frame_num + 1));
        }
    }
    if (stim_count == 0) {
        return;
    }

    std::vector<int32_t> stimulus_to_camera(stim_count, -1);
    int32_t offset = static_cast<int32_t>(data_.stimulus_camera_frame_offset);
    for (size_t camera_frame = 0; camera_frame < mapping.size(); ++camera_frame) {
        int32_t stim_index = mapping[camera_frame];
        if (stim_index < 0 || static_cast<size_t>(stim_index) >= stimulus_to_camera.size()) {
            continue;
        }
        if (stimulus_to_camera[stim_index] == -1) {
            int32_t resolved = static_cast<int32_t>(camera_frame) + offset;
            stimulus_to_camera[stim_index] = resolved;
        }
    }

    bool updated_any = false;
    size_t updated_count = 0;
    for (auto& record : data_.chaser_states) {
        if (record.camera_frame_id >= 0) {
            continue;
        }
        int32_t stim_index = record.stimulus_frame_num;
        if (stim_index < 0 || static_cast<size_t>(stim_index) >= stimulus_to_camera.size()) {
            continue;
        }
        int32_t camera_frame = stimulus_to_camera[stim_index];
        if (camera_frame >= 0) {
            record.camera_frame_id = camera_frame;
            updated_any = true;
            ++updated_count;
        }
    }

    if (updated_any) {
        rebuildChaserStateIndices();
        std::cout << "  [Chaser] Camera frames inferred from alignment mapping "
                  << "(offset " << offset << ", updated " << updated_count << " records)"
                  << std::endl;
    }
}

bool ZarrDetectionLoader::loadStimulusFrameMetadataMapping(
    const ts::kvstore::KvStore& store,
    const std::string& run_base,
    std::vector<int32_t>& stimulus_to_camera) {

    const std::string meta_base = run_base + "video_metadata/frame_metadata/";
    auto has_column = [&](const std::string& name) {
        if (arrayExists(store, meta_base + name)) {
            return true;
        }
        if (root_path_.empty()) {
            return false;
        }
        namespace fs = std::filesystem;
        fs::path probe = fs::path(root_path_) / meta_base / name / "zarr.json";
        return fs::exists(probe);
    };

    if (!has_column("stimulus_frame_num") ||
        !has_column("triggering_camera_frame_id")) {
        return false;
    }

    std::vector<int64_t> stimulus_frames64;
    std::vector<int64_t> camera_frames64;
    if (!readInt64Array(store, meta_base + "stimulus_frame_num", stimulus_frames64) ||
        stimulus_frames64.empty()) {
        return false;
    }
    if (!readInt64Array(store, meta_base + "triggering_camera_frame_id", camera_frames64) ||
        camera_frames64.size() != stimulus_frames64.size()) {
        return false;
    }

    int64_t max_stimulus = -1;
    for (int64_t value : stimulus_frames64) {
        if (value > max_stimulus) {
            max_stimulus = value;
        }
    }
    if (max_stimulus < 0) {
        return false;
    }

    stimulus_to_camera.assign(static_cast<size_t>(max_stimulus) + 1, -1);
    size_t applied = 0;
    for (size_t i = 0; i < stimulus_frames64.size(); ++i) {
        int64_t stim64 = stimulus_frames64[i];
        int64_t cam64 = camera_frames64[i];
        if (stim64 < 0 || cam64 < 0) {
            continue;
        }
        size_t stim_index = static_cast<size_t>(stim64);
        if (stim_index >= stimulus_to_camera.size()) {
            continue;
        }
        int32_t cam32 = clampToInt32(cam64);
        if (stimulus_to_camera[stim_index] == -1) {
            stimulus_to_camera[stim_index] = cam32;
            ++applied;
        }
    }

    if (applied == 0) {
        stimulus_to_camera.clear();
        return false;
    }

    std::cout << "  [Chaser] Frame metadata mapping loaded (" << applied
              << " stimulus frames)" << std::endl;
    return true;
}

bool ZarrDetectionLoader::loadMovementData(const ts::kvstore::KvStore& store) {
    data_.has_movement_data = false;
    data_.movement_series.clear();
    data_.movement_selected_index = std::numeric_limits<size_t>::max();

    bool loaded = loadLegacyMovementData(store);

    if (!loaded) {
        return false;
    }

    finalizeMovementSelection();
    return data_.has_movement_data;
}
bool ZarrDetectionLoader::loadLegacyMovementData(const ts::kvstore::KvStore& store) {
    const std::vector<std::pair<std::string, std::string>> categories = {
        {"analysis/movement_runs/offline", "offline"},
        {"analysis/movement_runs/online_refined", "online_refined"},
        {"analysis/movement_runs/online", "online"}
    };

    bool loaded_any = false;

    for (const auto& [group_path, category_name] : categories) {
        std::vector<std::string> run_candidates;
        if (auto group_attrs = readAttrsAny(store, group_path)) {
            const std::string latest = extractLatestRunName(*group_attrs);
            if (!latest.empty()) {
                run_candidates.push_back(latest);
            }
        }
        if (!root_path_.empty()) {
            auto runs = collect_runs_fs(root_path_, group_path, {});
            run_candidates.insert(run_candidates.end(), runs.begin(), runs.end());
        }
        std::sort(run_candidates.begin(), run_candidates.end());
        run_candidates.erase(std::unique(run_candidates.begin(), run_candidates.end()), run_candidates.end());
        if (run_candidates.empty()) {
            continue;
        }

        // Only load the most recent run for legacy data
        const std::string& run_name = run_candidates.back();
        std::string run_base = group_path + "/" + run_name + "/";

        float pixels_per_mm = 0.0f;
        std::vector<std::string> track_ids;
        auto append_track = [&](const std::string& track) {
            if (!track.empty()) {
                track_ids.push_back(track);
            }
        };

        if (auto run_attrs = readAttrsAny(store, run_base)) {
            auto readPixelsPerMm = [&](const char* key) {
                if (run_attrs->contains(key) && (*run_attrs)[key].is_number()) {
                    pixels_per_mm = static_cast<float>((*run_attrs)[key].get<double>());
                }
            };
            readPixelsPerMm("pixels_per_mm");
            readPixelsPerMm("pixels_per_mm_camera");
            readPixelsPerMm("pixel_to_mm");

            const std::vector<std::string> track_keys = {"primary_track", "default_track", "track_id"};
            for (const auto& key : track_keys) {
                if (run_attrs->contains(key)) {
                    const auto& value = (*run_attrs)[key];
                    if (value.is_string()) {
                        append_track(value.get<std::string>());
                    } else if (value.is_number_integer()) {
                        append_track("id_" + std::to_string(value.get<int>()));
                    }
                }
            }
        }

        if (!root_path_.empty()) {
            namespace fs = std::filesystem;
            fs::path track_root = fs::path(root_path_) / run_base / "tracks";
            if (fs::exists(track_root) && fs::is_directory(track_root)) {
                for (const auto& entry : fs::directory_iterator(track_root)) {
                    if (entry.is_directory()) {
                        append_track(entry.path().filename().string());
                    }
                }
            }
        }

        if (track_ids.empty()) {
            append_track("id_0");
        }

        std::sort(track_ids.begin(), track_ids.end());
        track_ids.erase(std::unique(track_ids.begin(), track_ids.end()), track_ids.end());

        std::vector<int64_t> run_camera_frame_ids;
        readInt64Array(store, run_base + "camera_frame_ids", run_camera_frame_ids);

        auto readDistanceArray = [&](const std::vector<std::string>& names,
                                     std::vector<float>& dest) -> bool {
            for (const auto& name : names) {
                if (readFloatArray(store, run_base + name, dest) && !dest.empty()) {
                    return true;
                }
            }
            dest.clear();
            return false;
        };

        auto loadDistanceValues = [&](const std::vector<std::string>& mm_names,
                                      const std::vector<std::string>& px_names,
                                      std::vector<float>& dest,
                                      const char* label) -> bool {
            if (readDistanceArray(mm_names, dest)) {
                return true;
            }
            if (!px_names.empty()) {
                std::vector<float> px;
                if (readDistanceArray(px_names, px) && !px.empty()) {
                    if (pixels_per_mm > 1e-6f) {
                        dest.resize(px.size());
                        for (size_t i = 0; i < px.size(); ++i) {
                            dest[i] = px[i] / pixels_per_mm;
                        }
                        return true;
                    }
                    std::cout << "  [LegacyMovement] Unable to convert " << label << " for run '"
                              << run_name << "' (pixels_per_mm missing)" << std::endl;
                }
            }
            return false;
        };

        const std::vector<std::string> smoothed_distance_mm_names = {"distance_to_target_smoothed_mm"};
        const std::vector<std::string> smoothed_distance_px_names = {"distance_to_target_smoothed_px"};
        const std::vector<std::string> raw_distance_mm_names = {"distance_to_target_mm"};
        const std::vector<std::string> raw_distance_px_names = {"distance_to_target_px"};

        std::vector<float> run_distance_to_target_mm;
        bool using_smoothed_distance = loadDistanceValues(
                                       smoothed_distance_mm_names,
                                       smoothed_distance_px_names,
                                       run_distance_to_target_mm,
                                       "distance_to_target_smoothed_px");
        bool run_distance_loaded = using_smoothed_distance;
        if (!run_distance_loaded) {
            run_distance_loaded = loadDistanceValues(
                raw_distance_mm_names,
                raw_distance_px_names,
                run_distance_to_target_mm,
                "distance_to_target_px");
            using_smoothed_distance = false;
        }
        const char* distance_label = using_smoothed_distance ? "distance_to_target_smoothed_mm"
                                                             : "distance_to_target_mm";

        std::vector<uint8_t> run_has_offline_flags;
        readBoolArray(store, run_base + "has_offline", run_has_offline_flags);

        std::unordered_map<int64_t, size_t> run_camera_lookup;
        const std::vector<int64_t>* run_camera_ptr = nullptr;
        const std::unordered_map<int64_t, size_t>* run_lookup_ptr = nullptr;
        const std::vector<float>* run_distance_ptr = nullptr;
        const std::vector<uint8_t>* run_offline_ptr = nullptr;

        if (!run_camera_frame_ids.empty()) {
            run_camera_ptr = &run_camera_frame_ids;
            run_camera_lookup.reserve(run_camera_frame_ids.size());
            for (size_t i = 0; i < run_camera_frame_ids.size(); ++i) {
                run_camera_lookup.emplace(run_camera_frame_ids[i], i);
            }
            if (!run_camera_lookup.empty()) {
                run_lookup_ptr = &run_camera_lookup;
            }
        }

        if (run_camera_ptr && !run_distance_to_target_mm.empty()) {
            if (run_distance_to_target_mm.size() == run_camera_frame_ids.size()) {
                run_distance_ptr = &run_distance_to_target_mm;
            } else {
                std::cout << "  [LegacyMovement] Ignoring " << distance_label << " for run '"
                          << run_name << "' due to size mismatch (camera_frame_ids="
                          << run_camera_frame_ids.size()
                          << ", " << distance_label << "=" << run_distance_to_target_mm.size()
                          << ")" << std::endl;
            }
        }

        if (run_distance_ptr && !run_has_offline_flags.empty()) {
            if (run_has_offline_flags.size() == run_distance_to_target_mm.size()) {
                run_offline_ptr = &run_has_offline_flags;
            } else {
                std::cout << "  [LegacyMovement] Ignoring has_offline mask for run '"
                          << run_name << "' due to size mismatch (" << distance_label << "="
                          << run_distance_to_target_mm.size()
                          << ", has_offline=" << run_has_offline_flags.size() << ")"
                          << std::endl;
            }
        }

        const std::vector<std::string> frame_names = {"frames", "frame_indices"};
        const std::vector<std::string> time_float_names = {"time_seconds"};
        const std::vector<std::string> timestamp_ns_names = {"timestamp_ns_session"};
        const std::vector<std::string> smoothed_mm_names = {"smoothed_speed_mm", "smoothed_speed_mm_per_s", "smoothed_speed_mmps"};
        const std::vector<std::string> smoothed_px_names = {"smoothed_speed_px", "smoothed_speed_px_per_s", "smoothed_speed_pxps"};
        const std::vector<std::string> instant_mm_names = {"instantaneous_speed_mm", "instantaneous_speed_mm_per_s", "instantaneous_speed_mmps"};
        const std::vector<std::string> instant_px_names = {"instantaneous_speed_px", "instantaneous_speed_px_per_s", "instantaneous_speed_pxps"};

        for (const auto& track_id : track_ids) {
            std::string track_base = run_base + "tracks/" + track_id + "/";
            bool loaded_track = loadMovementTrack(
                store,
                "[LegacyMovement]",
                run_name,
                track_id,
                track_base,
                frame_names,
                time_float_names,
                timestamp_ns_names,
                smoothed_mm_names,
                smoothed_px_names,
                instant_mm_names,
                instant_px_names,
                pixels_per_mm,
                0.0,
                category_name,
                category_name,
                std::string(),
                0.0,
                0,
                0,
                false,
                run_camera_ptr,
                run_lookup_ptr,
                run_distance_ptr,
                run_offline_ptr);
            loaded_any = loaded_any || loaded_track;
        }
    }

    return loaded_any;
}

bool ZarrDetectionLoader::loadMovementTrack(
    const ts::kvstore::KvStore& store,
    const std::string& log_tag,
    const std::string& run_name,
    const std::string& track_id,
    const std::string& track_base,
    const std::vector<std::string>& frame_names,
    const std::vector<std::string>& time_float_names,
    const std::vector<std::string>& timestamp_ns_names,
    const std::vector<std::string>& smoothed_mm_names,
    const std::vector<std::string>& smoothed_px_names,
    const std::vector<std::string>& instant_mm_names,
    const std::vector<std::string>& instant_px_names,
    float pixels_per_mm,
    double run_fps,
    const std::string& category,
    const std::string& detection_variant,
    const std::string& source_detect_run,
    double smoothing_seconds,
    int video_width,
    int video_height,
    bool from_speed_runs,
    const std::vector<int64_t>* run_camera_frame_ids,
    const std::unordered_map<int64_t, size_t>* run_camera_lookup,
    const std::vector<float>* run_distance_to_target_mm,
    const std::vector<uint8_t>* run_has_offline_flags) {

    auto readInt32Or64List = [&](const std::vector<std::string>& candidates,
                                 std::vector<int32_t>& dest) {
        dest.clear();
        for (const auto& name : candidates) {
            if (readInt32Array(store, track_base + name, dest) && !dest.empty()) {
                return true;
            }
            std::vector<int64_t> tmp64;
            if (readInt64Array(store, track_base + name, tmp64) && !tmp64.empty()) {
                dest.resize(tmp64.size());
                for (size_t i = 0; i < tmp64.size(); ++i) {
                    dest[i] = clampToInt32(tmp64[i]);
                }
                return true;
            }
        }
        return false;
    };

    std::vector<int32_t> frame_indices;
    readInt32Or64List(frame_names, frame_indices);

    std::vector<float> time_seconds;
    bool time_loaded = false;
    for (const auto& name : time_float_names) {
        if (readFloatArray(store, track_base + name, time_seconds) && !time_seconds.empty()) {
            time_loaded = true;
            break;
        }
    }
    if (!time_loaded) {
        std::vector<int64_t> timestamps_ns;
        for (const auto& name : timestamp_ns_names) {
            if (readInt64Array(store, track_base + name, timestamps_ns) && !timestamps_ns.empty()) {
                time_seconds.resize(timestamps_ns.size());
                constexpr double kNsToSeconds = 1e-9;
                for (size_t i = 0; i < timestamps_ns.size(); ++i) {
                    time_seconds[i] = static_cast<float>(timestamps_ns[i] * kNsToSeconds);
                }
                time_loaded = true;
                break;
            }
        }
    }
    if (!time_loaded && run_fps > 0.0 && !frame_indices.empty()) {
        time_seconds.resize(frame_indices.size());
        double inv_fps = 1.0 / run_fps;
        for (size_t i = 0; i < frame_indices.size(); ++i) {
            time_seconds[i] = static_cast<float>(static_cast<double>(frame_indices[i]) * inv_fps);
        }
        time_loaded = true;
    }
    if (!time_loaded) {
        return false;
    }

    auto readSpeedArray = [&](const std::vector<std::string>& names,
                              std::vector<float>& dest) -> bool {
        for (const auto& name : names) {
            if (readFloatArray(store, track_base + name, dest) && !dest.empty()) {
                return true;
            }
        }
        dest.clear();
        return false;
    };

    auto readSpeedValues = [&](const std::vector<std::string>& mm_names,
                               const std::vector<std::string>& px_names,
                               std::vector<float>& dest,
                               const char* label) -> bool {
        if (readSpeedArray(mm_names, dest)) {
            return true;
        }
        if (!px_names.empty()) {
            std::vector<float> px;
            if (readSpeedArray(px_names, px) && !px.empty()) {
                if (pixels_per_mm > 1e-6f) {
                    dest.resize(px.size());
                    for (size_t i = 0; i < px.size(); ++i) {
                        dest[i] = px[i] / pixels_per_mm;
                    }
                    return true;
                }
                std::cout << "  " << log_tag << " Unable to convert " << label
                          << " for run '" << run_name << "' track '" << track_id
                          << "' (pixels_per_mm missing)" << std::endl;
            }
        }
        return false;
    };

    std::vector<float> smoothed_mm;
    std::vector<float> instant_mm;
    bool has_smoothed = readSpeedValues(smoothed_mm_names, smoothed_px_names, smoothed_mm, "smoothed speed");
    bool has_instant = readSpeedValues(instant_mm_names, instant_px_names, instant_mm, "instantaneous speed");

    std::vector<float> heading_degrees;
    readFloatArray(store, track_base + "heading_degrees", heading_degrees);

    std::vector<float> smoothed_heading_degrees;
    readFloatArray(store, track_base + "smoothed_heading_degrees", smoothed_heading_degrees);

    std::vector<uint8_t> keypoint_success;
    readBoolArray(store, track_base + "keypoint_success", keypoint_success);

    std::vector<float> heading_per_second_degrees;
    readFloatArray(store, track_base + "heading_per_second_degrees", heading_per_second_degrees);

    std::vector<float> heading_per_second_resultant;
    readFloatArray(store, track_base + "heading_per_second_resultant", heading_per_second_resultant);

    std::vector<float> heading_per_second_time_seconds;
    const std::vector<std::string> heading_per_second_time_names = {
        "heading_per_second_time_seconds",
        "heading_per_second_seconds"
    };
    for (const auto& name : heading_per_second_time_names) {
        if (readFloatArray(store, track_base + name, heading_per_second_time_seconds) &&
            !heading_per_second_time_seconds.empty()) {
            break;
        }
    }

    if (!has_smoothed && !has_instant) {
        return false;
    }

    size_t sample_count = time_seconds.size();
    if (has_smoothed) {
        sample_count = std::min(sample_count, smoothed_mm.size());
    }
    if (has_instant) {
        sample_count = std::min(sample_count, instant_mm.size());
    }
    if (!heading_degrees.empty()) {
        sample_count = std::min(sample_count, heading_degrees.size());
    }
    if (!smoothed_heading_degrees.empty()) {
        sample_count = std::min(sample_count, smoothed_heading_degrees.size());
    }
    if (!keypoint_success.empty()) {
        sample_count = std::min(sample_count, keypoint_success.size());
    }
    if (!frame_indices.empty()) {
        sample_count = std::min(sample_count, frame_indices.size());
    }
    if (sample_count == 0) {
        return false;
    }

    auto trim_to = [&](auto& vec) {
        if (!vec.empty() && vec.size() > sample_count) {
            vec.resize(sample_count);
        }
    };
    trim_to(time_seconds);
    trim_to(smoothed_mm);
    trim_to(instant_mm);
    trim_to(heading_degrees);
    trim_to(smoothed_heading_degrees);
    trim_to(keypoint_success);
    trim_to(frame_indices);

    std::vector<float> distance_series;
    if (run_camera_frame_ids && run_camera_lookup && run_distance_to_target_mm &&
        !run_camera_frame_ids->empty() && !run_distance_to_target_mm->empty()) {
        const auto& camera_ids = *run_camera_frame_ids;
        const auto& distance_mm = *run_distance_to_target_mm;
        const std::vector<uint8_t>* has_offline = nullptr;
        if (run_has_offline_flags &&
            run_has_offline_flags->size() == distance_mm.size()) {
            has_offline = run_has_offline_flags;
        }

        distance_series.assign(time_seconds.size(), std::numeric_limits<float>::quiet_NaN());
        bool any_valid = false;

        for (size_t i = 0; i < frame_indices.size() && i < distance_series.size(); ++i) {
            int32_t frame = frame_indices[i];
            auto lookup_it = run_camera_lookup->find(static_cast<int64_t>(frame));
            if (lookup_it == run_camera_lookup->end()) {
                continue;
            }
            size_t run_idx = lookup_it->second;
            if (run_idx >= distance_mm.size()) {
                continue;
            }
            if (has_offline && (run_idx >= has_offline->size() || (*has_offline)[run_idx] == 0)) {
                continue;
            }
            float value = distance_mm[run_idx];
            if (!std::isfinite(static_cast<double>(value))) {
                continue;
            }
            distance_series[i] = value;
            any_valid = true;
        }

        if (!any_valid) {
            distance_series.clear();
        }
    }

    ZarrDetectionData::MovementSeries series;
    series.category = category;
    series.run_name = run_name;
    series.track_id = track_id;
    series.detection_variant = detection_variant;
    series.source_detect_run = source_detect_run;
    series.fps = run_fps;
    series.smoothing_seconds = smoothing_seconds;
    series.video_width = video_width;
    series.video_height = video_height;
    series.from_speed_runs = from_speed_runs;
    series.time_seconds = std::move(time_seconds);
    series.smoothed_speed_mm = std::move(smoothed_mm);
    series.instant_speed_mm = std::move(instant_mm);
    series.heading_degrees = std::move(heading_degrees);
    series.smoothed_heading_degrees = std::move(smoothed_heading_degrees);
    series.keypoint_success = std::move(keypoint_success);
    series.frame_indices = std::move(frame_indices);
    if (!distance_series.empty()) {
        series.distance_to_target_mm = std::move(distance_series);
    }
    if (!heading_per_second_degrees.empty()) {
        series.heading_per_second_degrees = std::move(heading_per_second_degrees);
    }
    if (!heading_per_second_resultant.empty()) {
        series.heading_per_second_resultant = std::move(heading_per_second_resultant);
    }
    if (!heading_per_second_time_seconds.empty()) {
        series.heading_per_second_time_seconds = std::move(heading_per_second_time_seconds);
    }

    data_.movement_series.push_back(std::move(series));
    std::cout << "  " << log_tag << " Loaded run '" << run_name << "' track '" << track_id
              << "' (" << category << ", samples " << sample_count << ")"
              << std::endl;
    return true;
}

void ZarrDetectionLoader::cacheDetectionStage(InterpolationRunData stage,
                                              DetectionDataset dataset_type) {
    switch (dataset_type) {
        case DetectionDataset::RawDetect:
            data_.raw_detection_dataset = std::move(stage);
            data_.has_raw_detection_dataset = true;
            break;
        case DetectionDataset::RefinedFiltered:
            data_.refined_filtered_dataset = std::move(stage);
            data_.has_refined_filtered_dataset = true;
            break;
        case DetectionDataset::RefinedInterpolated:
            data_.refined_interpolated_dataset = std::move(stage);
            data_.has_refined_interpolated_dataset = true;
            break;
        case DetectionDataset::RefinedRoot:
            data_.refined_root_dataset = std::move(stage);
            data_.has_refined_root_dataset = true;
            break;
    }
}

void ZarrDetectionLoader::computeDetectionsFromOffsets(
    const std::vector<size_t>& offsets,
    std::vector<int32_t>& n_detections_out) const {
    size_t frame_count = offsets.size() > 0 ? offsets.size() - 1 : 0;
    n_detections_out.assign(frame_count, 0);
    for (size_t frame = 0; frame < frame_count; ++frame) {
        size_t start = offsets[frame];
        size_t end = offsets[frame + 1];
        size_t count = (end >= start) ? (end - start) : 0;
        n_detections_out[frame] = static_cast<int32_t>(count);
    }
}

void ZarrDetectionLoader::computeActiveDatasetInterpolationFlags() {
    size_t frame_count = 0;
    if (!data_.frame_offsets.empty()) {
        frame_count = data_.frame_offsets.size() > 0 ? data_.frame_offsets.size() - 1 : 0;
    }
    if (frame_count == 0 && !data_.n_detections.empty()) {
        frame_count = data_.n_detections.size();
    }

    size_t total_detections = 0;
    if (!data_.frame_offsets.empty()) {
        total_detections = data_.frame_offsets.back();
    } else if (!data_.frame_indices.empty()) {
        total_detections = data_.frame_indices.size();
    }

    data_.frame_interpolated_flags.clear();

    if (!data_.detection_source_flags.empty() &&
        total_detections == data_.detection_source_flags.size() &&
        !data_.frame_offsets.empty()) {
        data_.frame_interpolated_flags.assign(frame_count, 0);
        for (size_t frame = 0; frame < frame_count; ++frame) {
            size_t start = data_.frame_offsets[frame];
            size_t end = data_.frame_offsets[frame + 1];
            bool any_interp = false;
            for (size_t idx = start; idx < end && idx < data_.detection_source_flags.size(); ++idx) {
                if (data_.detection_source_flags[idx] != 0) {
                    any_interp = true;
                    break;
                }
            }
            data_.frame_interpolated_flags[frame] = any_interp ? 1 : 0;
        }
    } else {
        data_.frame_interpolated_flags.clear();
    }
}

bool ZarrDetectionLoader::applyDetectionDataset(const InterpolationRunData& stage,
                                                DetectionDataset dataset_type) {
    if (stage.frame_indices.empty() && stage.n_detections.empty() &&
        stage.frame_offsets.empty()) {
        return false;
    }

    data_.frame_indices = stage.frame_indices;
    data_.bbox_norm_coords = stage.bbox_norm_coords;
    data_.flat_scores = stage.flat_scores;
    data_.flat_class_ids = stage.flat_class_ids;
    data_.frame_offsets = stage.frame_offsets;
    data_.detection_source_flags = stage.detection_source;
    data_.has_scores = stage.has_scores;
    data_.has_class_ids = stage.has_class_ids;
    data_.coordinates_normalized = stage.uses_palette_layout;
    data_.layout = ZarrLayoutType::kPaletteRuns;

    data_.n_detections = stage.n_detections;
    if (data_.n_detections.empty()) {
        computeDetectionsFromOffsets(data_.frame_offsets, data_.n_detections);
    }

    if (data_.frame_offsets.empty() && !data_.n_detections.empty()) {
        data_.frame_offsets.assign(data_.n_detections.size() + 1, 0);
        size_t running = 0;
        for (size_t frame = 0; frame < data_.n_detections.size(); ++frame) {
            running += static_cast<size_t>(std::max(data_.n_detections[frame], 0));
            data_.frame_offsets[frame + 1] = running;
        }
    }

    if (data_.frame_offsets.empty() && !data_.frame_indices.empty()) {
        int32_t max_index = -1;
        for (int32_t idx : data_.frame_indices) {
            max_index = std::max(max_index, idx);
        }
        size_t count = max_index >= 0 ? static_cast<size_t>(max_index + 1) : 0;
        data_.frame_offsets.assign(count + 1, 0);
        std::vector<int32_t> counts(count, 0);
        for (int32_t idx : data_.frame_indices) {
            if (idx >= 0 && static_cast<size_t>(idx) < counts.size()) {
                counts[idx]++;
            }
        }
        size_t running = 0;
        for (size_t i = 0; i < counts.size(); ++i) {
            running += static_cast<size_t>(counts[i]);
            data_.frame_offsets[i + 1] = running;
        }
        if (data_.n_detections.empty()) {
            data_.n_detections = counts;
        }
    }

    if (data_.n_detections.empty()) {
        computeDetectionsFromOffsets(data_.frame_offsets, data_.n_detections);
    }

    data_.total_frames = data_.n_detections.size();
    if (data_.total_frames == 0 && data_.frame_offsets.size() > 1) {
        data_.total_frames = data_.frame_offsets.size() - 1;
    }

    data_.max_detections = 0;
    if (!data_.frame_offsets.empty()) {
        for (size_t frame = 0; frame + 1 < data_.frame_offsets.size(); ++frame) {
            size_t count = data_.frame_offsets[frame + 1] - data_.frame_offsets[frame];
            data_.max_detections = std::max(data_.max_detections, count);
        }
    }
    for (const auto count : data_.n_detections) {
        if (count >= 0) {
            data_.max_detections = std::max(
                data_.max_detections,
                static_cast<size_t>(count));
        }
    }

    data_.detect_run_name = stage.run_name;
    if (!stage.stage_label.empty()) {
        if (!data_.detect_run_name.empty()) {
            data_.detect_run_name += "/";
        }
        data_.detect_run_name += stage.stage_label;
    }
    data_.detect_run_method = stage.method.empty() ? stage.stage_label : stage.method;
    data_.detect_run_created_at = stage.created_at;
    data_.detect_run_source = stage.source_detection_run;
    data_.detect_run_provenance_json = stage.provenance_json;

    computeActiveDatasetInterpolationFlags();
    active_dataset_ = dataset_type;
    return true;
}

std::vector<std::pair<ZarrDetectionLoader::DetectionDataset, std::string>>
ZarrDetectionLoader::getAvailableDetectionDatasets() const {
    std::vector<std::pair<DetectionDataset, std::string>> result;
    auto make_label = [](const InterpolationRunData& stage,
                         const std::string& fallback) -> std::string {
        if (!stage.run_name.empty()) {
            if (!stage.stage_label.empty()) {
                return stage.run_name + "/" + stage.stage_label;
            }
            return stage.run_name;
        }
        return fallback;
    };

    if (data_.has_raw_detection_dataset) {
        std::string label = make_label(data_.raw_detection_dataset, "Detect run");
        result.emplace_back(DetectionDataset::RawDetect, std::move(label));
    }
    if (data_.has_refined_filtered_dataset) {
        std::string label = make_label(data_.refined_filtered_dataset, "Refined filtered");
        result.emplace_back(DetectionDataset::RefinedFiltered, std::move(label));
    }
    if (data_.has_refined_interpolated_dataset) {
        std::string label = make_label(data_.refined_interpolated_dataset, "Refined interpolated");
        result.emplace_back(DetectionDataset::RefinedInterpolated, std::move(label));
    }
    if (data_.has_refined_root_dataset) {
        std::string label = make_label(data_.refined_root_dataset, "Refined (root)");
        result.emplace_back(DetectionDataset::RefinedRoot, std::move(label));
    }
    return result;
}

bool ZarrDetectionLoader::isDatasetAvailable(DetectionDataset dataset) const {
    switch (dataset) {
        case DetectionDataset::RawDetect:
            return data_.has_raw_detection_dataset;
        case DetectionDataset::RefinedFiltered:
            return data_.has_refined_filtered_dataset;
        case DetectionDataset::RefinedInterpolated:
            return data_.has_refined_interpolated_dataset;
        case DetectionDataset::RefinedRoot:
            return data_.has_refined_root_dataset;
    }
    return false;
}

bool ZarrDetectionLoader::setActiveDetectionDataset(DetectionDataset dataset) {
    if (dataset == active_dataset_) {
        return true;
    }
    if (!isDatasetAvailable(dataset)) {
        return false;
    }

    switch (dataset) {
        case DetectionDataset::RawDetect:
            return applyDetectionDataset(data_.raw_detection_dataset, dataset);
        case DetectionDataset::RefinedFiltered:
            return applyDetectionDataset(data_.refined_filtered_dataset, dataset);
        case DetectionDataset::RefinedInterpolated:
            return applyDetectionDataset(data_.refined_interpolated_dataset, dataset);
        case DetectionDataset::RefinedRoot:
            return applyDetectionDataset(data_.refined_root_dataset, dataset);
    }
    return false;
}

bool ZarrDetectionLoader::activeDatasetHasSyntheticDetections() const {
    return std::any_of(data_.detection_source_flags.begin(),
                       data_.detection_source_flags.end(),
                       [](uint8_t value) { return value != 0; });
}

void ZarrDetectionLoader::finalizeMovementSelection() {
    if (data_.movement_series.empty()) {
        data_.movement_selected_index = std::numeric_limits<size_t>::max();
        data_.has_movement_data = false;
        return;
    }

    auto scoreFor = [](const std::string& value) -> int {
        std::string lower;
        lower.resize(value.size());
        std::transform(value.begin(), value.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower == "refined") return 5;
        if (lower == "offline") return 4;
        if (lower == "online_refined") return 3;
        if (lower == "online") return 2;
        if (lower == "speed_run") return 1;
        return 0;
    };

    size_t best_index = 0;
    int best_score = std::numeric_limits<int>::min();
    for (size_t i = 0; i < data_.movement_series.size(); ++i) {
        const auto& series = data_.movement_series[i];
        int score = std::max(scoreFor(series.category), scoreFor(series.detection_variant));
        if (score > best_score) {
            best_score = score;
            best_index = i;
        }
    }

    data_.movement_selected_index = best_index;
    data_.has_movement_data = true;
}

bool ZarrDetectionLoader::loadStandardFormat(const ts::kvstore::KvStore& store) {
    try {
        // Load metadata first
        if (!loadMetadata(store)) {
            std::cerr << "Warning: Could not load metadata from zarr.json/.zattrs" << std::endl;
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

        if (arrayExists(store, "n_detections")) {
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

        if (arrayExists(store, "scores")) {
            if (loadScores(store, "scores")) {
                std::cout << "  Loaded scores array" << std::endl;
                data_.has_scores = true;
            } else {
                std::cerr << "Warning: scores array exists but failed to load" << std::endl;
            }
        } else {
            std::cout << "  No scores array found (optional)" << std::endl;
        }

        if (arrayExists(store, "class_ids")) {
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
        auto open_result = openArrayAny<float, 3>(store, path, context_);

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
        auto open_result = openArrayAny<float, 2>(store, path, context_);

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
        auto open_result = openArrayAny<int32_t, 2>(store, path, context_);

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
        auto open_result = openArrayAny<int32_t, 1>(store, path, context_);

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

        bool fps_from_metadata = false;
        double duration_seconds = 0.0;
        double root_fps = 0.0;

        if (auto root_attrs = readAttrsAny(store, "")) {
            if (root_attrs->contains("fps") && (*root_attrs)["fps"].is_number()) {
                root_fps = (*root_attrs)["fps"].get<double>();
            }
        }

        if (auto attrs = readAttrsAny(store, "raw_video")) {
            metadata_found = true;

            auto assignIfString = [&](const char* key, std::string& dest) {
                if (attrs->contains(key) && (*attrs)[key].is_string()) {
                    dest = (*attrs)[key].get<std::string>();
                    return true;
                }
                return false;
            };

            if (!assignIfString("source_path", data_.video_path)) {
                assignIfString("source_video", data_.video_path);
            }

            if (attrs->contains("total_frames") && (*attrs)["total_frames"].is_number()) {
                data_.total_frames = static_cast<size_t>((*attrs)["total_frames"].get<double>());
            }
            if (attrs->contains("fps") && (*attrs)["fps"].is_number()) {
                data_.fps = (*attrs)["fps"].get<double>();
                fps_from_metadata = true;
            } else if (attrs->contains("frame_rate") && (*attrs)["frame_rate"].is_number()) {
                data_.fps = (*attrs)["frame_rate"].get<double>();
                fps_from_metadata = true;
            }
            if (attrs->contains("video_width") && (*attrs)["video_width"].is_number()) {
                data_.image_width = static_cast<int>((*attrs)["video_width"].get<double>());
            }
            if (attrs->contains("video_height") && (*attrs)["video_height"].is_number()) {
                data_.image_height = static_cast<int>((*attrs)["video_height"].get<double>());
            }
            if ((data_.image_width <= 0 || data_.image_height <= 0) &&
                attrs->contains("original_resolution") &&
                (*attrs)["original_resolution"].is_array() &&
                (*attrs)["original_resolution"].size() >= 2) {
                const auto& res = (*attrs)["original_resolution"];
                int dim0 = res[0].is_number() ? static_cast<int>(res[0].get<double>()) : 0;
                int dim1 = res[1].is_number() ? static_cast<int>(res[1].get<double>()) : 0;
                if (data_.image_height <= 0 && dim0 > 0) {
                    data_.image_height = dim0;
                }
                if (data_.image_width <= 0 && dim1 > 0) {
                    data_.image_width = dim1;
                }
            }
            if (attrs->contains("video_duration_seconds") && (*attrs)["video_duration_seconds"].is_number()) {
                duration_seconds = (*attrs)["video_duration_seconds"].get<double>();
            }
        }

        if (!fps_from_metadata && root_fps > 0.0) {
            data_.fps = root_fps;
            fps_from_metadata = true;
        }

        if (!fps_from_metadata && duration_seconds > 0.0 && data_.total_frames > 0) {
            double computed_fps = static_cast<double>(data_.total_frames) / duration_seconds;
            if (computed_fps > 0.0) {
                data_.fps = computed_fps;
                fps_from_metadata = true;
            }
        }

        if (!fps_from_metadata && data_.fps <= 0.0) {
            data_.fps = 30.0;
        }

        if (data_.image_width <= 0 || data_.image_height <= 0 || data_.total_frames == 0) {
            auto video_result = openArrayAny<uint8_t, 3>(
                store,
                "raw_video/images_full",
                context_);

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
                video_result = openArrayAny<uint8_t, 3>(
                    store,
                    "raw_video/images_ds",
                    context_);

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
            if (auto refined_attrs = readAttrsAny(store, "refined_runs")) {
                std::string latest_run = extractLatestRunName(*refined_attrs);

                if (!latest_run.empty()) {
                    std::string base_path = "refined_runs/" + latest_run + "/";
                    std::string subgroup;

                    if (arrayExists(store, base_path + "interpolated/frame_indices")) {
                        subgroup = "interpolated";
                    } else if (arrayExists(store, base_path + "filtered/frame_indices")) {
                        subgroup = "filtered";
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

        std::vector<std::string> run_names;
        if (!root_path_.empty()) {
            run_names = collect_runs_fs(
                root_path_,
                "interpolation_runs",
                {"bboxes", "interpolation_mask"});
        }

        run_names.erase(
            std::remove_if(
                run_names.begin(),
                run_names.end(),
                [](const std::string& name) {
                    return name.rfind("interp_", 0) != 0;
                }),
            run_names.end());

        if (run_names.empty()) {
            std::cout << "  No interpolation runs found" << std::endl;
            return false;
        }

        std::cout << "  Found " << run_names.size() << " interpolation run(s)" << std::endl;
        std::string latest_run = run_names.back();
        std::cout << "  Loading latest run: " << latest_run << std::endl;

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
        data_.latest_interpolation.run_name = run_name;
        data_.latest_interpolation.method.clear();
        data_.latest_interpolation.created_at.clear();

        bool metadata_loaded = false;
        if (auto attrs = readAttrsAny(store, base_path)) {
            metadata_loaded = true;
            if (attrs->contains("created_at") && (*attrs)["created_at"].is_string()) {
                data_.latest_interpolation.created_at = (*attrs)["created_at"].get<std::string>();
            }
            if (data_.latest_interpolation.created_at.empty() &&
                attrs->contains("created_at_utc") && (*attrs)["created_at_utc"].is_string()) {
                data_.latest_interpolation.created_at =
                    (*attrs)["created_at_utc"].get<std::string>();
            }
            if (attrs->contains("method") && (*attrs)["method"].is_string()) {
                data_.latest_interpolation.method = (*attrs)["method"].get<std::string>();
            }
            std::cout << "    Loaded interpolation metadata from zarr.json/.zattrs" << std::endl;
        } else {
            std::cout << "    No zarr.json/.zattrs metadata found, using defaults" << std::endl;
        }

        if (metadata_loaded) {
            if (data_.latest_interpolation.method.empty()) {
                data_.latest_interpolation.method = "linear";
            }
        } else {
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

        if (!data_.latest_interpolation.created_at.empty()) {
            std::cout << "    Created: " << data_.latest_interpolation.created_at << std::endl;
        }
        std::cout << "    Method: " << data_.latest_interpolation.method << std::endl;
        
        // Now load the actual data arrays - these are required
        
        std::cout << "    Loading interpolated bboxes from: " << base_path + "bboxes" << std::endl;
        
        auto bbox_result = openArrayAny<float, 3>(
            store,
            base_path + "bboxes",
            context_);
        
        if (!bbox_result.ok()) {
            std::cerr << "Failed to load interpolated bboxes: " << bbox_result.status().ToString() << std::endl;
            return false;
        }
        
        data_.latest_interpolation.bboxes_store = bbox_result.value();
        std::cout << "    Successfully loaded interpolated bboxes" << std::endl;
        
        // Load interpolation mask (true = interpolated, false = original)
        std::cout << "    Loading interpolation mask from: " << base_path + "interpolation_mask" << std::endl;
        
        auto mask_result = openArrayAny<bool, 1>(
            store,
            base_path + "interpolation_mask",
            context_);
        
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
    if (frame_id < data_.frame_interpolated_flags.size()) {
        return data_.frame_interpolated_flags[frame_id] != 0;
    }

    if (active_dataset_ != DetectionDataset::RefinedInterpolated) {
        return false;
    }

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
    size_t frame_id, bool use_interpolated, bool include_eye_masks) const {
    
    FrameDetections result;
    result.frame_id = frame_id;
    result.includes_eye_masks = false;
    const float nan_value = std::numeric_limits<float>::quiet_NaN();
    
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
        const bool can_use_headings =
            data_.has_heading_data && !want_interpolated;
        const bool can_use_eye_masks =
            include_eye_masks && data_.has_eye_masks &&
            data_.eye_masks_loaded && !want_interpolated;
        const bool can_use_keypoints =
            data_.has_keypoints && !want_interpolated &&
            data_.keypoints_per_detection > 0 &&
            !data_.flat_keypoints_px.empty();

        if (offsets.empty() || frame_id + 1 >= offsets.size()) {
            return result;
        }

        size_t start = offsets[frame_id];
        size_t end = offsets[frame_id + 1];

        if (can_use_headings) {
            result.headings_deg.reserve(end - start);
            result.swim_bladder_pixels.reserve(end - start);
            result.heading_valid.reserve(end - start);
        }
        if (can_use_eye_masks) {
            result.eye_masks.reserve(end - start);
            result.includes_eye_masks = true;
        }
        if (can_use_keypoints) {
            result.keypoints_pixels.reserve(end - start);
            result.keypoint_labels = data_.keypoint_labels;
            result.keypoints_per_detection = data_.keypoints_per_detection;
            result.has_keypoints = false;
        }
        if (!data_.detection_source_flags.empty()) {
            result.detection_source.reserve(end - start);
        }

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

            if (!data_.detection_source_flags.empty()) {
                if (idx < data_.detection_source_flags.size()) {
                    result.detection_source.push_back(data_.detection_source_flags[idx]);
                } else {
                    result.detection_source.push_back(0);
                }
            }

            if (can_use_keypoints) {
                std::vector<std::array<float, 2>> kp_set(
                    data_.keypoints_per_detection,
                    std::array<float, 2>{nan_value, nan_value});
                size_t stride = data_.keypoints_per_detection * 2;
                size_t base = idx * stride;
                if (base + stride <= data_.flat_keypoints_px.size()) {
                    bool detection_has_points = false;
                    for (size_t kp_idx = 0; kp_idx < data_.keypoints_per_detection; ++kp_idx) {
                        float px = data_.flat_keypoints_px[base + kp_idx * 2 + 0];
                        float py = data_.flat_keypoints_px[base + kp_idx * 2 + 1];
                        kp_set[kp_idx][0] = px;
                        kp_set[kp_idx][1] = py;
                        if (std::isfinite(px) && std::isfinite(py)) {
                            detection_has_points = true;
                        }
                    }
                    if (detection_has_points) {
                        result.has_keypoints = true;
                    }
                }
                result.keypoints_pixels.push_back(std::move(kp_set));
            }

            if (can_use_headings) {
                float heading = (idx < data_.flat_headings_deg.size())
                    ? data_.flat_headings_deg[idx]
                    : 0.0f;
                uint8_t valid = (idx < data_.flat_heading_valid.size())
                    ? data_.flat_heading_valid[idx]
                    : 0;
                std::array<float, 2> anchor = {nan_value, nan_value};
                if (idx < data_.flat_swim_bladder_px.size()) {
                    anchor = data_.flat_swim_bladder_px[idx];
                }
                if (std::isnan(anchor[0]) || std::isnan(anchor[1])) {
                    float cx = pixel_box[0] + 0.5f * (pixel_box[2] - pixel_box[0]);
                    float cy = pixel_box[1] + 0.5f * (pixel_box[3] - pixel_box[1]);
                    anchor = {cx, cy};
                }
                result.headings_deg.push_back(heading);
                result.swim_bladder_pixels.push_back(anchor);
                result.heading_valid.push_back(valid);
            }

            if (can_use_eye_masks) {
                FrameDetections::EyeMask mask_entry;
                mask_entry.offset_x =
                    (idx < data_.roi_offset_x.size()) ? data_.roi_offset_x[idx] : nan_value;
                mask_entry.offset_y =
                    (idx < data_.roi_offset_y.size()) ? data_.roi_offset_y[idx] : nan_value;
                mask_entry.roi_width =
                    (idx < data_.roi_width_px.size()) ? data_.roi_width_px[idx] : 0.0f;
                mask_entry.roi_height =
                    (idx < data_.roi_height_px.size()) ? data_.roi_height_px[idx] : 0.0f;
                mask_entry.rows = static_cast<int>(data_.eye_mask_height);
                mask_entry.cols = static_cast<int>(data_.eye_mask_width);

                int32_t roi_lookup =
                    (idx < data_.mask_roi_indices.size()) ? data_.mask_roi_indices[idx] : -1;
                mask_entry.roi_index = roi_lookup;
                bool offsets_valid = std::isfinite(mask_entry.offset_x) && std::isfinite(mask_entry.offset_y);
                bool dims_valid = (mask_entry.roi_width > 0.0f && mask_entry.roi_height > 0.0f);

                if (roi_lookup >= 0 &&
                    static_cast<size_t>(roi_lookup) < data_.eye_mask_roi_count &&
                    offsets_valid && dims_valid) {
                    populateEyeMaskEntry(static_cast<size_t>(roi_lookup), mask_entry);
                }
                if (data_.has_eye_angles && roi_lookup >= 0) {
                    size_t roi_idx = static_cast<size_t>(roi_lookup);
                    bool roi_valid = data_.eye_angle_valid_mask.empty() ||
                                     (roi_idx < data_.eye_angle_valid_mask.size() &&
                                      data_.eye_angle_valid_mask[roi_idx] != 0);
                    if (roi_idx < data_.eye_angle_left_deg.size()) {
                        float left_angle = data_.eye_angle_left_deg[roi_idx];
                        mask_entry.feret_minor_angle_deg[0] = left_angle;
                        mask_entry.feret_angle_valid[0] =
                            (roi_valid && std::isfinite(left_angle)) ? 1 : 0;
                    }
                    if (roi_idx < data_.eye_angle_right_deg.size()) {
                        float right_angle = data_.eye_angle_right_deg[roi_idx];
                        mask_entry.feret_minor_angle_deg[1] = right_angle;
                        mask_entry.feret_angle_valid[1] =
                            (roi_valid && std::isfinite(right_angle)) ? 1 : 0;
                    }
                    mask_entry.has_eye_angles =
                        (mask_entry.feret_angle_valid[0] != 0) ||
                        (mask_entry.feret_angle_valid[1] != 0);
                }

                result.eye_masks.push_back(std::move(mask_entry));
            }
        }

        result.is_interpolated = isFrameInterpolated(frame_id);
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
                if (!data_.detection_source_flags.empty()) {
                    size_t idx = static_cast<size_t>(det_idx);
                    if (idx < data_.detection_source_flags.size()) {
                        result.detection_source.push_back(data_.detection_source_flags[idx]);
                    } else {
                        result.detection_source.push_back(0);
                    }
                }
                
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

std::string ZarrDetectionLoader::formatStimulusEvent(
    const ZarrDetectionData::EventLogEntry& entry) const {
    auto trim = [](std::string text) -> std::string {
        auto begin = std::find_if(text.begin(), text.end(),
                                  [](unsigned char ch) { return !std::isspace(ch); });
        auto end = std::find_if(text.rbegin(), text.rend(),
                                [](unsigned char ch) { return !std::isspace(ch); }).base();
        if (begin >= end) {
            return std::string();
        }
        return std::string(begin, end);
    };

    std::string label;
    auto it = data_.event_type_names.find(entry.event_type_id);
    if (it != data_.event_type_names.end()) {
        label = it->second;
    }
    std::string context = trim(entry.name_or_context);
    if (label.empty()) {
        label = !context.empty() ? context
                                 : ("Event " + std::to_string(entry.event_type_id));
    } else if (!context.empty() && context != label) {
        label += " - " + context;
    }

    std::string details = trim(entry.details_json);
    if (!details.empty() && details != "{}" && details != "null") {
        bool suppress_details = false;
        if ((details.front() == '[' && details.back() == ']') ||
            (details.front() == '{' && details.back() == '}')) {
            try {
                auto parsed = json::parse(details);
                if (parsed.is_structured()) {
                    suppress_details = true;
                }
            } catch (const json::parse_error&) {
                // leave suppress_details false
            }
        }
        if (!suppress_details) {
            if (details.size() > 96) {
                details.resize(93);
                details += "...";
            }
            label += " [" + details + "]";
        }
    }
    return label;
}

std::vector<std::string> ZarrDetectionLoader::getStimulusEventsForFrame(
    size_t frame_id) const {
    std::vector<std::string> result;
    if (!data_.has_stimulus_events) {
        return result;
    }
    if (frame_id < data_.stimulus_events_by_camera_frame.size()) {
        for (size_t idx : data_.stimulus_events_by_camera_frame[frame_id]) {
            if (idx >= data_.stimulus_events.size()) {
                continue;
            }
            result.push_back(formatStimulusEvent(data_.stimulus_events[idx]));
        }
    }
    if (!result.empty()) {
        return result;
    }
    if (frame_id < data_.stimulus_events_by_frame.size()) {
        for (size_t idx : data_.stimulus_events_by_frame[frame_id]) {
            if (idx >= data_.stimulus_events.size()) {
                continue;
            }
            result.push_back(formatStimulusEvent(data_.stimulus_events[idx]));
        }
    }
    return result;
}

std::vector<ZarrDetectionLoader::StimulusEventSummary>
ZarrDetectionLoader::getStimulusEventTimeline() const {
    std::vector<StimulusEventSummary> result;
    if (!data_.has_stimulus_events) {
        return result;
    }

    result.reserve(data_.stimulus_events.size());
    for (const auto& entry : data_.stimulus_events) {
        StimulusEventSummary summary;
        summary.stimulus_frame_num = entry.stimulus_frame_num;
        summary.camera_frame_id = entry.camera_frame_id;
        summary.event_type_id = entry.event_type_id;
        summary.label = formatStimulusEvent(entry);
        result.push_back(std::move(summary));
    }

    std::sort(result.begin(), result.end(),
              [](const StimulusEventSummary& a,
                 const StimulusEventSummary& b) {
                  if (a.stimulus_frame_num == b.stimulus_frame_num) {
                      return a.event_type_id < b.event_type_id;
                  }
                  return a.stimulus_frame_num < b.stimulus_frame_num;
              });
    return result;
}

std::vector<ZarrDetectionLoader::ChaserBoundingBox>
ZarrDetectionLoader::getChaserBoundingBoxesForFrame(size_t frame_id) const {
    std::vector<ChaserBoundingBox> result;
    if (!data_.has_chaser_bboxes ||
        frame_id >= data_.chaser_bboxes_by_camera_frame.size()) {
        return result;
    }

    // Use a map to keep only the latest bbox for each chaser_index
    // Key: chaser_index, Value: index in result vector
    std::unordered_map<int32_t, size_t> latest_by_chaser_index;

    for (size_t idx : data_.chaser_bboxes_by_camera_frame[frame_id]) {
        if (idx >= data_.chaser_bounding_boxes.size()) {
            continue;
        }
        const auto& src = data_.chaser_bounding_boxes[idx];
        ChaserBoundingBox box;
        box.fish_id = src.fish_id;
        box.x_px = src.x_px;
        box.y_px = src.y_px;
        box.width_px = src.width_px;
        box.height_px = src.height_px;
        box.centroid_x = src.centroid_x;
        box.centroid_y = src.centroid_y;
        box.confidence = src.confidence;
        box.camera_frame_id = src.camera_frame_id;
        box.stimulus_frame_num = src.stimulus_frame_num;
        box.chaser_index = src.chaser_index;
        box.is_target = src.is_target;

        // Deduplicate: keep only the latest stimulus frame for each chaser_index
        int32_t key = src.chaser_index;
        auto it = latest_by_chaser_index.find(key);
        if (it == latest_by_chaser_index.end()) {
            // First bbox with this chaser_index
            latest_by_chaser_index[key] = result.size();
            result.push_back(box);
        } else {
            // Already have a bbox with this chaser_index, check if this one is newer
            ChaserBoundingBox& existing = result[it->second];
            // Prefer the one with higher stimulus_frame_num (more recent)
            // If tied, prefer is_target=true, then higher confidence
            bool should_replace = false;
            if (box.stimulus_frame_num > existing.stimulus_frame_num) {
                should_replace = true;
            } else if (box.stimulus_frame_num == existing.stimulus_frame_num) {
                // Same stimulus frame, use other criteria
                if (box.is_target && !existing.is_target) {
                    should_replace = true;
                } else if (box.is_target == existing.is_target) {
                    // Both target or both not target, prefer higher confidence
                    if (std::isfinite(box.confidence) && std::isfinite(existing.confidence)) {
                        if (box.confidence > existing.confidence) {
                            should_replace = true;
                        }
                    } else if (std::isfinite(box.confidence) && !std::isfinite(existing.confidence)) {
                        should_replace = true;
                    }
                }
            }

            if (should_replace) {
                existing = box;
            }
        }
    }

    return result;
}

std::vector<ZarrDetectionLoader::ChaserState>
ZarrDetectionLoader::getChaserStatesForFrame(size_t frame_id) const {
    std::vector<ChaserState> result;
    if (!data_.has_chaser_states ||
        frame_id >= data_.chaser_states_by_camera_frame.size()) {
        if (kChaserDebugLoggingEnabled) {
            std::cout << "  [ChaserDebug] Frame " << frame_id
                      << ": 0 chaser state(s)" << std::endl;
            if (!data_.has_chaser_states) {
                std::cout << "    (Chaser data not loaded)" << std::endl;
            }
        }
        return result;
    }

    std::unordered_map<int32_t, size_t> latest_by_index;
    for (size_t idx : data_.chaser_states_by_camera_frame[frame_id]) {
        if (idx >= data_.chaser_states.size()) {
            continue;
        }
        const auto& src = data_.chaser_states[idx];
        ChaserState candidate;
        candidate.stimulus_frame_num = src.stimulus_frame_num;
        candidate.camera_frame_id = src.camera_frame_id;
        candidate.chaser_index = src.chaser_index;
        candidate.chaser_pos_x = src.chaser_pos_x;
        candidate.chaser_pos_y = src.chaser_pos_y;
        candidate.target_pos_x = src.target_pos_x;
        candidate.target_pos_y = src.target_pos_y;
        candidate.chaser_radius_px = src.chaser_radius_px;
        candidate.distance_to_target_px = src.distance_to_target_px;
        candidate.target_speed_px_per_s = src.target_speed_px_per_s;
        candidate.is_chasing = src.is_chasing != 0;
        candidate.timestamp_ns_session = src.timestamp_ns_session;
        candidate.texture_space = src.texture_space;
        candidate.chaser_camera_x = src.chaser_camera_x;
        candidate.chaser_camera_y = src.chaser_camera_y;
        candidate.target_camera_x = src.target_camera_x;
        candidate.target_camera_y = src.target_camera_y;
        candidate.has_camera_coords = src.has_camera_coords;

        int32_t index_key = candidate.chaser_index;
        auto it = latest_by_index.find(index_key);
        bool keep = true;
        if (it != latest_by_index.end()) {
            auto& existing = result[it->second];
            if (candidate.stimulus_frame_num < existing.stimulus_frame_num) {
                keep = false;
            } else if (candidate.stimulus_frame_num == existing.stimulus_frame_num &&
                       candidate.timestamp_ns_session <= existing.timestamp_ns_session) {
                keep = false;
            }
            if (keep) {
                existing = candidate;
            }
        } else if (keep) {
            latest_by_index[index_key] = result.size();
            result.push_back(candidate);
        }
    }

    if (kChaserDebugLoggingEnabled) {
        std::cout << "  [ChaserDebug] Frame " << frame_id << ": "
                  << result.size() << " chaser state(s)" << std::endl;
        for (const auto& state : result) {
            std::cout << "    idx=" << state.chaser_index
                      << " camera_frame=" << state.camera_frame_id
                      << " stim_frame=" << state.stimulus_frame_num
                      << " chaser=(" << state.chaser_pos_x << "," << state.chaser_pos_y << ")"
                      << " target=(" << state.target_pos_x << "," << state.target_pos_y << ")"
                      << " radius_px=" << state.chaser_radius_px
                      << " distance_px=" << state.distance_to_target_px
                      << " target_speed=" << state.target_speed_px_per_s
                      << " is_chasing=" << (state.is_chasing ? "true" : "false")
                      << " timestamp_ns=" << state.timestamp_ns_session
                      << " space=" << (state.texture_space ? "texture" : "camera")
                      << std::endl;
            if (state.has_camera_coords) {
                std::cout << "      camera_chaser=(" << state.chaser_camera_x << "," << state.chaser_camera_y << ")"
                          << " camera_target=(" << state.target_camera_x << "," << state.target_camera_y << ")"
                          << std::endl;
            }
        }
    }

    return result;
}

size_t ZarrDetectionLoader::getMovementSeriesCount() const {
    return data_.movement_series.size();
}

const ZarrDetectionData::MovementSeries*
ZarrDetectionLoader::getMovementSeries(size_t index) const {
    if (index < data_.movement_series.size()) {
        return &data_.movement_series[index];
    }
    return nullptr;
}

size_t ZarrDetectionLoader::getSelectedMovementSeriesIndex() const {
    return data_.movement_selected_index;
}

const ZarrDetectionData::MovementSeries*
ZarrDetectionLoader::getSelectedMovementSeries() const {
    size_t index = data_.movement_selected_index;
    if (index < data_.movement_series.size()) {
        return &data_.movement_series[index];
    }
    return nullptr;
}

bool ZarrDetectionLoader::selectMovementSeries(size_t index) {
    if (index >= data_.movement_series.size()) {
        return false;
    }
    data_.movement_selected_index = index;
    data_.has_movement_data = true;
    return true;
}
