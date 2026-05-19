#include "zarr_loader_internal.h"
#include <chrono>
#include <cstdlib>
#include <iostream>

using json = nlohmann::json;

namespace {

bool startupTraceEnabled() {
    const char* value = std::getenv("CRIMSON_STARTUP_TRACE");
    return value != nullptr && std::string(value) == "1";
}

bool eagerCropImagesEnabled() {
    const char* value = std::getenv("CRIMSON_EAGER_CROP_IMAGES");
    return value != nullptr && std::string(value) == "1";
}

size_t jsonShapeDim(const json& node_meta, size_t index) {
    if (!node_meta.contains("shape") || !node_meta["shape"].is_array() ||
        node_meta["shape"].size() <= index ||
        !node_meta["shape"][index].is_number()) {
        return 0;
    }
    const auto value = node_meta["shape"][index].get<int64_t>();
    return value > 0 ? static_cast<size_t>(value) : 0;
}

double elapsedMilliseconds(std::chrono::steady_clock::time_point start,
                           std::chrono::steady_clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

std::string displaySpeedLevelLabel(const std::string& level) {
    if (level == "raw") {
        return "Raw Speed";
    }
    if (level == "filtered") {
        return "Filtered Speed";
    }
    if (level == "smoothed") {
        return "Smoothed Speed";
    }
    if (level == "averaged") {
        return "Averaged Speed";
    }
    if (!level.empty()) {
        std::string label = level;
        std::replace(label.begin(), label.end(), '_', ' ');
        if (!label.empty()) {
            label[0] = static_cast<char>(
                std::toupper(static_cast<unsigned char>(label[0])));
        }
        return label + " Speed";
    }
    return "Speed";
}

std::string flatSpeedPrefixForLevel(const std::string& level) {
    if (level == "raw" || level == "filtered" || level == "smoothed" ||
        level == "averaged") {
        return "speed_" + level;
    }
    if (level.rfind("speed_", 0) == 0) {
        return level;
    }
    return level;
}

std::string formatTrackId(int32_t track_id) {
    return "id_" + std::to_string(track_id);
}

std::string normalizeSpeedLevelToken(std::string value) {
    if (value.rfind("speed_", 0) == 0) {
        value = value.substr(6);
    }
    return value;
}

const json* provenanceParameters(const json& attrs) {
    if (attrs.contains("provenance") && attrs["provenance"].is_object()) {
        const auto& provenance = attrs["provenance"];
        if (provenance.contains("parameters") &&
            provenance["parameters"].is_object()) {
            return &provenance["parameters"];
        }
    }
    return nullptr;
}

std::string jsonStringValue(const json& object, const char* key) {
    if (object.contains(key) && object[key].is_string()) {
        return object[key].get<std::string>();
    }
    return std::string();
}

std::string jsonStringAttr(const json& attrs, const char* key) {
    std::string value = jsonStringValue(attrs, key);
    if (!value.empty()) {
        return value;
    }
    if (const json* params = provenanceParameters(attrs)) {
        return jsonStringValue(*params, key);
    }
    return std::string();
}

const json* sourceRefsObject(const json& attrs) {
    if (attrs.contains("source_refs") && attrs["source_refs"].is_object()) {
        return &attrs["source_refs"];
    }
    return nullptr;
}

std::string jsonStringAttrWithSourceRefs(const json& attrs, const char* key) {
    std::string value = jsonStringAttr(attrs, key);
    if (!value.empty()) {
        return value;
    }
    if (const json* refs = sourceRefsObject(attrs)) {
        return jsonStringValue(*refs, key);
    }
    return std::string();
}

bool jsonNumberValue(const json& object, const char* key, double& out) {
    if (object.contains(key) && object[key].is_number()) {
        out = object[key].get<double>();
        return true;
    }
    return false;
}

float jsonFloatAttr(const json& attrs, const char* key) {
    double value = 0.0;
    if (jsonNumberValue(attrs, key, value)) {
        return static_cast<float>(value);
    }
    if (const json* params = provenanceParameters(attrs);
        params && jsonNumberValue(*params, key, value)) {
        return static_cast<float>(value);
    }
    return std::numeric_limits<float>::quiet_NaN();
}

int32_t jsonTrackIdAttr(const json& attrs, const char* key) {
    auto readTrack = [&](const json& object) -> std::optional<int32_t> {
        if (!object.contains(key)) {
            return std::nullopt;
        }
        const auto& value = object[key];
        if (value.is_number_integer()) {
            return value.get<int32_t>();
        }
        if (value.is_string()) {
            std::string text = value.get<std::string>();
            if (text.rfind("id_", 0) == 0) {
                text = text.substr(3);
            }
            try {
                return static_cast<int32_t>(std::stoi(text));
            } catch (...) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    };

    if (auto value = readTrack(attrs)) {
        return *value;
    }
    if (const json* params = provenanceParameters(attrs)) {
        if (auto value = readTrack(*params)) {
            return *value;
        }
    }
    return -1;
}

std::optional<int32_t> jsonIntValue(const json& object, const char* key) {
    if (!object.contains(key)) {
        return std::nullopt;
    }
    const auto& value = object[key];
    if (value.is_number_integer()) {
        return value.get<int32_t>();
    }
    if (value.is_number()) {
        return static_cast<int32_t>(value.get<double>());
    }
    return std::nullopt;
}

std::optional<int32_t> jsonIntAttr(const json& attrs, const char* key) {
    if (auto value = jsonIntValue(attrs, key)) {
        return value;
    }
    if (const json* params = provenanceParameters(attrs)) {
        return jsonIntValue(*params, key);
    }
    return std::nullopt;
}

std::optional<int32_t> jsonIntAttrWithSourceRefs(const json& attrs,
                                                 const char* key) {
    if (auto value = jsonIntAttr(attrs, key)) {
        return value;
    }
    if (const json* refs = sourceRefsObject(attrs)) {
        return jsonIntValue(*refs, key);
    }
    return std::nullopt;
}

struct CompactSwimBoutCandidate {
    int32_t candidate_id = -1;
    bool is_default = false;
    std::string detection_method;
    float min_bout_duration_s = std::numeric_limits<float>::quiet_NaN();
    float min_gap_duration_s = std::numeric_limits<float>::quiet_NaN();
};

struct CompactSwimBoutSignal {
    int32_t signal_id = -1;
    int32_t candidate_id = -1;
    std::string speed_level;
    std::string role;
    std::string signal_name;
    std::string source_level;
    std::string path_distance_source_level;
    std::string transform_type;
    std::string units;
    float tau_s = std::numeric_limits<float>::quiet_NaN();
};

template <typename T>
T valueAtOrDefault(const std::vector<T>& values, size_t index, T fallback) {
    return index < values.size() ? values[index] : fallback;
}

std::string stringAtOrEmpty(const std::vector<std::string>& values,
                            size_t index) {
    return index < values.size() ? values[index] : std::string();
}

struct TrackKinematicsCompatibilityFilter {
    std::unordered_set<std::string> run_names;
    std::unordered_set<int32_t> track_ids;
};

TrackKinematicsCompatibilityFilter makeTrackKinematicsCompatibilityFilter(
    const std::vector<ZarrDetectionData::MovementSeries>& movement_series) {
    TrackKinematicsCompatibilityFilter filter;
    for (const auto& series : movement_series) {
        if (series.category.rfind("track_kinematics/", 0) != 0) {
            continue;
        }
        if (!series.run_name.empty()) {
            filter.run_names.insert(series.run_name);
        }
        const int32_t track_id =
            jsonTrackIdAttr(json{{"track_id", series.track_id}}, "track_id");
        if (track_id >= 0) {
            filter.track_ids.insert(track_id);
        }
    }
    return filter;
}

bool isCompatibleTrackKinematicsSource(
    const json& attrs,
    const TrackKinematicsCompatibilityFilter& filter,
    const char* source_run_key,
    const char* track_id_key) {
    if (filter.run_names.empty()) {
        return true;
    }

    const std::string source_run = jsonStringAttr(attrs, source_run_key);
    if (!source_run.empty() && filter.run_names.count(source_run) == 0) {
        return false;
    }

    const int32_t track_id = jsonTrackIdAttr(attrs, track_id_key);
    if (track_id >= 0 && !filter.track_ids.empty() &&
        filter.track_ids.count(track_id) == 0) {
        return false;
    }
    return true;
}

using SwimBoutSourceKey = std::pair<std::string, std::string>;

std::set<SwimBoutSourceKey> preferredSwimBoutKinematicsSources(
    const std::vector<ZarrDetectionData::SwimBoutSeries>& swim_bout_series) {
    std::set<SwimBoutSourceKey> preferred;
    auto add_matching = [&](auto predicate) {
        for (const auto& series : swim_bout_series) {
            if (series.run_name.empty() || series.speed_level.empty() ||
                !predicate(series)) {
                continue;
            }
            preferred.emplace(series.run_name,
                              normalizeSpeedLevelToken(series.speed_level));
        }
    };

    add_matching([](const auto& series) {
        return series.is_latest_run && series.is_default_level;
    });
    if (!preferred.empty()) {
        return preferred;
    }
    add_matching([](const auto& series) { return series.is_latest_run; });
    if (!preferred.empty()) {
        return preferred;
    }
    add_matching([](const auto& series) { return series.is_default_level; });
    if (!preferred.empty()) {
        return preferred;
    }
    if (!swim_bout_series.empty() && !swim_bout_series.front().run_name.empty()) {
        preferred.emplace(
            swim_bout_series.front().run_name,
            normalizeSpeedLevelToken(swim_bout_series.front().speed_level));
    }
    return preferred;
}

bool matchesPreferredSwimBoutSource(
    const json& attrs,
    const std::set<SwimBoutSourceKey>& preferred_sources) {
    if (preferred_sources.empty()) {
        return true;
    }
    const std::string source_swim_bout_run =
        jsonStringAttrWithSourceRefs(attrs, "source_swim_bout_run");
    const std::string source_swim_bout_speed_level =
        normalizeSpeedLevelToken(
            jsonStringAttrWithSourceRefs(attrs,
                                         "source_swim_bout_speed_level"));
    if (source_swim_bout_run.empty() || source_swim_bout_speed_level.empty()) {
        return false;
    }
    return preferred_sources.count(
               {source_swim_bout_run, source_swim_bout_speed_level}) > 0;
}

}  // namespace

bool ZarrDetectionLoader::loadMovementData(const ts::kvstore::KvStore& store) {
    data_.has_movement_data = false;
    data_.movement_series.clear();
    data_.movement_selected_index = std::numeric_limits<size_t>::max();
    data_.movement_crop_run_name.clear();
    data_.swim_bout_series.clear();
    data_.bout_kinematics_series.clear();
    data_.crop_data = {};

    const bool trace_startup = startupTraceEnabled();
    auto trace_mark = std::chrono::steady_clock::now();
    auto traceStep = [&](const char* label) {
        if (!trace_startup) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        std::cout << "  [StartupTrace] " << label << " "
                  << elapsedMilliseconds(trace_mark, now) << " ms"
                  << std::endl;
        trace_mark = now;
    };

    const bool loaded_track_kinematics = loadTrackKinematicsData(store);
    traceStep("loadTrackKinematicsData");
    const bool loaded_legacy = loadLegacyMovementData(store);
    traceStep("loadLegacyMovementData");
    loadSwimBoutData(store);
    traceStep("loadSwimBoutData");
    loadBoutKinematicsData(store);
    traceStep("loadBoutKinematicsData");
    bool loaded = loaded_track_kinematics || loaded_legacy;

    if (!loaded) {
        return false;
    }

    finalizeMovementSelection();
    traceStep("finalizeMovementSelection");
    if (data_.has_movement_data) {
        std::string crop_candidate = data_.movement_crop_run_name;
        if (crop_candidate.empty() && !data_.keypoints_source_crop_run.empty()) {
            crop_candidate = NormalizeCropRunName(data_.keypoints_source_crop_run);
        }
        if (!crop_candidate.empty()) {
            loadMovementCropRunMetadata(store, crop_candidate);
            if (eagerCropImagesEnabled()) {
                loadMovementCropRun(store, crop_candidate);
            }
        }
        if (!crop_candidate.empty() && trace_startup &&
            !eagerCropImagesEnabled()) {
            std::cout << "  [StartupTrace] deferred crop image preload for '"
                      << crop_candidate << "'" << std::endl;
        }
    }
    traceStep("loadMovementCropRun");
    return data_.has_movement_data;
}

bool ZarrDetectionLoader::loadTrackKinematicsData(
    const ts::kvstore::KvStore& store) {
    const std::vector<std::pair<std::string, std::string>> scopes = {
        {"analysis/track_kinematics_runs/offline", "offline"},
        {"analysis/track_kinematics_runs/online_refined", "online_refined"},
        {"analysis/track_kinematics_runs/online", "online"},
    };

    bool loaded_any = false;

    for (const auto& [scope_path, scope_name] : scopes) {
        std::vector<std::string> run_candidates;
        std::string latest;
        if (auto scope_attrs = readGroupAttrs(store, scope_path)) {
            latest = extractLatestRunName(*scope_attrs);
            if (!latest.empty()) {
                run_candidates.push_back(latest);
            }
        }
        if (latest.empty() && !root_path_.empty()) {
            auto runs = collect_runs_fs(root_path_, scope_path, {});
            run_candidates.insert(run_candidates.end(), runs.begin(), runs.end());
        }
        std::sort(run_candidates.begin(), run_candidates.end());
        run_candidates.erase(std::unique(run_candidates.begin(),
                                         run_candidates.end()),
                             run_candidates.end());
        if (run_candidates.empty()) {
            continue;
        }

        const std::string run_name =
            !latest.empty() ? latest : run_candidates.back();
        const std::string run_base = scope_path + "/" + run_name + "/";

        float pixels_per_mm = 0.0f;
        double run_fps = 0.0;
        double smoothing_seconds = 0.0;
        std::string detection_variant;
        std::string source_detect_run;
        std::string crop_candidate;

        if (auto run_attrs_opt = readGroupAttrs(store, run_base)) {
            const json& run_attrs = *run_attrs_opt;
            auto readNumber = [&](const char* key, double& out) {
                if (run_attrs.contains(key) && run_attrs[key].is_number()) {
                    out = run_attrs[key].get<double>();
                }
            };
            readNumber("fps", run_fps);
            readNumber("smoothing_seconds", smoothing_seconds);

            double pixel_to_mm = 0.0;
            readNumber("pixel_to_mm", pixel_to_mm);
            if (pixel_to_mm > 1e-12) {
                pixels_per_mm = static_cast<float>(1.0 / pixel_to_mm);
            }
            if (run_attrs.contains("pixels_per_mm") &&
                run_attrs["pixels_per_mm"].is_number()) {
                pixels_per_mm =
                    static_cast<float>(run_attrs["pixels_per_mm"].get<double>());
            }

            crop_candidate = ExtractCropRunFromObject(run_attrs);
            if (run_attrs.contains("inputs") && run_attrs["inputs"].is_object()) {
                const auto& inputs = run_attrs["inputs"];
                std::string from_inputs = ExtractCropRunFromObject(inputs);
                if (!from_inputs.empty()) {
                    crop_candidate = from_inputs;
                }
                if (inputs.contains("detection_variant") &&
                    inputs["detection_variant"].is_string()) {
                    detection_variant =
                        inputs["detection_variant"].get<std::string>();
                }
                if (inputs.contains("source_detect_run") &&
                    inputs["source_detect_run"].is_string()) {
                    source_detect_run =
                        inputs["source_detect_run"].get<std::string>();
                }
            }
            if (!crop_candidate.empty() && data_.movement_crop_run_name.empty()) {
                data_.movement_crop_run_name = crop_candidate;
            }
        }

        std::vector<std::string> track_ids;
        std::vector<int32_t> track_ids_array;
        if (readInt32Array(store, run_base + "track_ids", track_ids_array) &&
            !track_ids_array.empty()) {
            track_ids.reserve(track_ids_array.size());
            for (int32_t track_id : track_ids_array) {
                track_ids.push_back(formatTrackId(track_id));
            }
        }
        if (track_ids.empty() && !root_path_.empty()) {
            namespace fs = std::filesystem;
            fs::path track_root = fs::path(root_path_) / run_base / "tracks";
            if (fs::exists(track_root) && fs::is_directory(track_root)) {
                for (const auto& entry : fs::directory_iterator(track_root)) {
                    if (entry.is_directory()) {
                        track_ids.push_back(entry.path().filename().string());
                    }
                }
            }
        }
        if (track_ids.empty()) {
            track_ids.push_back("id_0");
        }
        std::sort(track_ids.begin(), track_ids.end());
        track_ids.erase(std::unique(track_ids.begin(), track_ids.end()),
                        track_ids.end());

        const std::vector<std::string> speed_levels = {
            "filtered", "smoothed", "raw", "averaged"};
        const std::vector<std::string> frame_names = {"frame_indices"};
        const std::vector<std::string> time_float_names = {"time_seconds"};
        const std::vector<std::string> timestamp_ns_names = {
            "timestamp_ns_session"};

        for (const auto& track_id : track_ids) {
            const std::string track_base = run_base + "tracks/" + track_id + "/";
            for (const auto& level : speed_levels) {
                const std::string flat_prefix = flatSpeedPrefixForLevel(level);
                const std::vector<std::string> primary_mm_names = {
                    "movement/speed/" + level + "/mm",
                    flat_prefix + "_mm",
                };
                const std::vector<std::string> primary_px_names = {
                    "movement/speed/" + level + "/px",
                    flat_prefix + "_px",
                };
                const std::vector<std::string> secondary_mm_names =
                    level == "raw"
                        ? std::vector<std::string>{}
                        : std::vector<std::string>{
                              "movement/speed/raw/mm",
                              "speed_raw_mm",
                          };
                const std::vector<std::string> secondary_px_names =
                    level == "raw"
                        ? std::vector<std::string>{}
                        : std::vector<std::string>{
                              "movement/speed/raw/px",
                              "speed_raw_px",
                          };

                bool loaded_track = loadMovementTrack(
                    store,
                    "[TrackKinematics]",
                    run_name,
                    track_id,
                    track_base,
                    frame_names,
                    time_float_names,
                    timestamp_ns_names,
                    primary_mm_names,
                    primary_px_names,
                    secondary_mm_names,
                    secondary_px_names,
                    pixels_per_mm,
                    run_fps,
                    "track_kinematics/" + scope_name,
                    detection_variant.empty() ? level : detection_variant,
                    source_detect_run,
                    smoothing_seconds,
                    0,
                    0,
                    false,
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr,
                    level,
                    displaySpeedLevelLabel(level),
                    level == "raw" ? std::string() : "Raw Speed");
                loaded_any = loaded_any || loaded_track;
            }
        }
    }

    return loaded_any;
}

bool ZarrDetectionLoader::loadSwimBoutData(
    const ts::kvstore::KvStore& store) {
    const std::string parent_path = "analysis/swim_bout_runs";
    const auto compatibility_filter =
        makeTrackKinematicsCompatibilityFilter(data_.movement_series);
    std::vector<std::string> run_candidates;
    std::string latest;
    if (auto parent_attrs = readGroupAttrs(store, parent_path)) {
        latest = extractLatestRunName(*parent_attrs);
        if (!latest.empty()) {
            run_candidates.push_back(latest);
        }
    }
    if (!root_path_.empty()) {
        auto runs = collect_runs_fs(root_path_, parent_path, {});
        run_candidates.insert(run_candidates.end(), runs.begin(), runs.end());
    }
    std::sort(run_candidates.begin(), run_candidates.end());
    run_candidates.erase(std::unique(run_candidates.begin(),
                                     run_candidates.end()),
                         run_candidates.end());
    if (run_candidates.empty()) {
        return false;
    }

    auto readFrameColumn = [&](const std::string& path,
                               std::vector<int32_t>& out) -> bool {
        std::vector<int64_t> tmp64;
        if (readInt64Array(store, path, tmp64) && !tmp64.empty()) {
            out.resize(tmp64.size());
            for (size_t i = 0; i < tmp64.size(); ++i) {
                out[i] = clampToInt32(tmp64[i]);
            }
            return true;
        }
        return readInt32Array(store, path, out) && !out.empty();
    };

    auto readFloat2DRow = [&](const std::string& path,
                              size_t row_index,
                              std::vector<float>& out) -> bool {
        auto attempt = [&](auto type_token) -> bool {
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
            if (array.rank() != 2 || row_index >= static_cast<size_t>(array.shape()[0])) {
                return false;
            }
            const size_t cols = static_cast<size_t>(array.shape()[1]);
            const auto* data = static_cast<const Source*>(array.data());
            out.resize(cols);
            for (size_t col = 0; col < cols; ++col) {
                out[col] = static_cast<float>(data[row_index * cols + col]);
            }
            return true;
        };
        return attempt(float{}) || attempt(double{});
    };

    auto trimSeriesToBoutCount =
        [](ZarrDetectionData::SwimBoutSeries& series) -> size_t {
        const size_t bout_count =
            std::min(series.start_frame.size(), series.end_frame.size());
        auto trim = [&](auto& values) {
            if (!values.empty() && values.size() > bout_count) {
                values.resize(bout_count);
            }
        };
        trim(series.start_frame);
        trim(series.end_frame);
        trim(series.core_start_frame);
        trim(series.core_end_frame);
        trim(series.start_time_s);
        trim(series.end_time_s);
        trim(series.duration_s);
        trim(series.path_length_mm);
        trim(series.path_length_px);
        trim(series.net_displacement_mm);
        trim(series.net_displacement_px);
        trim(series.peak_detection_signal_mm_s);
        trim(series.peak_speed_mm_s);
        trim(series.gap_censored);
        return bout_count;
    };

    auto appendFilteredFrameColumn =
        [&](const std::string& path,
            const std::vector<size_t>& rows,
            std::vector<int32_t>& out) -> bool {
        std::vector<int32_t> values;
        if (!readFrameColumn(path, values)) {
            return false;
        }
        out.reserve(rows.size());
        for (size_t row : rows) {
            if (row < values.size()) {
                out.push_back(values[row]);
            }
        }
        return !out.empty();
    };

    auto appendFilteredFloatColumn =
        [&](const std::string& path,
            const std::vector<size_t>& rows,
            std::vector<float>& out) -> bool {
        std::vector<float> values;
        if (!readFloatArray(store, path, values)) {
            return false;
        }
        out.reserve(rows.size());
        for (size_t row : rows) {
            if (row < values.size()) {
                out.push_back(values[row]);
            }
        }
        return !out.empty();
    };

    auto appendFilteredBoolColumn =
        [&](const std::string& path,
            const std::vector<size_t>& rows,
            std::vector<uint8_t>& out) -> bool {
        std::vector<uint8_t> values;
        if (!readBoolArray(store, path, values)) {
            return false;
        }
        out.reserve(rows.size());
        for (size_t row : rows) {
            if (row < values.size()) {
                out.push_back(values[row]);
            }
        }
        return !out.empty();
    };

    auto loadCompactRun = [&](const std::string& run_name,
                              const std::string& run_base,
                              const json& run_attrs,
                              bool is_latest_run) -> bool {
        const std::string candidates_base = run_base + "indexes/candidates/";
        const std::string signals_base = run_base + "indexes/signal_variants/";
        const std::string bouts_base = run_base + "tables/bouts/";

        std::vector<int32_t> candidate_ids;
        if (!readInt32Array(store, candidates_base + "candidate_id",
                            candidate_ids) ||
            candidate_ids.empty()) {
            return false;
        }
        std::vector<uint8_t> candidate_is_default;
        std::vector<std::string> candidate_detection_methods;
        std::vector<float> candidate_min_bout_duration_s;
        std::vector<float> candidate_min_gap_duration_s;
        readBoolArray(store, candidates_base + "is_default",
                      candidate_is_default);
        readStringArray(store, candidates_base + "detection_method",
                        candidate_detection_methods);
        readFloatArray(store, candidates_base + "min_bout_duration_s",
                       candidate_min_bout_duration_s);
        readFloatArray(store, candidates_base + "min_gap_duration_s",
                       candidate_min_gap_duration_s);

        std::vector<CompactSwimBoutCandidate> candidates;
        candidates.reserve(candidate_ids.size());
        for (size_t i = 0; i < candidate_ids.size(); ++i) {
            CompactSwimBoutCandidate candidate;
            candidate.candidate_id = candidate_ids[i];
            candidate.is_default =
                i < candidate_is_default.size() && candidate_is_default[i] != 0;
            candidate.detection_method =
                stringAtOrEmpty(candidate_detection_methods, i);
            candidate.min_bout_duration_s =
                valueAtOrDefault(candidate_min_bout_duration_s,
                                 i,
                                 std::numeric_limits<float>::quiet_NaN());
            candidate.min_gap_duration_s =
                valueAtOrDefault(candidate_min_gap_duration_s,
                                 i,
                                 std::numeric_limits<float>::quiet_NaN());
            candidates.push_back(std::move(candidate));
        }

        int32_t selected_candidate_id =
            jsonIntAttr(run_attrs, "default_candidate_id").value_or(-1);
        auto candidate_it = std::find_if(
            candidates.begin(),
            candidates.end(),
            [&](const CompactSwimBoutCandidate& candidate) {
                return candidate.candidate_id == selected_candidate_id;
            });
        if (candidate_it == candidates.end()) {
            candidate_it = std::find_if(
                candidates.begin(),
                candidates.end(),
                [](const CompactSwimBoutCandidate& candidate) {
                    return candidate.is_default;
                });
        }
        if (candidate_it == candidates.end()) {
            candidate_it = candidates.begin();
        }
        if (candidate_it == candidates.end()) {
            return false;
        }
        selected_candidate_id = candidate_it->candidate_id;

        std::vector<int32_t> signal_ids;
        if (!readInt32Array(store, signals_base + "signal_id", signal_ids) ||
            signal_ids.empty()) {
            return false;
        }
        std::vector<int32_t> signal_candidate_ids;
        std::vector<std::string> signal_speed_levels;
        std::vector<std::string> signal_roles;
        std::vector<std::string> signal_names;
        std::vector<std::string> signal_source_levels;
        std::vector<std::string> signal_path_distance_source_levels;
        std::vector<std::string> signal_transform_types;
        std::vector<std::string> signal_units;
        std::vector<float> signal_tau_s;
        readInt32Array(store, signals_base + "candidate_id",
                       signal_candidate_ids);
        readStringArray(store, signals_base + "speed_level",
                        signal_speed_levels);
        readStringArray(store, signals_base + "role", signal_roles);
        readStringArray(store, signals_base + "signal_name", signal_names);
        readStringArray(store, signals_base + "source_level",
                        signal_source_levels);
        readStringArray(store, signals_base + "path_distance_source_level",
                        signal_path_distance_source_levels);
        readStringArray(store, signals_base + "transform_type",
                        signal_transform_types);
        readStringArray(store, signals_base + "units", signal_units);
        readFloatArray(store, signals_base + "tau_s", signal_tau_s);

        std::vector<CompactSwimBoutSignal> signals;
        signals.reserve(signal_ids.size());
        for (size_t i = 0; i < signal_ids.size(); ++i) {
            const int32_t row_candidate_id =
                valueAtOrDefault(signal_candidate_ids, i, selected_candidate_id);
            if (row_candidate_id != selected_candidate_id) {
                continue;
            }
            CompactSwimBoutSignal signal;
            signal.signal_id = signal_ids[i];
            signal.candidate_id = row_candidate_id;
            signal.speed_level = stringAtOrEmpty(signal_speed_levels, i);
            signal.role = stringAtOrEmpty(signal_roles, i);
            signal.signal_name = stringAtOrEmpty(signal_names, i);
            signal.source_level = stringAtOrEmpty(signal_source_levels, i);
            signal.path_distance_source_level =
                stringAtOrEmpty(signal_path_distance_source_levels, i);
            signal.transform_type = stringAtOrEmpty(signal_transform_types, i);
            signal.units = stringAtOrEmpty(signal_units, i);
            signal.tau_s =
                valueAtOrDefault(signal_tau_s,
                                 i,
                                 std::numeric_limits<float>::quiet_NaN());
            if (signal.speed_level.empty()) {
                signal.speed_level = signal.signal_name;
            }
            signals.push_back(std::move(signal));
        }
        if (signals.empty()) {
            return false;
        }

        std::vector<int32_t> bout_candidate_ids;
        std::vector<int32_t> bout_signal_ids;
        if (!readInt32Array(store, bouts_base + "candidate_id",
                            bout_candidate_ids) ||
            !readInt32Array(store, bouts_base + "signal_id",
                            bout_signal_ids)) {
            return false;
        }

        const std::string default_level =
            jsonStringAttr(run_attrs, "default_level");
        const std::optional<int32_t> default_signal_id =
            jsonIntAttr(run_attrs, "default_signal_id");
        bool loaded_any_signal = false;
        for (const auto& signal : signals) {
            std::vector<size_t> matching_rows;
            const size_t table_rows =
                std::min(bout_candidate_ids.size(), bout_signal_ids.size());
            matching_rows.reserve(table_rows);
            for (size_t row = 0; row < table_rows; ++row) {
                if (bout_candidate_ids[row] == selected_candidate_id &&
                    bout_signal_ids[row] == signal.signal_id) {
                    matching_rows.push_back(row);
                }
            }
            if (matching_rows.empty()) {
                continue;
            }

            ZarrDetectionData::SwimBoutSeries series;
            series.run_name = run_name;
            series.speed_level = signal.speed_level;
            series.is_compact_layout = true;
            series.candidate_id = selected_candidate_id;
            series.signal_id = signal.signal_id;
            series.signal_role = signal.role;
            series.signal_name = signal.signal_name;
            series.default_level = default_level;
            series.is_latest_run = is_latest_run;
            series.is_default_level =
                (default_signal_id && *default_signal_id == signal.signal_id) ||
                (!default_level.empty() &&
                 normalizeSpeedLevelToken(default_level) ==
                     normalizeSpeedLevelToken(signal.speed_level));
            series.source_track_kinematics_run =
                jsonStringAttr(run_attrs, "source_track_kinematics_run");
            series.track_id = jsonTrackIdAttr(run_attrs, "track_id");
            series.detection_method = !candidate_it->detection_method.empty()
                                          ? candidate_it->detection_method
                                          : jsonStringAttr(run_attrs,
                                                           "detection_method");
            series.detection_signal_label =
                !signal.transform_type.empty()
                    ? signal.transform_type
                    : (!signal.role.empty() ? signal.role : signal.speed_level);
            if (series.detection_signal_label.empty() &&
                normalizeSpeedLevelToken(signal.speed_level) == "exponential") {
                series.detection_signal_label = "exponential detector";
            }
            series.detection_signal_source_level = signal.source_level;
            series.detection_signal_source_path =
                run_base + "signals/detector_signal_mm_s";
            series.movement_metric_source_level = signal.source_level;
            series.path_distance_source_level = signal.path_distance_source_level;
            series.threshold_mm = jsonFloatAttr(run_attrs, "threshold_mm");
            series.exponential_tau_s = std::isfinite(static_cast<double>(
                                           signal.tau_s))
                                           ? signal.tau_s
                                           : jsonFloatAttr(run_attrs,
                                                           "exponential_tau_s");
            series.min_bout_duration_s =
                std::isfinite(static_cast<double>(
                    candidate_it->min_bout_duration_s))
                    ? candidate_it->min_bout_duration_s
                    : jsonFloatAttr(run_attrs, "min_bout_duration_s");
            series.min_gap_duration_s =
                std::isfinite(static_cast<double>(
                    candidate_it->min_gap_duration_s))
                    ? candidate_it->min_gap_duration_s
                    : jsonFloatAttr(run_attrs, "min_gap_duration_s");
            series.min_peak_prominence_mm_s =
                jsonFloatAttr(run_attrs, "min_peak_prominence_mm_s");
            series.peak_width_rel_height =
                jsonFloatAttr(run_attrs, "peak_width_rel_height");

            appendFilteredFrameColumn(bouts_base + "start_frame",
                                      matching_rows,
                                      series.start_frame);
            appendFilteredFrameColumn(bouts_base + "end_frame",
                                      matching_rows,
                                      series.end_frame);
            appendFilteredFrameColumn(bouts_base + "core_start_frame",
                                      matching_rows,
                                      series.core_start_frame);
            appendFilteredFrameColumn(bouts_base + "core_end_frame",
                                      matching_rows,
                                      series.core_end_frame);
            appendFilteredFloatColumn(bouts_base + "start_time_s",
                                      matching_rows,
                                      series.start_time_s);
            appendFilteredFloatColumn(bouts_base + "end_time_s",
                                      matching_rows,
                                      series.end_time_s);
            appendFilteredFloatColumn(bouts_base + "duration_s",
                                      matching_rows,
                                      series.duration_s);
            appendFilteredFloatColumn(bouts_base + "path_length_mm",
                                      matching_rows,
                                      series.path_length_mm);
            appendFilteredFloatColumn(bouts_base + "path_length_px",
                                      matching_rows,
                                      series.path_length_px);
            appendFilteredFloatColumn(bouts_base + "net_displacement_mm",
                                      matching_rows,
                                      series.net_displacement_mm);
            appendFilteredFloatColumn(bouts_base + "net_displacement_px",
                                      matching_rows,
                                      series.net_displacement_px);
            appendFilteredFloatColumn(bouts_base +
                                          "peak_detection_signal_mm_s",
                                      matching_rows,
                                      series.peak_detection_signal_mm_s);
            if (!appendFilteredFloatColumn(bouts_base +
                                               "peak_physical_speed_mm_s",
                                           matching_rows,
                                           series.peak_speed_mm_s)) {
                appendFilteredFloatColumn(bouts_base + "peak_speed_mm_s",
                                          matching_rows,
                                          series.peak_speed_mm_s);
            }
            appendFilteredBoolColumn(bouts_base + "gap_censored",
                                     matching_rows,
                                     series.gap_censored);

            std::vector<int32_t> detector_signal_ids;
            if (readInt32Array(store,
                               run_base +
                                   "signals/detector_signal_signal_ids",
                               detector_signal_ids)) {
                const auto detector_row_it =
                    std::find(detector_signal_ids.begin(),
                              detector_signal_ids.end(),
                              signal.signal_id);
                if (detector_row_it != detector_signal_ids.end()) {
                    const size_t detector_row =
                        static_cast<size_t>(std::distance(
                            detector_signal_ids.begin(), detector_row_it));
                    if (readFloat2DRow(run_base +
                                           "signals/detector_signal_mm_s",
                                       detector_row,
                                       series.detector_trace_values)) {
                        readFrameColumn(run_base + "signals/frame_indices",
                                        series.detector_trace_frame_indices);
                        series.detector_trace_label = "Detector response";
                        series.detector_trace_units =
                            signal.units.empty() ? "mm/s" : signal.units;
                        series.has_detector_trace =
                            !series.detector_trace_values.empty();
                    }
                }
            }

            if (trimSeriesToBoutCount(series) == 0) {
                continue;
            }
            data_.swim_bout_series.push_back(std::move(series));
            loaded_any_signal = true;
        }

        if (loaded_any_signal) {
            std::cout << "  [SwimBouts] Loaded compact-v2 run '" << run_name
                      << "' candidate " << selected_candidate_id << std::endl;
        }
        return loaded_any_signal;
    };

    bool loaded_any = false;
    for (const auto& run_name : run_candidates) {
        const std::string run_base = parent_path + "/" + run_name + "/";

        json run_attrs = json::object();
        if (auto run_attrs_opt = readGroupAttrs(store, run_base)) {
            run_attrs = *run_attrs_opt;
        }
        if (!isCompatibleTrackKinematicsSource(
                run_attrs, compatibility_filter, "source_track_kinematics_run",
                "track_id")) {
            continue;
        }

        const std::string layout = jsonStringAttr(run_attrs, "layout");
        const bool compact_v2_run =
            layout == "compact_tabular_v2" ||
            arrayExists(store, run_base + "indexes/candidates");
        if (compact_v2_run) {
            loaded_any =
                loadCompactRun(run_name,
                               run_base,
                               run_attrs,
                               !latest.empty() && run_name == latest) ||
                loaded_any;
            continue;
        }

        const std::string default_level = jsonStringAttr(run_attrs,
                                                         "default_level");
        std::vector<std::string> speed_levels;
        if (!default_level.empty()) {
            speed_levels.push_back(default_level);
        }
        if (!root_path_.empty()) {
            auto fs_levels =
                collect_runs_fs(root_path_, parent_path + "/" + run_name,
                                {"bouts"});
            speed_levels.insert(speed_levels.end(), fs_levels.begin(),
                                fs_levels.end());
        }
        std::sort(speed_levels.begin(), speed_levels.end());
        speed_levels.erase(std::unique(speed_levels.begin(), speed_levels.end()),
                           speed_levels.end());

        for (const auto& speed_level : speed_levels) {
            const std::string level_base = run_base + speed_level + "/";
            const std::string bouts_base = level_base + "bouts/";
            if (!arrayExists(store, bouts_base + "start_frame") &&
                !arrayExists(store, bouts_base + "end_frame")) {
                continue;
            }

            json level_attrs = json::object();
            if (auto level_attrs_opt = readGroupAttrs(store, level_base)) {
                level_attrs = *level_attrs_opt;
            }

            ZarrDetectionData::SwimBoutSeries series;
            series.run_name = run_name;
            series.speed_level = speed_level;
            series.default_level = default_level;
            series.is_latest_run = !latest.empty() && run_name == latest;
            series.is_default_level =
                !default_level.empty() &&
                normalizeSpeedLevelToken(default_level) ==
                    normalizeSpeedLevelToken(speed_level);
            if (level_attrs.contains("is_default_level") &&
                level_attrs["is_default_level"].is_boolean()) {
                series.is_default_level =
                    level_attrs["is_default_level"].get<bool>();
            }

            series.source_track_kinematics_run =
                jsonStringAttr(run_attrs, "source_track_kinematics_run");
            series.track_id = jsonTrackIdAttr(run_attrs, "track_id");
            series.detection_method =
                jsonStringAttr(run_attrs, "detection_method");
            series.detection_signal_source_level =
                jsonStringAttr(run_attrs, "detection_signal_source_level");
            if (series.detection_signal_source_level.empty()) {
                series.detection_signal_source_level =
                    jsonStringAttr(run_attrs, "exponential_source_level");
            }
            if (series.detection_signal_source_level.empty()) {
                series.detection_signal_source_level =
                    jsonStringAttr(level_attrs, "source_speed_level");
            }
            series.detection_signal_source_path =
                jsonStringAttr(run_attrs, "detection_signal_source_path");
            series.movement_metric_source_level =
                jsonStringAttr(run_attrs, "movement_metric_source_level");
            series.path_distance_source_level =
                jsonStringAttr(run_attrs, "path_distance_source_level");
            if (series.path_distance_source_level.empty()) {
                series.path_distance_source_level =
                    jsonStringAttr(level_attrs, "path_distance_source_level");
            }
            series.threshold_mm = jsonFloatAttr(run_attrs, "threshold_mm");
            series.exponential_tau_s =
                jsonFloatAttr(run_attrs, "exponential_tau_s");
            if (!std::isfinite(static_cast<double>(series.exponential_tau_s))) {
                series.exponential_tau_s = jsonFloatAttr(level_attrs, "tau_s");
            }
            series.min_bout_duration_s =
                jsonFloatAttr(run_attrs, "min_bout_duration_s");
            series.min_gap_duration_s =
                jsonFloatAttr(run_attrs, "min_gap_duration_s");
            series.min_peak_prominence_mm_s =
                jsonFloatAttr(run_attrs, "min_peak_prominence_mm_s");
            series.peak_width_rel_height =
                jsonFloatAttr(run_attrs, "peak_width_rel_height");

            if (level_attrs.contains("detection_signal_transform_family") &&
                level_attrs["detection_signal_transform_family"].is_string()) {
                series.detection_signal_label =
                    level_attrs["detection_signal_transform_family"]
                        .get<std::string>();
            } else if (level_attrs.contains("detection_signal_array") &&
                       level_attrs["detection_signal_array"].is_string()) {
                series.detection_signal_label =
                    level_attrs["detection_signal_array"].get<std::string>();
            } else if (normalizeSpeedLevelToken(speed_level) == "exponential") {
                series.detection_signal_label = "exponential detector";
            }

            readFrameColumn(bouts_base + "start_frame", series.start_frame);
            readFrameColumn(bouts_base + "end_frame", series.end_frame);
            readFrameColumn(bouts_base + "core_start_frame",
                            series.core_start_frame);
            readFrameColumn(bouts_base + "core_end_frame",
                            series.core_end_frame);
            readFloatArray(store, bouts_base + "start_time_s",
                           series.start_time_s);
            readFloatArray(store, bouts_base + "end_time_s", series.end_time_s);
            readFloatArray(store, bouts_base + "duration_s", series.duration_s);
            readFloatArray(store, bouts_base + "path_length_mm",
                           series.path_length_mm);
            readFloatArray(store, bouts_base + "path_length_px",
                           series.path_length_px);
            readFloatArray(store, bouts_base + "net_displacement_mm",
                           series.net_displacement_mm);
            readFloatArray(store, bouts_base + "net_displacement_px",
                           series.net_displacement_px);
            if (!readFloatArray(store,
                                bouts_base + "peak_detection_signal_mm_s",
                                series.peak_detection_signal_mm_s)) {
                readFloatArray(store, bouts_base + "peak_speed_mm_s",
                               series.peak_detection_signal_mm_s);
            }
            readFloatArray(store, bouts_base + "peak_speed_mm_s",
                           series.peak_speed_mm_s);
            readBoolArray(store, bouts_base + "gap_censored",
                          series.gap_censored);

            if (readFloatArray(store,
                               level_base + "detection_signal_mm_s",
                               series.detector_trace_values)) {
                series.detector_trace_label = "Detector response";
                series.detector_trace_units = "mm/s";
            } else if (readFloatArray(store,
                                      level_base + "speed_exponential_mm",
                                      series.detector_trace_values)) {
                series.detector_trace_label = "Detector response";
                series.detector_trace_units = "mm/s";
            }
            if (!series.detector_trace_values.empty()) {
                readFrameColumn(level_base + "frame_indices",
                                series.detector_trace_frame_indices);
                series.has_detector_trace = true;
            }

            const size_t bout_count =
                std::min(series.start_frame.size(), series.end_frame.size());
            if (bout_count == 0) {
                continue;
            }
            auto trim = [&](auto& values) {
                if (!values.empty() && values.size() > bout_count) {
                    values.resize(bout_count);
                }
            };
            trim(series.start_frame);
            trim(series.end_frame);
            trim(series.core_start_frame);
            trim(series.core_end_frame);
            trim(series.start_time_s);
            trim(series.end_time_s);
            trim(series.duration_s);
            trim(series.path_length_mm);
            trim(series.path_length_px);
            trim(series.net_displacement_mm);
            trim(series.net_displacement_px);
            trim(series.peak_detection_signal_mm_s);
            trim(series.peak_speed_mm_s);
            trim(series.gap_censored);

            data_.swim_bout_series.push_back(std::move(series));
            loaded_any = true;
        }
    }

    if (loaded_any) {
        std::cout << "  [SwimBouts] Loaded "
                  << data_.swim_bout_series.size() << " candidates"
                  << std::endl;
    }
    return loaded_any;
}

bool ZarrDetectionLoader::loadBoutKinematicsData(
    const ts::kvstore::KvStore& store) {
    const std::string parent_path = "analysis/bout_kinematics_runs";
    const auto compatibility_filter =
        makeTrackKinematicsCompatibilityFilter(data_.movement_series);
    const auto preferred_swim_bout_sources =
        preferredSwimBoutKinematicsSources(data_.swim_bout_series);
    std::vector<std::string> run_candidates;
    if (auto parent_attrs = readGroupAttrs(store, parent_path)) {
        const std::string latest = extractLatestRunName(*parent_attrs);
        if (!latest.empty()) {
            run_candidates.push_back(latest);
        }
    }
    if (!root_path_.empty()) {
        auto runs = collect_runs_fs(root_path_, parent_path, {});
        run_candidates.insert(run_candidates.end(), runs.begin(), runs.end());
    }
    std::sort(run_candidates.begin(), run_candidates.end());
    run_candidates.erase(std::unique(run_candidates.begin(),
                                     run_candidates.end()),
                         run_candidates.end());
    if (run_candidates.empty()) {
        return false;
    }

    bool loaded_any = false;
    for (const auto& run_name : run_candidates) {
        const std::string run_base = parent_path + "/" + run_name + "/";
        json run_attrs = json::object();
        if (auto run_attrs_opt = readGroupAttrs(store, run_base)) {
            run_attrs = *run_attrs_opt;
        }
        if (!isCompatibleTrackKinematicsSource(
                run_attrs, compatibility_filter, "source_track_kinematics_run",
                "source_track_id")) {
            continue;
        }
        if (!matchesPreferredSwimBoutSource(run_attrs,
                                            preferred_swim_bout_sources)) {
            continue;
        }

        bool compact_layout =
            jsonStringAttr(run_attrs, "layout") == "compact_tabular_v2";
        if (!compact_layout) {
            if (auto metrics_attrs =
                    readGroupAttrs(store, run_base + "movement_metrics")) {
                compact_layout =
                    jsonStringAttr(*metrics_attrs, "layout") ==
                    "compact_tabular_v2";
            }
        }
        if (!compact_layout && !root_path_.empty()) {
            namespace fs = std::filesystem;
            const fs::path movement_metrics_path =
                fs::path(root_path_) / run_base / "movement_metrics";
            const fs::path level_index_path =
                fs::path(root_path_) / run_base / "level_index";
            compact_layout = fs::exists(movement_metrics_path) &&
                             fs::exists(level_index_path);
        }

        const std::string metrics_base = run_base + "movement/per_bout_metrics/";
        if (!root_path_.empty()) {
            namespace fs = std::filesystem;
            const fs::path metrics_path = fs::path(root_path_) / metrics_base;
            if (!compact_layout && !fs::exists(metrics_path)) {
                continue;
            }
        }

        ZarrDetectionData::BoutKinematicsSeries series;
        series.run_name = run_name;
        series.source_track_kinematics_run =
            jsonStringAttrWithSourceRefs(run_attrs,
                                         "source_track_kinematics_run");
        series.source_track_id =
            jsonIntAttrWithSourceRefs(run_attrs, "source_track_id").value_or(
                jsonTrackIdAttr(run_attrs, "source_track_id"));
        series.source_swim_bout_run =
            jsonStringAttrWithSourceRefs(run_attrs, "source_swim_bout_run");
        series.source_swim_bout_speed_level =
            jsonStringAttrWithSourceRefs(run_attrs,
                                         "source_swim_bout_speed_level");
        series.source_swim_bout_candidate_id =
            jsonIntAttrWithSourceRefs(run_attrs,
                                      "source_swim_bout_candidate_id")
                .value_or(-1);
        series.source_swim_bout_signal_id =
            jsonIntAttrWithSourceRefs(run_attrs, "source_swim_bout_signal_id")
                .value_or(-1);
        series.source_swim_bout_signal_role =
            jsonStringAttrWithSourceRefs(run_attrs,
                                         "source_swim_bout_signal_role");
        series.schema_id = jsonStringAttr(run_attrs, "schema_id");
        series.created_at_utc = jsonStringAttr(run_attrs, "created_at_utc");
        series.movement_metric_source_level =
            jsonStringAttr(run_attrs, "movement_metric_source_level");
        series.is_compact_layout = compact_layout;

        std::string metrics_error;
        loadBoutKinematicsMetrics(store, series, &metrics_error);
        data_.bout_kinematics_series.push_back(std::move(series));
        loaded_any = true;
    }

    if (loaded_any) {
        std::cout << "  [BoutKinematics] Loaded "
                  << data_.bout_kinematics_series.size() << " candidates"
                  << std::endl;
    }
    return loaded_any;
}

bool ZarrDetectionLoader::loadBoutKinematicsMetrics(
    const ts::kvstore::KvStore& store,
    ZarrDetectionData::BoutKinematicsSeries& series,
    std::string* error_message) {
    auto set_error = [&](const std::string& message) {
        series.metrics_load_failed = true;
        series.metrics_load_error = message;
        if (error_message) {
            *error_message = message;
        }
    };

    if (series.run_name.empty()) {
        set_error("missing bout-kinematics run name");
        return false;
    }

    const std::string metrics_base =
        "analysis/bout_kinematics_runs/" + series.run_name +
        "/movement/per_bout_metrics/";

    auto readFrameColumn = [&](const std::string& path,
                               std::vector<int32_t>& out) -> bool {
        std::vector<int64_t> tmp64;
        if (readInt64Array(store, path, tmp64) && !tmp64.empty()) {
            out.resize(tmp64.size());
            for (size_t i = 0; i < tmp64.size(); ++i) {
                out[i] = clampToInt32(tmp64[i]);
            }
            return true;
        }
        return readInt32Array(store, path, out) && !out.empty();
    };

    series.source_start_frame.clear();
    series.source_end_frame.clear();
    series.source_core_start_frame.clear();
    series.source_core_end_frame.clear();
    series.physical_active_start_frame.clear();
    series.physical_active_end_frame.clear();
    series.physical_active_duration_s.clear();
    series.physical_active_path_length_mm.clear();
    series.physical_active_path_length_px.clear();
    series.physical_active_mean_speed_mm_s.clear();
    series.physical_active_peak_speed_mm_s.clear();
    series.physical_active_valid.clear();
    series.failure_reason.clear();
    series.heading_smoothed_net_delta_heading_deg.clear();
    series.heading_raw_net_delta_heading_deg.clear();
    series.eye_gaze_within_bout_vergence_gaze_mean_deg.clear();
    series.compact_movement_metric_count = 0;
    series.compact_heading_smoothed_metric_count = 0;
    series.compact_heading_raw_metric_count = 0;
    series.compact_eye_gaze_metric_count = 0;

    auto trim_to = [](auto& values, size_t row_count) {
        if (!values.empty() && values.size() > row_count) {
            values.resize(row_count);
        }
    };

    if (series.is_compact_layout) {
        const std::string run_base =
            "analysis/bout_kinematics_runs/" + series.run_name + "/";
        const std::string movement_base = run_base + "movement_metrics/";

        readFrameColumn(movement_base + "source_start_frame",
                        series.source_start_frame);
        readFrameColumn(movement_base + "source_end_frame",
                        series.source_end_frame);
        readFrameColumn(movement_base + "source_core_start_frame",
                        series.source_core_start_frame);
        readFrameColumn(movement_base + "source_core_end_frame",
                        series.source_core_end_frame);
        readFrameColumn(movement_base + "physical_active_start_frame",
                        series.physical_active_start_frame);
        readFrameColumn(movement_base + "physical_active_end_frame",
                        series.physical_active_end_frame);
        readFloatArray(store,
                       movement_base + "physical_active_duration_s",
                       series.physical_active_duration_s);
        readFloatArray(store,
                       movement_base + "physical_active_path_length_mm",
                       series.physical_active_path_length_mm);
        readFloatArray(store,
                       movement_base + "physical_active_path_length_px",
                       series.physical_active_path_length_px);
        readFloatArray(store,
                       movement_base + "physical_active_mean_speed_mm_s",
                       series.physical_active_mean_speed_mm_s);
        readFloatArray(store,
                       movement_base + "physical_active_peak_speed_mm_s",
                       series.physical_active_peak_speed_mm_s);
        readBoolArray(store,
                      movement_base + "physical_active_valid",
                      series.physical_active_valid);
        readStringArray(store,
                        movement_base + "failure_reason_bytes",
                        series.failure_reason);

        const size_t movement_count =
            std::max(series.source_start_frame.size(),
                     series.physical_active_duration_s.size());
        series.compact_movement_metric_count = movement_count;

        const std::string heading_base = run_base + "heading_metrics/";
        std::vector<std::string> heading_levels;
        std::vector<float> net_delta_heading_deg;
        readStringArray(store,
                        heading_base + "heading_level_bytes",
                        heading_levels);
        readFloatArray(store,
                       heading_base + "net_delta_heading_deg",
                       net_delta_heading_deg);
        for (size_t i = 0; i < heading_levels.size(); ++i) {
            const std::string level = heading_levels[i];
            const float value =
                i < net_delta_heading_deg.size()
                    ? net_delta_heading_deg[i]
                    : std::numeric_limits<float>::quiet_NaN();
            if (level == "heading_smoothed") {
                series.heading_smoothed_net_delta_heading_deg.push_back(value);
            } else if (level == "heading_raw") {
                series.heading_raw_net_delta_heading_deg.push_back(value);
            }
        }
        series.compact_heading_smoothed_metric_count =
            series.heading_smoothed_net_delta_heading_deg.size();
        series.compact_heading_raw_metric_count =
            series.heading_raw_net_delta_heading_deg.size();

        const std::string eye_gaze_base = run_base + "eye_gaze_metrics/";
        readFloatArray(store,
                       eye_gaze_base +
                           "within_bout_vergence_gaze_mean_deg",
                       series.eye_gaze_within_bout_vergence_gaze_mean_deg);
        std::vector<int32_t> eye_gaze_source_start_frame;
        readFrameColumn(eye_gaze_base + "source_start_frame",
                        eye_gaze_source_start_frame);
        series.compact_eye_gaze_metric_count =
            std::max(eye_gaze_source_start_frame.size(),
                     series.eye_gaze_within_bout_vergence_gaze_mean_deg.size());

        if (movement_count == 0) {
            set_error("no compact movement metric arrays were readable for '" +
                      series.run_name + "'");
            return false;
        }

        trim_to(series.source_start_frame, movement_count);
        trim_to(series.source_end_frame, movement_count);
        trim_to(series.source_core_start_frame, movement_count);
        trim_to(series.source_core_end_frame, movement_count);
        trim_to(series.physical_active_start_frame, movement_count);
        trim_to(series.physical_active_end_frame, movement_count);
        trim_to(series.physical_active_duration_s, movement_count);
        trim_to(series.physical_active_path_length_mm, movement_count);
        trim_to(series.physical_active_path_length_px, movement_count);
        trim_to(series.physical_active_mean_speed_mm_s, movement_count);
        trim_to(series.physical_active_peak_speed_mm_s, movement_count);
        trim_to(series.physical_active_valid, movement_count);
        trim_to(series.failure_reason, movement_count);

        series.metrics_loaded = true;
        series.metrics_load_failed = false;
        series.metrics_load_error.clear();
        std::cout << "  [BoutKinematics] Loaded compact-v2 metrics for '"
                  << series.run_name << "' (movement "
                  << series.compact_movement_metric_count
                  << ", heading_smoothed "
                  << series.compact_heading_smoothed_metric_count
                  << ", heading_raw "
                  << series.compact_heading_raw_metric_count
                  << ", eye_gaze "
                  << series.compact_eye_gaze_metric_count << ")"
                  << std::endl;
        return true;
    }

    readFrameColumn(metrics_base + "source_start_frame",
                    series.source_start_frame);
    readFrameColumn(metrics_base + "source_end_frame",
                    series.source_end_frame);
    readFrameColumn(metrics_base + "source_core_start_frame",
                    series.source_core_start_frame);
    readFrameColumn(metrics_base + "source_core_end_frame",
                    series.source_core_end_frame);
    readFrameColumn(metrics_base + "physical_active_start_frame",
                    series.physical_active_start_frame);
    readFrameColumn(metrics_base + "physical_active_end_frame",
                    series.physical_active_end_frame);
    readFloatArray(store,
                   metrics_base + "physical_active_duration_s",
                   series.physical_active_duration_s);
    readFloatArray(store,
                   metrics_base + "physical_active_path_length_mm",
                   series.physical_active_path_length_mm);
    readFloatArray(store,
                   metrics_base + "physical_active_path_length_px",
                   series.physical_active_path_length_px);
    readFloatArray(store,
                   metrics_base + "physical_active_mean_speed_mm_s",
                   series.physical_active_mean_speed_mm_s);
    readFloatArray(store,
                   metrics_base + "physical_active_peak_speed_mm_s",
                   series.physical_active_peak_speed_mm_s);
    readBoolArray(store,
                  metrics_base + "physical_active_valid",
                  series.physical_active_valid);
    readStringArray(store,
                    metrics_base + "failure_reason_bytes",
                    series.failure_reason);

    const size_t row_count = std::max(series.source_start_frame.size(),
                                      series.physical_active_duration_s.size());
    if (row_count == 0) {
        set_error("no per-bout metric arrays were readable for '" +
                  series.run_name + "'");
        return false;
    }

    trim_to(series.source_start_frame, row_count);
    trim_to(series.source_end_frame, row_count);
    trim_to(series.source_core_start_frame, row_count);
    trim_to(series.source_core_end_frame, row_count);
    trim_to(series.physical_active_start_frame, row_count);
    trim_to(series.physical_active_end_frame, row_count);
    trim_to(series.physical_active_duration_s, row_count);
    trim_to(series.physical_active_path_length_mm, row_count);
    trim_to(series.physical_active_path_length_px, row_count);
    trim_to(series.physical_active_mean_speed_mm_s, row_count);
    trim_to(series.physical_active_peak_speed_mm_s, row_count);
    trim_to(series.physical_active_valid, row_count);
    trim_to(series.failure_reason, row_count);

    series.metrics_loaded = true;
    series.metrics_load_failed = false;
    series.metrics_load_error.clear();
    std::cout << "  [BoutKinematics] Loaded metrics for '"
              << series.run_name << "' (" << row_count << " bouts)"
              << std::endl;
    return true;
}

bool ZarrDetectionLoader::ensureBoutKinematicsMetricsLoaded(
    const std::string& run_name,
    std::string* error_message) {
    auto found = std::find_if(
        data_.bout_kinematics_series.begin(),
        data_.bout_kinematics_series.end(),
        [&](const auto& series) { return series.run_name == run_name; });
    if (found == data_.bout_kinematics_series.end()) {
        if (error_message) {
            *error_message = "unknown bout-kinematics run '" + run_name + "'";
        }
        return false;
    }
    if (found->metrics_loaded) {
        return true;
    }
    if (root_path_.empty()) {
        found->metrics_load_failed = true;
        found->metrics_load_error = "archive path is unavailable";
        if (error_message) {
            *error_message = found->metrics_load_error;
        }
        return false;
    }

    const std::string kvstore_path = normalizeKvstoreFileRootPath(root_path_);
    auto spec_result =
        ts::kvstore::Spec::FromJson({{"driver", "file"},
                                     {"path", kvstore_path}});
    if (!spec_result.ok()) {
        found->metrics_load_failed = true;
        found->metrics_load_error =
            "failed to create kvstore spec: " +
            spec_result.status().ToString();
        if (error_message) {
            *error_message = found->metrics_load_error;
        }
        return false;
    }

    auto store_result = ts::kvstore::Open(spec_result.value(), context_).result();
    if (!store_result.ok()) {
        found->metrics_load_failed = true;
        found->metrics_load_error =
            "failed to open kvstore: " + store_result.status().ToString();
        if (error_message) {
            *error_message = found->metrics_load_error;
        }
        return false;
    }

    return loadBoutKinematicsMetrics(store_result.value(), *found, error_message);
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
        if (auto group_attrs = readGroupAttrs(store, group_path)) {
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

        if (auto run_attrs_opt = readGroupAttrs(store, run_base)) {
            const json& run_attrs = *run_attrs_opt;
            auto readPixelsPerMm = [&](const char* key) {
                if (run_attrs.contains(key) && run_attrs[key].is_number()) {
                    pixels_per_mm = static_cast<float>(run_attrs[key].get<double>());
                }
            };
            readPixelsPerMm("pixels_per_mm");
            readPixelsPerMm("pixels_per_mm_camera");
            readPixelsPerMm("pixel_to_mm");

            std::string crop_candidate;
            crop_candidate = ExtractCropRunFromObject(run_attrs);
            if (run_attrs.contains("inputs") && run_attrs["inputs"].is_object()) {
                const auto& inputs = run_attrs["inputs"];
                std::string from_inputs = ExtractCropRunFromObject(inputs);
                if (!from_inputs.empty()) {
                    crop_candidate = from_inputs;
                }
                if (crop_candidate.empty()) {
                    std::string keypoint_run;
                    if (inputs.contains("keypoint_run") && inputs["keypoint_run"].is_string()) {
                        keypoint_run = inputs["keypoint_run"].get<std::string>();
                    } else if (inputs.contains("base_keypoint_run") && inputs["base_keypoint_run"].is_string()) {
                        keypoint_run = inputs["base_keypoint_run"].get<std::string>();
                    }
                    if (!keypoint_run.empty()) {
                        std::string resolved = ResolveCropRunFromKeypointRun(store, keypoint_run);
                        if (!resolved.empty()) {
                            crop_candidate = resolved;
                        }
                    }
                }
            }
            if (!crop_candidate.empty()) {
                data_.movement_crop_run_name = crop_candidate;
            }

            const std::vector<std::string> track_keys = {"primary_track", "default_track", "track_id"};
            for (const auto& key : track_keys) {
                if (run_attrs.contains(key)) {
                    const auto& value = run_attrs[key];
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
        const std::vector<std::string> smoothed_mm_names = {
            "speed_smoothed_mm",
            "smoothed_speed_mm",
            "speed_smoothed_mm_per_s",
            "smoothed_speed_mm_per_s",
            "speed_smoothed_mmps",
            "smoothed_speed_mmps"
        };
        const std::vector<std::string> smoothed_px_names = {
            "speed_smoothed_px",
            "smoothed_speed_px",
            "speed_smoothed_px_per_s",
            "smoothed_speed_px_per_s",
            "speed_smoothed_pxps",
            "smoothed_speed_pxps"
        };
        const std::vector<std::string> instant_mm_names = {
            "speed_raw_mm",
            "speed_filtered_mm",
            "instantaneous_speed_mm",
            "instantaneous_speed_mm_per_s",
            "instantaneous_speed_mmps"
        };
        const std::vector<std::string> instant_px_names = {
            "speed_raw_px",
            "speed_filtered_px",
            "instantaneous_speed_px",
            "instantaneous_speed_px_per_s",
            "instantaneous_speed_pxps"
        };

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

    if (loaded_any && !data_.movement_crop_run_name.empty()) {
        loadMovementCropRunMetadata(store, data_.movement_crop_run_name);
        if (eagerCropImagesEnabled()) {
            loadMovementCropRun(store, data_.movement_crop_run_name);
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
    const std::vector<uint8_t>* run_has_offline_flags,
    const std::string& speed_level,
    const std::string& primary_speed_label,
    const std::string& secondary_speed_label) {

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

    std::vector<int32_t> detection_indices;
    readInt32Or64List({"detection_indices", "roi_indices", "detection_index"}, detection_indices);

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
                              std::vector<float>& dest,
                              std::string* source_name = nullptr) -> bool {
        for (const auto& name : names) {
            if (readFloatArray(store, track_base + name, dest) && !dest.empty()) {
                if (source_name) {
                    *source_name = track_base + name;
                }
                return true;
            }
        }
        dest.clear();
        return false;
    };

    auto readSpeedValues = [&](const std::vector<std::string>& mm_names,
                               const std::vector<std::string>& px_names,
                               std::vector<float>& dest,
                               const char* label,
                               std::string& units_out,
                               std::string& source_out) -> bool {
        units_out.clear();
        source_out.clear();
        if (readSpeedArray(mm_names, dest, &source_out)) {
            units_out = "mm/s";
            return true;
        }
        if (!px_names.empty()) {
            std::vector<float> px;
            std::string px_source;
            if (readSpeedArray(px_names, px, &px_source) && !px.empty()) {
                if (pixels_per_mm > 1e-6f) {
                    dest.resize(px.size());
                    for (size_t i = 0; i < px.size(); ++i) {
                        dest[i] = px[i] / pixels_per_mm;
                    }
                    units_out = "mm/s";
                    source_out = px_source + " converted from px/s";
                    return true;
                }
                dest = std::move(px);
                units_out = "px/s";
                source_out = px_source;
                if (log_tag != "[TrackKinematics]") {
                    std::cout << "  " << log_tag << " Using " << label
                              << " in px/s for run '" << run_name
                              << "' track '" << track_id
                              << "' (pixels_per_mm missing)" << std::endl;
                }
                return true;
            }
        }
        return false;
    };

    std::vector<float> smoothed_mm;
    std::vector<float> instant_mm;
    std::string smoothed_units;
    std::string instant_units;
    std::string smoothed_source_path;
    std::string instant_source_path;
    bool has_smoothed = readSpeedValues(smoothed_mm_names,
                                        smoothed_px_names,
                                        smoothed_mm,
                                        "primary speed",
                                        smoothed_units,
                                        smoothed_source_path);
    bool has_instant = readSpeedValues(instant_mm_names,
                                       instant_px_names,
                                       instant_mm,
                                       "secondary speed",
                                       instant_units,
                                       instant_source_path);

    std::vector<float> heading_degrees;
    readFloatArray(store, track_base + "heading_degrees", heading_degrees);

    std::vector<float> smoothed_heading_degrees;
    readFloatArray(store, track_base + "smoothed_heading_degrees", smoothed_heading_degrees);

    std::vector<uint8_t> keypoint_success;
    readBoolArray(store, track_base + "keypoint_success", keypoint_success);

    std::vector<uint8_t> sample_valid;
    readBoolArray(store, track_base + "sample_valid", sample_valid);

    std::vector<uint8_t> transition_valid;
    readBoolArray(store, track_base + "transition_valid", transition_valid);

    auto readVec2Array = [&](const std::string& rel_path,
                             std::vector<std::array<float, 2>>& out) -> bool {
        auto readAs = [&](auto token) -> bool {
            using Source = decltype(token);
            auto open_result =
                openArrayAny<Source, 2>(store, track_base + rel_path, context_);
            if (!open_result.ok()) {
                return false;
            }
            auto array_result = ts::Read(open_result.value()).result();
            if (!array_result.ok()) {
                return false;
            }
            auto array = array_result.value();
            if (array.rank() != 2 || array.shape()[1] != 2) {
                return false;
            }
            const size_t rows = static_cast<size_t>(array.shape()[0]);
            out.resize(rows);
            const auto* data = static_cast<const Source*>(array.data());
            for (size_t row = 0; row < rows; ++row) {
                out[row][0] = static_cast<float>(data[row * 2 + 0]);
                out[row][1] = static_cast<float>(data[row * 2 + 1]);
            }
            return true;
        };

        out.clear();
        return readAs(float{}) || readAs(double{});
    };

    std::vector<std::array<float, 2>> positions_px;
    readVec2Array("positions_px", positions_px);

    std::vector<std::array<float, 2>> positions_mm;
    readVec2Array("positions_mm", positions_mm);

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
    if (!sample_valid.empty()) {
        sample_count = std::min(sample_count, sample_valid.size());
    }
    if (!transition_valid.empty()) {
        sample_count = std::min(sample_count, transition_valid.size());
    }
    if (!positions_px.empty()) {
        sample_count = std::min(sample_count, positions_px.size());
    }
    if (!positions_mm.empty()) {
        sample_count = std::min(sample_count, positions_mm.size());
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
    trim_to(sample_valid);
    trim_to(transition_valid);
    trim_to(positions_px);
    trim_to(positions_mm);
    trim_to(frame_indices);
    trim_to(detection_indices);

    if (keypoint_success.empty() && !sample_valid.empty()) {
        keypoint_success = sample_valid;
    }

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
    series.speed_level = speed_level;
    series.primary_speed_label =
        primary_speed_label.empty() ? "Smoothed Speed" : primary_speed_label;
    series.primary_speed_units =
        smoothed_units.empty() ? "mm/s" : smoothed_units;
    series.primary_speed_source_path = smoothed_source_path;
    series.secondary_speed_label =
        secondary_speed_label.empty() ? "Instantaneous Speed"
                                      : secondary_speed_label;
    series.secondary_speed_units =
        instant_units.empty() ? series.primary_speed_units : instant_units;
    series.secondary_speed_source_path = instant_source_path;
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
    series.sample_valid = std::move(sample_valid);
    series.transition_valid = std::move(transition_valid);
    series.positions_px = std::move(positions_px);
    series.positions_mm = std::move(positions_mm);
    series.frame_indices = std::move(frame_indices);
    series.detection_indices = std::move(detection_indices);
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
    series.frame_to_row.reserve(series.frame_indices.size());
    for (size_t i = 0; i < series.frame_indices.size(); ++i) {
        series.frame_to_row.emplace(series.frame_indices[i], i);
    }

    data_.movement_series.push_back(std::move(series));
    std::cout << "  " << log_tag << " Loaded run '" << run_name << "' track '" << track_id
              << "' (" << category
              << (speed_level.empty() ? "" : ", speed " + speed_level)
              << ", samples " << sample_count << ")"
              << std::endl;
    return true;
}

bool ZarrDetectionLoader::loadMovementCropRunMetadata(
    const ts::kvstore::KvStore& store,
    const std::string& crop_run_name) {
    std::string normalized = NormalizeCropRunName(crop_run_name);
    if (normalized.empty()) {
        return false;
    }
    if (data_.crop_data.metadata_loaded &&
        data_.crop_data.run_name == normalized) {
        return true;
    }

    data_.crop_data = {};
    data_.crop_data.run_name = normalized;

    const std::string crop_base = "crop_runs/" + normalized + "/";
    std::cout << "  [CropRun] Resolving crop metadata at '" << crop_base << "'"
              << std::endl;

    std::vector<int32_t> crop_frame_indices;
    if (!readInt32Array(store, crop_base + "frame_indices", crop_frame_indices)) {
        std::vector<int64_t> tmp64;
        if (readInt64Array(store, crop_base + "frame_indices", tmp64)) {
            crop_frame_indices.resize(tmp64.size());
            for (size_t i = 0; i < tmp64.size(); ++i) {
                crop_frame_indices[i] = clampToInt32(tmp64[i]);
            }
        }
    }

    size_t roi_count = crop_frame_indices.size();
    size_t height = 0;
    size_t width = 0;
    size_t channels = 1;
    if (auto image_meta = readNodeMetaV3(store, crop_base + "roi_images")) {
        roi_count = std::max(roi_count, jsonShapeDim(*image_meta, 0));
        height = jsonShapeDim(*image_meta, 1);
        width = jsonShapeDim(*image_meta, 2);
        const size_t channel_dim = jsonShapeDim(*image_meta, 3);
        if (channel_dim > 0) {
            channels = channel_dim;
        }
    }

    if (roi_count == 0) {
        data_.crop_data = {};
        return false;
    }

    if (crop_frame_indices.size() < roi_count) {
        crop_frame_indices.resize(roi_count, -1);
    }
    data_.crop_data.roi_count = roi_count;
    data_.crop_data.height = height;
    data_.crop_data.width = width;
    data_.crop_data.channels = channels;
    data_.crop_data.frame_indices = std::move(crop_frame_indices);
    data_.crop_data.metadata_loaded = true;
    return true;
}

bool ZarrDetectionLoader::loadMovementCropRun(const ts::kvstore::KvStore& store,
                                              const std::string& crop_run_name) {
    std::string normalized = NormalizeCropRunName(crop_run_name);
    if (normalized.empty()) {
        return false;
    }
    if (!loadMovementCropRunMetadata(store, normalized)) {
        return false;
    }
    if (data_.crop_data.loaded && data_.crop_data.run_name == normalized) {
        return true;
    }

    const std::string crop_base = "crop_runs/" + normalized + "/";
    std::cout << "  [CropRun] Loading persisted crop images at '"
              << crop_base << "'" << std::endl;

    auto assignFrameIndices = [&](size_t roi_count) {
        if (data_.crop_data.frame_indices.size() < roi_count) {
            data_.crop_data.frame_indices.resize(roi_count, -1);
        }
    };

    auto load_from_array = [&](auto& store_handle, int rank) -> bool {
        using StoreType = std::decay_t<decltype(store_handle)>;
        if (!store_handle.ok()) {
            return false;
        }
        auto array_result = ts::Read(store_handle.value()).result();
        if (!array_result.ok()) {
            return false;
        }
        auto array = array_result.value();
        if (array.rank() != rank) {
            return false;
        }
        size_t roi_count = static_cast<size_t>(array.shape()[0]);
        size_t height = static_cast<size_t>(array.shape()[1]);
        size_t width = static_cast<size_t>(array.shape()[2]);
        size_t channels = (rank == 4) ? static_cast<size_t>(array.shape()[3]) : 1;
        if (roi_count == 0 || height == 0 || width == 0 || channels == 0) {
            return false;
        }
        size_t total = roi_count * height * width * channels;
        data_.crop_data.images.resize(total);
        const uint8_t* src = static_cast<const uint8_t*>(array.data());
        std::copy(src, src + total, data_.crop_data.images.begin());
        data_.crop_data.roi_count = roi_count;
        data_.crop_data.height = height;
        data_.crop_data.width = width;
        data_.crop_data.channels = channels;
        assignFrameIndices(roi_count);
        data_.crop_data.loaded = true;
        return true;
    };

    auto store4 = openArrayAny<uint8_t, 4>(store, crop_base + "roi_images", context_);
    if (!load_from_array(store4, 4)) {
        auto store3 = openArrayAny<uint8_t, 3>(store, crop_base + "roi_images", context_);
        load_from_array(store3, 3);
    }

    if (!data_.crop_data.loaded) {
        data_.crop_data.images.clear();
        return false;
    }

    if (kChaserDebugLoggingEnabled) {
        std::cout << "  [CropRun] Loaded '" << normalized << "' (roi_count="
                  << data_.crop_data.roi_count << ", size="
                  << data_.crop_data.height << "x" << data_.crop_data.width
                  << ", channels=" << data_.crop_data.channels << ")" << std::endl;
    }
    return true;
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
        if (lower.find("track_kinematics") != std::string::npos) return 6;
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

std::optional<ZarrDetectionLoader::MovementFrameSample>
ZarrDetectionLoader::getMovementSampleForFrame(int32_t frame_index) const {
    const auto* series = getSelectedMovementSeries();
    if (!series || frame_index < 0) {
        return std::nullopt;
    }

    auto row_it = series->frame_to_row.find(frame_index);
    if (row_it == series->frame_to_row.end()) {
        return std::nullopt;
    }

    const size_t row = row_it->second;
    ZarrDetectionLoader::MovementFrameSample sample;
    sample.valid = true;
    sample.frame_index = frame_index;
    sample.row_index = row;
    sample.category = series->category;
    sample.run_name = series->run_name;
    sample.track_id = series->track_id;
    sample.speed_level = series->speed_level;
    sample.speed_label = series->primary_speed_label.empty()
                             ? "Speed"
                             : series->primary_speed_label;
    sample.speed_units = series->primary_speed_units.empty()
                             ? "mm/s"
                             : series->primary_speed_units;

    if (row < series->smoothed_speed_mm.size()) {
        const float speed = series->smoothed_speed_mm[row];
        if (std::isfinite(static_cast<double>(speed))) {
            sample.has_speed = true;
            sample.speed = speed;
        }
    }

    if (row < series->smoothed_heading_degrees.size()) {
        const float heading = series->smoothed_heading_degrees[row];
        if (std::isfinite(static_cast<double>(heading))) {
            sample.has_heading = true;
            sample.heading_smoothed = true;
            sample.heading_degrees = heading;
        }
    }
    if (!sample.has_heading && row < series->heading_degrees.size()) {
        const float heading = series->heading_degrees[row];
        if (std::isfinite(static_cast<double>(heading))) {
            sample.has_heading = true;
            sample.heading_smoothed = false;
            sample.heading_degrees = heading;
        }
    }

    if (row < series->positions_px.size()) {
        const auto& point = series->positions_px[row];
        if (std::isfinite(static_cast<double>(point[0])) &&
            std::isfinite(static_cast<double>(point[1]))) {
            sample.has_position_px = true;
            sample.x_px = point[0];
            sample.y_px = point[1];
        }
    }
    if (row < series->sample_valid.size()) {
        sample.has_sample_valid = true;
        sample.sample_valid = series->sample_valid[row] != 0;
    }
    if (row < series->transition_valid.size()) {
        sample.has_transition_valid = true;
        sample.transition_valid = series->transition_valid[row] != 0;
    }

    return sample;
}

std::vector<ZarrDetectionLoader::MovementTrailPoint>
ZarrDetectionLoader::getMovementTrailForFrame(
    int32_t frame_index,
    double duration_seconds,
    bool valid_samples_only) const {
    std::vector<MovementTrailPoint> trail;
    const auto* series = getSelectedMovementSeries();
    if (!series || frame_index < 0 || duration_seconds <= 0.0 ||
        series->positions_px.empty()) {
        return trail;
    }

    size_t current_row = std::numeric_limits<size_t>::max();
    if (const auto row_it = series->frame_to_row.find(frame_index);
        row_it != series->frame_to_row.end()) {
        current_row = row_it->second;
    } else if (!series->frame_indices.empty()) {
        const auto upper = std::upper_bound(series->frame_indices.begin(),
                                            series->frame_indices.end(),
                                            frame_index);
        if (upper == series->frame_indices.begin()) {
            return trail;
        }
        current_row = static_cast<size_t>(
            std::distance(series->frame_indices.begin(), upper) - 1);
    } else {
        current_row = static_cast<size_t>(frame_index);
    }

    if (current_row >= series->positions_px.size()) {
        return trail;
    }

    const double fps =
        series->fps > 0.0 ? series->fps : (data_.fps > 0.0 ? data_.fps : 60.0);
    auto sample_time_seconds = [&](size_t row) -> double {
        if (row < series->time_seconds.size()) {
            const double t = static_cast<double>(series->time_seconds[row]);
            if (std::isfinite(t)) {
                return t;
            }
        }
        if (row < series->frame_indices.size() && fps > 0.0) {
            return static_cast<double>(series->frame_indices[row]) / fps;
        }
        return fps > 0.0 ? static_cast<double>(row) / fps
                         : static_cast<double>(row);
    };

    const double current_time = sample_time_seconds(current_row);
    constexpr size_t kMaxTrailPoints = 600;
    for (size_t row = current_row + 1; row > 0 && trail.size() < kMaxTrailPoints;) {
        --row;
        if (row >= series->positions_px.size()) {
            continue;
        }

        const double age = current_time - sample_time_seconds(row);
        if (age < -1e-6) {
            continue;
        }
        if (age > duration_seconds) {
            break;
        }

        const auto& point = series->positions_px[row];
        if (!std::isfinite(static_cast<double>(point[0])) ||
            !std::isfinite(static_cast<double>(point[1]))) {
            continue;
        }

        const bool sample_valid =
            row >= series->sample_valid.size() || series->sample_valid[row] != 0;
        const bool transition_valid =
            row >= series->transition_valid.size() ||
            series->transition_valid[row] != 0;
        if (valid_samples_only && (!sample_valid || !transition_valid)) {
            continue;
        }

        MovementTrailPoint trail_point;
        trail_point.frame_index =
            row < series->frame_indices.size()
                ? series->frame_indices[row]
                : static_cast<int32_t>(row);
        trail_point.row_index = row;
        trail_point.x_px = point[0];
        trail_point.y_px = point[1];
        trail_point.age_seconds = static_cast<float>(age);
        const double normalized_age =
            duration_seconds > 0.0 ? std::clamp(age / duration_seconds, 0.0, 1.0)
                                   : 0.0;
        trail_point.alpha =
            static_cast<float>(std::clamp(1.0 - normalized_age, 0.0, 1.0));
        trail_point.sample_valid = sample_valid;
        trail_point.transition_valid = transition_valid;
        trail.push_back(trail_point);
    }

    std::reverse(trail.begin(), trail.end());

    const int32_t gap_frame_threshold =
        std::max<int32_t>(2, static_cast<int32_t>(std::ceil(fps * 0.1)));
    for (size_t i = 1; i < trail.size(); ++i) {
        bool gap = false;
        if (trail[i - 1].frame_index >= 0 && trail[i].frame_index >= 0) {
            gap = (trail[i].frame_index - trail[i - 1].frame_index) >
                  gap_frame_threshold;
        } else {
            gap = trail[i].row_index > trail[i - 1].row_index + 1;
        }
        if (gap || !trail[i - 1].sample_valid || !trail[i - 1].transition_valid ||
            !trail[i].sample_valid || !trail[i].transition_valid) {
            trail[i].break_before = true;
        }
    }

    return trail;
}

bool ZarrDetectionLoader::selectMovementSeries(size_t index) {
    if (index >= data_.movement_series.size()) {
        return false;
    }
    data_.movement_selected_index = index;
    data_.has_movement_data = true;
    return true;
}
