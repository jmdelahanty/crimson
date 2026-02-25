#include "ui_path_config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

std::string ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool IsRegularFileNoThrow(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

bool IsDirectoryNoThrow(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

std::string ExpandUserPath(const std::string& path) {
    if (path.empty() || path[0] != '~') {
        return path;
    }

    const char* home_env = std::getenv("HOME");
    if (!home_env || std::string(home_env).empty()) {
        return path;
    }
    const std::string home(home_env);

    if (path.size() == 1) {
        return home;
    }
    if (path[1] == '/') {
        return home + path.substr(1);
    }

    // Unsupported "~user" expansion; leave unchanged.
    return path;
}

static std::optional<UiPathConfig> LoadUiPathConfigFile(const std::filesystem::path& config_path) {
    if (!IsRegularFileNoThrow(config_path)) {
        return std::nullopt;
    }

    UiPathConfig config;
    std::ifstream in(config_path);
    if (!in) {
        return std::nullopt;
    }

    json payload;
    try {
        in >> payload;
    } catch (const std::exception& e) {
        std::cerr << "[UIPathConfig] Failed to parse " << config_path << ": "
                  << e.what() << std::endl;
        return std::nullopt;
    }

    if (payload.contains("default_start_path") &&
        payload["default_start_path"].is_string()) {
        config.default_start_path = ExpandUserPath(
            payload["default_start_path"].get<std::string>());
    }

    if (payload.contains("preferred_roots") && payload["preferred_roots"].is_array()) {
        config.preferred_roots.clear();
        for (const auto& item : payload["preferred_roots"]) {
            if (!item.is_string()) {
                continue;
            }
            config.preferred_roots.push_back(ExpandUserPath(item.get<std::string>()));
        }
    }

    std::vector<std::string> filtered_roots;
    filtered_roots.reserve(config.preferred_roots.size());
    for (const auto& root : config.preferred_roots) {
        if (root.empty()) {
            continue;
        }
        if (IsDirectoryNoThrow(root)) {
            filtered_roots.push_back(root);
        }
    }
    config.preferred_roots = std::move(filtered_roots);

    if ((!config.default_start_path.empty()) &&
        !IsDirectoryNoThrow(config.default_start_path)) {
        config.default_start_path.clear();
    }
    if (config.default_start_path.empty() && !config.preferred_roots.empty()) {
        config.default_start_path = config.preferred_roots.front();
    }
    if (config.default_start_path.empty()) {
        config.default_start_path = "/nvme1";
    }
    if (config.preferred_roots.empty()) {
        config.preferred_roots.push_back(config.default_start_path);
    }

    config.loaded_from = config_path.string();
    return config;
}

UiPathConfig LoadUiPathConfig(const std::filesystem::path& current_working_dir,
                              const std::filesystem::path& argv0_path) {
    std::vector<std::filesystem::path> candidates;

    if (const char* env_config = std::getenv("CRIMSON_UI_PATHS_CONFIG")) {
        if (*env_config != '\0') {
            candidates.push_back(std::filesystem::path(ExpandUserPath(env_config)));
        }
    }

    candidates.push_back(current_working_dir / "config" / "ui_paths.json");
    candidates.push_back(current_working_dir / "ui_paths.json");

    std::error_code ec;
    auto exe_abs = std::filesystem::absolute(argv0_path, ec);
    if (!ec && !exe_abs.empty()) {
        auto exe_dir = exe_abs.parent_path();
        if (!exe_dir.empty()) {
            auto repo_like_root = exe_dir.parent_path();
            if (!repo_like_root.empty()) {
                candidates.push_back(repo_like_root / "config" / "ui_paths.json");
            }
        }
    }

    if (const char* home = std::getenv("HOME")) {
        if (*home != '\0') {
            candidates.push_back(
                std::filesystem::path(home) / ".config" / "crimson" / "ui_paths.json");
        }
    }

    for (const auto& candidate : candidates) {
        if (auto config = LoadUiPathConfigFile(candidate)) {
            return *config;
        }
    }

    UiPathConfig fallback;
    if (!IsDirectoryNoThrow(fallback.default_start_path)) {
        fallback.default_start_path = current_working_dir.string();
        fallback.preferred_roots = {fallback.default_start_path};
    }
    return fallback;
}

bool IsSupportedVideoPath(const std::filesystem::path& path) {
    std::string ext = ToLowerCopy(path.extension().string());
    return ext == ".mp4" || ext == ".mov" || ext == ".mkv" || ext == ".avi";
}

std::optional<std::filesystem::path> ResolveAffiliatedVideoPath(
    const std::string& source_path_hint,
    const std::string& archive_path) {
    namespace fs = std::filesystem;

    if (source_path_hint.empty()) {
        return std::nullopt;
    }

    fs::path hint(source_path_hint);
    std::vector<fs::path> candidates;

    if (hint.is_absolute()) {
        candidates.push_back(hint);
    }

    if (!archive_path.empty()) {
        fs::path archive_root(archive_path);
        fs::path archive_parent = archive_root.parent_path();
        fs::path recording_root = archive_parent;
        if (!archive_parent.empty() && archive_parent.filename() == "zarr") {
            recording_root = archive_parent.parent_path();
        }

        if (!hint.is_absolute()) {
            candidates.push_back(archive_root / hint);
            candidates.push_back(archive_parent / hint);
            if (!recording_root.empty()) {
                candidates.push_back(recording_root / hint);
                candidates.push_back(recording_root / "cams" / hint.filename());
                candidates.push_back(recording_root / hint.filename());
            }
        }
    }

    if (!hint.is_absolute()) {
        candidates.push_back(hint);
    }

    for (const auto& candidate : candidates) {
        if (IsRegularFileNoThrow(candidate) && IsSupportedVideoPath(candidate)) {
            return candidate;
        }
    }
    for (const auto& candidate : candidates) {
        if (IsRegularFileNoThrow(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

static std::optional<std::filesystem::path> TryStimulusHintCandidates(
    const std::filesystem::path& hint,
    const std::string& archive_path,
    const std::string& recording_root) {
    namespace fs = std::filesystem;

    std::vector<fs::path> candidates;

    if (hint.is_absolute()) {
        candidates.push_back(hint);
    }

    if (!archive_path.empty()) {
        fs::path archive_root(archive_path);
        fs::path archive_parent = archive_root.parent_path();
        fs::path rec_root = archive_parent;
        if (!archive_parent.empty() && archive_parent.filename() == "zarr") {
            rec_root = archive_parent.parent_path();
        }

        if (!hint.is_absolute()) {
            candidates.push_back(archive_root / hint);
            candidates.push_back(archive_parent / hint);
            if (!rec_root.empty()) {
                candidates.push_back(rec_root / hint);
                candidates.push_back(rec_root / "raw" / hint.filename());
                candidates.push_back(rec_root / hint.filename());
            }
        }
    }

    if (!recording_root.empty()) {
        fs::path rr(recording_root);
        if (!hint.is_absolute()) {
            candidates.push_back(rr / hint);
            candidates.push_back(rr / "raw" / hint.filename());
            candidates.push_back(rr / hint.filename());
        }
    }

    if (!hint.is_absolute()) {
        candidates.push_back(hint);
    }

    for (const auto& candidate : candidates) {
        if (IsRegularFileNoThrow(candidate) && IsSupportedVideoPath(candidate)) {
            return candidate;
        }
    }
    for (const auto& candidate : candidates) {
        if (IsRegularFileNoThrow(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> ResolveStimulusVideoPath(
    const std::string& stimulus_video_hint,
    const std::string& source_h5_hint,
    const std::string& archive_path,
    const std::string& recording_root) {
    namespace fs = std::filesystem;

    // Strategy 1: Direct path from source_stimulus_video_path attr
    if (!stimulus_video_hint.empty()) {
        auto result = TryStimulusHintCandidates(
            fs::path(stimulus_video_hint), archive_path, recording_root);
        if (result.has_value()) {
            return result;
        }
    }

    // Strategy 2: Derive .mp4 from source_h5 attr
    if (!source_h5_hint.empty()) {
        fs::path h5_path(source_h5_hint);
        fs::path mp4_path = h5_path;
        mp4_path.replace_extension(".mp4");
        auto result = TryStimulusHintCandidates(mp4_path, archive_path, recording_root);
        if (result.has_value()) {
            return result;
        }
    }

    // Strategy 3: Filesystem scan of recording_root/raw/ for any .mp4
    if (!recording_root.empty()) {
        fs::path raw_dir = fs::path(recording_root) / "raw";
        if (IsDirectoryNoThrow(raw_dir)) {
            std::error_code ec;
            for (auto it = fs::directory_iterator(raw_dir, ec);
                 it != fs::directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (it->is_regular_file(ec) && !ec &&
                    IsSupportedVideoPath(it->path())) {
                    return it->path();
                }
            }
        }
    }

    return std::nullopt;
}

std::filesystem::path InferRecordingRootPath(
    const std::filesystem::path& video_path,
    const std::string& archive_path) {
    namespace fs = std::filesystem;

    if (!archive_path.empty()) {
        fs::path archive_root(archive_path);
        fs::path archive_parent = archive_root.parent_path();
        if (!archive_parent.empty() && archive_parent.filename() == "zarr") {
            fs::path candidate = archive_parent.parent_path();
            if (!candidate.empty()) {
                return candidate;
            }
        }
    }

    fs::path parent = video_path.parent_path();
    if (!parent.empty() && parent.filename() == "cams") {
        fs::path candidate = parent.parent_path();
        if (!candidate.empty()) {
            return candidate;
        }
    }
    return parent;
}
