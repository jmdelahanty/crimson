#include "zarr_loader_internal.h"
#include <iostream>

using json = nlohmann::json;

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
    constexpr const char* kZarrJsonKey = "zarr.json";
    constexpr size_t kZarrJsonKeyLen = 9;
    const std::array<std::string, 2> keys = {
        appendPath(path, "zarr.json"),
        appendPath(path, ".zattrs")
    };

    std::string first_read_error;

    for (const std::string& key : keys) {
#if defined(_WIN32)
        auto store_spec = store.spec();
        if (store_spec.ok()) {
            auto store_json = store_spec.value().ToJson();
            if (store_json.ok()) {
                crimson::windows_path::noteZarrPathAccess(
                    store_json.value(), key);
            }
        }
#endif
        auto read_result = ts::kvstore::Read(store, key).result();
        if (!read_result.ok()) {
            if (first_read_error.empty()) {
                first_read_error = read_result.status().ToString();
            }
            continue;
        }
        if (!read_result.value().has_value()) {
            continue;
        }

        try {
            std::string payload;
            absl::CopyCordToString(read_result.value().value, &payload);
            if (payload.empty()) {
                continue;
            }

            json meta = json::parse(payload);
            const bool is_zarr_json_key =
                key.size() >= kZarrJsonKeyLen &&
                key.compare(key.size() - kZarrJsonKeyLen, kZarrJsonKeyLen, kZarrJsonKey) == 0;
            if (is_zarr_json_key) {
                if (meta.contains("attributes") && meta["attributes"].is_object()) {
                    return meta["attributes"];
                }
                if (meta.is_object()) {
                    return meta;
                }
            } else if (meta.is_object()) {
                return meta;
            }
        } catch (const json::parse_error& e) {
            std::cerr << "Failed to parse JSON at " << key << ": "
                      << e.what() << std::endl;
        } catch (...) {
            // Ignore malformed attribute blobs.
        }
    }

    if (!first_read_error.empty()) {
        std::cout << "  [AttrProbe] Read error for " << keys.front()
                  << ": " << first_read_error << std::endl;
    } else if (kAttrProbeMissLoggingEnabled) {
        std::cout << "  [AttrProbe] No value at " << keys.front() << std::endl;
    }
    return std::nullopt;
}

std::optional<json> readNodeMetaV3(const ts::kvstore::KvStore& store,
                                   const std::string& path) {
#if defined(_WIN32)
    auto store_spec = store.spec();
    if (store_spec.ok()) {
        auto store_json = store_spec.value().ToJson();
        if (store_json.ok()) {
            crimson::windows_path::noteZarrPathAccess(
                store_json.value(), appendPath(path, "zarr.json"));
        }
    }
#endif
    auto read_result =
        ts::kvstore::Read(store, appendPath(path, "zarr.json")).result();
    if (!read_result.ok() || !read_result.value().has_value()) {
        return std::nullopt;
    }

    try {
        std::string payload;
        absl::CopyCordToString(read_result.value().value, &payload);
        if (payload.empty()) {
            return std::nullopt;
        }
        json meta = json::parse(payload);
        if (meta.is_object()) {
            return meta;
        }
    } catch (...) {
        // Ignore malformed metadata for optional probing.
    }
    return std::nullopt;
}

std::string normalizeKvstoreFileRootPath(std::filesystem::path path) {
    path = path.lexically_normal();
    std::string normalized = path.generic_string();
    if (!normalized.empty() && normalized.back() != '/') {
        normalized.push_back('/');
    }
    return normalized;
}

std::string toLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool hasZarrArchiveExtension(const std::filesystem::path& path) {
    const std::string ext = toLowerCopy(path.extension().string());
    return ext == ".zarr" || ext == ".zr3";
}

bool usesZarrV3StringDataType(const json& node_meta) {
    if (!node_meta.is_object() || !node_meta.contains("data_type")) {
        return false;
    }

    const auto& dtype = node_meta["data_type"];
    if (dtype.is_string()) {
        const std::string kind = toLowerCopy(dtype.get<std::string>());
        return kind == "string" || kind == "str" || kind == "utf8" || kind == "utf-8";
    }
    if (!dtype.is_object()) {
        return false;
    }

    if (dtype.contains("name") && dtype["name"].is_string()) {
        const std::string name = toLowerCopy(dtype["name"].get<std::string>());
        if (name == "string" || name == "utf8" || name == "utf-8") {
            return true;
        }
    }
    if (dtype.contains("kind") && dtype["kind"].is_string()) {
        const std::string kind = toLowerCopy(dtype["kind"].get<std::string>());
        if (kind == "string" || kind == "str" || kind == "utf8" || kind == "utf-8") {
            return true;
        }
    }
    return false;
}

std::optional<json> readAttrsFromZarrJsonFile(const std::filesystem::path& zarr_json_path) {
    try {
        std::ifstream file(zarr_json_path);
        if (!file) {
            return std::nullopt;
        }
        json meta;
        file >> meta;
        if (meta.contains("attributes") && meta["attributes"].is_object()) {
            return meta["attributes"];
        }
        if (meta.is_object()) {
            return meta;
        }
    } catch (...) {
        // Ignore malformed JSON during discovery probes.
    }
    return std::nullopt;
}

bool isAnalysisArchiveByAttrs(const std::filesystem::path& archive_root) {
    const auto attrs = readAttrsFromZarrJsonFile(archive_root / "zarr.json");
    if (!attrs.has_value() || !attrs->is_object()) {
        return false;
    }
    if (!attrs->contains("zarr_purpose") || !(*attrs)["zarr_purpose"].is_string()) {
        return false;
    }
    const std::string purpose = toLowerCopy((*attrs)["zarr_purpose"].get<std::string>());
    return purpose == "analysis";
}

int scoreDiscoveryCandidate(const ZarrDiscoveryCandidate& candidate) {
    int score = 0;
    if (candidate.has_detect_runs) {
        score += 200;
    }
    if (candidate.is_analysis_archive) {
        score += 100;
    }
    if (candidate.name_suggests_analysis) {
        score += 25;
    }
    if (candidate.name_suggests_training) {
        score -= 25;
    }
    return score;
}

void appendDiscoveryCandidatesInDirectory(
    const std::filesystem::path& directory_path,
    std::vector<ZarrDiscoveryCandidate>& candidates) {
    namespace fs = std::filesystem;

    for (const auto& entry : fs::directory_iterator(directory_path)) {
        if (!entry.is_directory()) {
            continue;
        }

        const fs::path archive_path = entry.path();
        if (!hasZarrArchiveExtension(archive_path)) {
            continue;
        }

        if (!fs::exists(archive_path / "zarr.json")) {
            continue;
        }

        ZarrDiscoveryCandidate candidate;
        candidate.path = archive_path;
        candidate.name = archive_path.filename().string();
        const std::string lowered_name = toLowerCopy(candidate.name);
        candidate.name_suggests_analysis =
            lowered_name.find("analysis") != std::string::npos;
        candidate.name_suggests_training =
            lowered_name.find("training") != std::string::npos;
        candidate.has_detect_runs = fs::exists(archive_path / "detect_runs" / "zarr.json");
        candidate.is_analysis_archive = isAnalysisArchiveByAttrs(archive_path);
        candidate.rank = scoreDiscoveryCandidate(candidate);

        if (candidate.rank > 0) {
            candidates.push_back(std::move(candidate));
        }
    }
}

ZarrDiscoveryResult discoverZarrArchiveInDirectory(const std::string& directory) {
    namespace fs = std::filesystem;

    ZarrDiscoveryResult result;
    fs::path directory_path(directory);
    if (!fs::exists(directory_path) || !fs::is_directory(directory_path)) {
        result.error_message = "Directory does not exist or is not readable: " + directory;
        return result;
    }

    std::vector<ZarrDiscoveryCandidate> candidates;
    try {
        appendDiscoveryCandidatesInDirectory(directory_path, candidates);

        if (candidates.empty()) {
            const fs::path nested_zarr_dir = directory_path / "zarr";
            if (fs::exists(nested_zarr_dir) && fs::is_directory(nested_zarr_dir)) {
                appendDiscoveryCandidatesInDirectory(nested_zarr_dir, candidates);
            }
        }
    } catch (const std::exception& e) {
        result.error_message = std::string("Error searching for zarr archives: ") + e.what();
        return result;
    }

    if (candidates.empty()) {
        result.error_message =
            "No compatible .zarr/.zr3 archives found (expected detect_runs or zarr_purpose=analysis)";
        return result;
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const ZarrDiscoveryCandidate& a, const ZarrDiscoveryCandidate& b) {
                  if (a.rank != b.rank) {
                      return a.rank > b.rank;
                  }
                  return a.name < b.name;
              });

    const int top_rank = candidates.front().rank;
    std::vector<std::string> top_candidates;
    for (const auto& candidate : candidates) {
        if (candidate.rank == top_rank) {
            top_candidates.push_back(candidate.path.string());
        }
    }

    if (top_candidates.size() > 1) {
        std::ostringstream oss;
        oss << "Ambiguous zarr archive selection. Multiple equally ranked candidates found:";
        for (const auto& path : top_candidates) {
            oss << "\n  - " << path;
        }
        oss << "\nUse an explicit archive path override.";
        result.error_message = oss.str();
        return result;
    }

    result.selected_archive = candidates.front().path;
    return result;
}

std::optional<std::filesystem::path> resolveExplicitZarrArchivePath(
    const std::string& candidate_path) {
    namespace fs = std::filesystem;

    if (candidate_path.empty()) {
        return std::nullopt;
    }

    fs::path input(candidate_path);
    fs::path current = input;
    if (!fs::exists(current) || fs::is_regular_file(current)) {
        current = current.parent_path();
    }

    while (!current.empty()) {
        if (fs::is_directory(current) &&
            hasZarrArchiveExtension(current) &&
            fs::exists(current / "zarr.json")) {
            return current;
        }
        if (current == current.root_path()) {
            break;
        }
        current = current.parent_path();
    }

    if (fs::is_directory(input) && fs::exists(input / "zarr.json")) {
        return input;
    }

    return std::nullopt;
}

std::string NormalizeCropRunName(const std::string& name) {
    constexpr std::string_view prefix = "crop_runs/";
    if (name.rfind(prefix.data(), 0) == 0) {
        return name.substr(prefix.size());
    }
    return name;
}

std::string ExtractCropRunFromObject(const json& obj) {
    const char* keys[] = {"crop_run", "source_crop_run", "base_crop_run"};
    for (const char* key : keys) {
        if (obj.contains(key) && obj[key].is_string()) {
            return NormalizeCropRunName(obj[key].get<std::string>());
        }
    }
    return "";
}

std::string ResolveCropRunFromKeypointRun(const ts::kvstore::KvStore& store,
                                          const std::string& run_name) {
    if (run_name.empty()) {
        return "";
    }
    std::string current = NormalizeCropRunName(run_name);
    std::unordered_set<std::string> visited;
    while (!current.empty() && visited.insert(current).second) {
        auto kp_attrs = readAttrsAny(store, "analysis/keypoints_runs/" + current + "/");
        if (!kp_attrs.has_value()) {
            break;
        }
        std::string crop = ExtractCropRunFromObject(*kp_attrs);
        if (crop.empty() && kp_attrs->contains("inputs") && (*kp_attrs)["inputs"].is_object()) {
            crop = ExtractCropRunFromObject((*kp_attrs)["inputs"]);
        }
        if (!crop.empty()) {
            return crop;
        }
        std::string next;
        if (kp_attrs->contains("inputs") && (*kp_attrs)["inputs"].is_object()) {
            const auto& inputs = (*kp_attrs)["inputs"];
            if (inputs.contains("base_keypoint_run") && inputs["base_keypoint_run"].is_string()) {
                next = NormalizeCropRunName(inputs["base_keypoint_run"].get<std::string>());
            }
        }
        if (next.empty() && kp_attrs->contains("base_keypoint_run") &&
            (*kp_attrs)["base_keypoint_run"].is_string()) {
            next = NormalizeCropRunName((*kp_attrs)["base_keypoint_run"].get<std::string>());
        }
        if (next.empty()) {
            break;
        }
        current = next;
    }
    return "";
}

std::string currentUtcIsoTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm utc_tm{};
#if defined(_WIN32)
    gmtime_s(&utc_tm, &now_time);
#else
    gmtime_r(&now_time, &utc_tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

json makeEmptyGroupMetadataV3() {
    return json{
        {"attributes", json::object()},
        {"zarr_format", 3},
        {"consolidated_metadata", nullptr},
        {"node_type", "group"}};
}

json normalizeGroupMetadataV3(json node_meta) {
    if (!node_meta.is_object()) {
        node_meta = makeEmptyGroupMetadataV3();
    }
    if (!node_meta.contains("attributes") || !node_meta["attributes"].is_object()) {
        node_meta["attributes"] = json::object();
    }
    node_meta["zarr_format"] = 3;
    if (!node_meta.contains("consolidated_metadata")) {
        node_meta["consolidated_metadata"] = nullptr;
    }
    node_meta["node_type"] = "group";
    return node_meta;
}

bool writeNodeMetaV3(const ts::kvstore::KvStore& store,
                     const std::string& path,
                     const json& node_meta,
                     std::string* error_message) {
    const std::string payload = node_meta.dump(2);
    auto write_result = ts::kvstore::Write(
        store,
        appendPath(path, "zarr.json"),
        std::optional<ts::kvstore::Value>(absl::Cord(payload))).result();
    if (!write_result.ok()) {
        if (error_message) {
            *error_message =
                "Failed to write metadata for '" + path + "': " +
                write_result.status().ToString();
        }
        return false;
    }
    return true;
}

json makeNumericArrayMetadata(const std::vector<ts::Index>& shape,
                              const std::vector<ts::Index>& chunk_shape,
                              const std::string& data_type,
                              const json& fill_value,
                              bool bytes_endian_little) {
    json bytes_codec = {{"name", "bytes"}};
    if (bytes_endian_little) {
        bytes_codec["configuration"] = {{"endian", "little"}};
    }
    return json{
        {"shape", shape},
        {"data_type", data_type},
        {"chunk_grid",
         {
             {"name", "regular"},
             {"configuration", {{"chunk_shape", chunk_shape}}},
         }},
        {"chunk_key_encoding",
         {
             {"name", "default"},
             {"configuration", {{"separator", "/"}}},
         }},
        {"fill_value", fill_value},
        {"codecs",
         json::array(
             {bytes_codec,
              json{{"name", "zstd"},
                   {"configuration", {{"level", 0}, {"checksum", false}}}}})},
        {"attributes", json::object()},
        {"zarr_format", 3},
        {"node_type", "array"},
        {"storage_transformers", json::array()}
    };
}

bool arrayExists(const ts::kvstore::KvStore& store, const std::string& path) {
    auto v3 = ts::kvstore::Read(store, appendPath(path, "zarr.json")).result();
    return v3.ok() && v3.value().has_value();
}

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
    if (attrs.contains("latest_complete") && attrs["latest_complete"].is_string()) {
        return attrs["latest_complete"].get<std::string>();
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
