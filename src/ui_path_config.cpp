#include "ui_path_config.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::optional<std::string> GetEnvValue(const char* name) {
    const char* value = std::getenv(name);
    if (!value || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

std::optional<fs::path> NormalizePath(const fs::path& path) {
    if (path.empty()) {
        return std::nullopt;
    }

    std::error_code ec;
    fs::path absolute_path = path;
    if (!absolute_path.is_absolute()) {
        absolute_path = fs::absolute(absolute_path, ec);
        if (ec) {
            absolute_path = path;
            ec.clear();
        }
    }

    fs::path canonical_path = fs::weakly_canonical(absolute_path, ec);
    if (!ec) {
        return canonical_path;
    }
    return absolute_path.lexically_normal();
}

void AppendUniquePath(std::vector<fs::path>& paths, const fs::path& candidate) {
    auto normalized = NormalizePath(candidate);
    if (!normalized) {
        return;
    }
    if (std::find(paths.begin(), paths.end(), *normalized) == paths.end()) {
        paths.push_back(*normalized);
    }
}

std::optional<fs::path> RelocateStoredPathToRecordingRoot(
    const fs::path& stored_path,
    const fs::path& recording_root) {
    if (!stored_path.is_absolute() || recording_root.empty() ||
        recording_root.filename().empty()) {
        return std::nullopt;
    }

    bool found_recording = false;
    fs::path suffix;
    for (const auto& component : stored_path) {
        if (!found_recording) {
            found_recording = component == recording_root.filename();
            continue;
        }
        suffix /= component;
    }
    if (!found_recording || suffix.empty()) {
        return std::nullopt;
    }
    return recording_root / suffix;
}

std::optional<fs::path> GetHomeDirectory() {
    if (auto home = GetEnvValue("HOME")) {
        return fs::path(*home);
    }
#ifdef _WIN32
    if (auto userprofile = GetEnvValue("USERPROFILE")) {
        return fs::path(*userprofile);
    }
    auto home_drive = GetEnvValue("HOMEDRIVE");
    auto home_path = GetEnvValue("HOMEPATH");
    if (home_drive && home_path) {
        return fs::path(*home_drive + *home_path);
    }
#endif
    return std::nullopt;
}

std::optional<fs::path> GetExpandedEnvPath(const char* name) {
    auto value = GetEnvValue(name);
    if (!value) {
        return std::nullopt;
    }
    return fs::path(ExpandUserPath(*value));
}

std::optional<fs::path> GetCrimsonUserConfigDir() {
    if (auto xdg_config_home = GetExpandedEnvPath("XDG_CONFIG_HOME")) {
        return *xdg_config_home / "crimson";
    }
#ifdef _WIN32
    if (auto appdata = GetExpandedEnvPath("APPDATA")) {
        return *appdata / "crimson";
    }
#endif
#ifdef __APPLE__
    if (auto home = GetHomeDirectory()) {
        return *home / "Library" / "Application Support" / "Crimson";
    }
#endif
    if (auto home = GetHomeDirectory()) {
        return *home / ".config" / "crimson";
    }
    return std::nullopt;
}

std::optional<fs::path> GetCrimsonUserCacheDir() {
    if (auto xdg_cache_home = GetExpandedEnvPath("XDG_CACHE_HOME")) {
        return *xdg_cache_home / "crimson";
    }
#ifdef _WIN32
    if (auto localappdata = GetExpandedEnvPath("LOCALAPPDATA")) {
        return *localappdata / "Crimson" / "Cache";
    }
#endif
    if (auto home = GetHomeDirectory()) {
        return *home / ".cache" / "crimson";
    }
    return std::nullopt;
}

std::optional<fs::path> ResolveExecutablePathImpl(const fs::path& argv0_path) {
    if (argv0_path.empty()) {
        return std::nullopt;
    }

    auto try_candidate = [](const fs::path& candidate) -> std::optional<fs::path> {
        auto normalized = NormalizePath(candidate);
        if (!normalized) {
            return std::nullopt;
        }
        if (IsRegularFileNoThrow(*normalized)) {
            return normalized;
        }
        return std::nullopt;
    };

    if (argv0_path.is_absolute() || argv0_path.has_parent_path()) {
        if (auto resolved = try_candidate(argv0_path)) {
            return resolved;
        }
    }

    if (auto path_env = GetEnvValue("PATH"); path_env && !argv0_path.has_parent_path()) {
#ifdef _WIN32
        constexpr char kPathSeparator = ';';
#else
        constexpr char kPathSeparator = ':';
#endif
        std::stringstream path_stream(*path_env);
        std::string entry;
        while (std::getline(path_stream, entry, kPathSeparator)) {
            if (entry.empty()) {
                continue;
            }
            fs::path candidate = fs::path(entry) / argv0_path;
            if (auto resolved = try_candidate(candidate)) {
                return resolved;
            }
#ifdef _WIN32
            if (!candidate.has_extension()) {
                candidate += ".exe";
                if (auto resolved = try_candidate(candidate)) {
                    return resolved;
                }
            }
#endif
        }
    }

    return try_candidate(argv0_path);
}

std::optional<fs::path> GetExecutableDir(const fs::path& argv0_path) {
    auto executable_path = ResolveExecutablePathImpl(argv0_path);
    if (!executable_path) {
        return std::nullopt;
    }
    fs::path executable_dir = executable_path->parent_path();
    if (executable_dir.empty()) {
        return std::nullopt;
    }
    return executable_dir;
}

std::vector<fs::path> CollectCrimsonResourceRoots(const fs::path& current_working_dir,
                                                  const fs::path& argv0_path) {
    std::vector<fs::path> roots;

    if (auto data_dir = GetExpandedEnvPath("CRIMSON_DATA_DIR")) {
        AppendUniquePath(roots, *data_dir);
    }

    if (auto executable_dir = GetExecutableDir(argv0_path)) {
        AppendUniquePath(roots, *executable_dir);
        AppendUniquePath(roots, *executable_dir / "share" / "crimson");
        fs::path install_root = executable_dir->parent_path();
        if (!install_root.empty()) {
            AppendUniquePath(roots, install_root);
            AppendUniquePath(roots, install_root / "share" / "crimson");
        }
    }

    if (!current_working_dir.empty()) {
        AppendUniquePath(roots, current_working_dir);
        AppendUniquePath(roots, current_working_dir / "share" / "crimson");
    }

    return roots;
}

fs::path GetPreferredDefaultStartPath(const fs::path& current_working_dir) {
    if (auto configured_start_path = GetExpandedEnvPath("CRIMSON_DEFAULT_START_PATH")) {
        if (IsDirectoryNoThrow(*configured_start_path)) {
            return *configured_start_path;
        }
    }

    if (auto home = GetHomeDirectory()) {
        if (IsDirectoryNoThrow(*home)) {
            return *home;
        }
    }

    if (!current_working_dir.empty() && IsDirectoryNoThrow(current_working_dir)) {
        return current_working_dir;
    }

    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    if (!ec && !cwd.empty()) {
        return cwd;
    }

    return ".";
}

std::string TrimCopy(const std::string& value) {
    auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
                    return std::isspace(ch) != 0;
                }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

bool ReplaceFileAtomically(const fs::path& source,
                           const fs::path& destination,
                           std::string& error_message) {
#ifdef _WIN32
    if (MoveFileExW(source.c_str(),
                    destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    error_message = "Could not replace " + destination.string() +
                    " (Windows error " + std::to_string(GetLastError()) + ")";
    return false;
#else
    std::error_code ec;
    fs::rename(source, destination, ec);
    if (!ec) {
        return true;
    }
    error_message = "Could not replace " + destination.string() + ": " +
                    ec.message();
    return false;
#endif
}

std::optional<UiPathConfig> LoadUiPathConfigFile(const fs::path& config_path,
                                                 const fs::path& fallback_start_path) {
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

    if (!config.default_start_path.empty() &&
        !IsDirectoryNoThrow(config.default_start_path)) {
        config.default_start_path.clear();
    }
    if (config.default_start_path.empty() && !config.preferred_roots.empty()) {
        config.default_start_path = config.preferred_roots.front();
    }
    if (config.default_start_path.empty()) {
        config.default_start_path = fallback_start_path.string();
    }
    if (config.preferred_roots.empty() && !config.default_start_path.empty()) {
        config.preferred_roots.push_back(config.default_start_path);
    }

    config.loaded_from = config_path.string();
    return config;
}

}  // namespace

std::string ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool IsRegularFileNoThrow(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

bool IsDirectoryNoThrow(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec);
}

std::string ExpandUserPath(const std::string& path) {
    if (path.empty() || path[0] != '~') {
        return path;
    }

    auto home_dir = GetHomeDirectory();
    if (!home_dir) {
        return path;
    }
    const std::string home = home_dir->string();

    if (path.size() == 1) {
        return home;
    }
    if (path[1] == '/') {
        return home + path.substr(1);
    }

    // Unsupported "~user" expansion; leave unchanged.
    return path;
}

std::optional<fs::path> ResolveExecutablePath(const fs::path& argv0_path) {
    return ResolveExecutablePathImpl(argv0_path);
}

std::optional<fs::path> ResolveCrimsonResourcePath(
    const fs::path& current_working_dir,
    const fs::path& argv0_path,
    const fs::path& relative_path) {
    for (const auto& root : CollectCrimsonResourceRoots(current_working_dir, argv0_path)) {
        fs::path candidate = root / relative_path;
        if (IsRegularFileNoThrow(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

fs::path GetDefaultCrimsonBufferDumpRoot() {
    if (auto cache_dir = GetCrimsonUserCacheDir()) {
        return *cache_dir / "buffer_dumps";
    }

    std::error_code ec;
    fs::path temp_dir = fs::temp_directory_path(ec);
    if (!ec && !temp_dir.empty()) {
        return temp_dir / "crimson_buffer_dumps";
    }

    fs::path cwd = fs::current_path(ec);
    if (!ec && !cwd.empty()) {
        return cwd / ".crimson" / "buffer_dumps";
    }

    return ".crimson/buffer_dumps";
}

std::optional<fs::path> GetCrimsonUserUiPathConfigPath() {
    auto config_dir = GetCrimsonUserConfigDir();
    if (!config_dir) {
        return std::nullopt;
    }
    return *config_dir / "ui_paths.json";
}

bool NormalizeUiPathConfig(const UiPathConfig& input,
                           UiPathConfig& normalized,
                           std::string& error_message) {
    normalized = UiPathConfig{};
    normalized.loaded_from = input.loaded_from;
    error_message.clear();

    const std::string default_value = TrimCopy(input.default_start_path);
    if (default_value.empty()) {
        error_message = "Default start path cannot be empty.";
        return false;
    }

    auto normalized_default = NormalizePath(ExpandUserPath(default_value));
    if (!normalized_default || !IsDirectoryNoThrow(*normalized_default)) {
        error_message = "Default start path is not an existing directory: " +
                        default_value;
        return false;
    }
    normalized.default_start_path = normalized_default->string();

    for (const auto& candidate_value : input.preferred_roots) {
        const std::string trimmed_value = TrimCopy(candidate_value);
        if (trimmed_value.empty()) {
            continue;
        }
        auto normalized_candidate =
            NormalizePath(ExpandUserPath(trimmed_value));
        if (!normalized_candidate ||
            !IsDirectoryNoThrow(*normalized_candidate)) {
            error_message = "Preferred root is not an existing directory: " +
                            trimmed_value;
            return false;
        }
        const std::string root = normalized_candidate->string();
        if (std::find(normalized.preferred_roots.begin(),
                      normalized.preferred_roots.end(),
                      root) == normalized.preferred_roots.end()) {
            normalized.preferred_roots.push_back(root);
        }
    }

    if (normalized.preferred_roots.empty()) {
        normalized.preferred_roots.push_back(normalized.default_start_path);
    }
    return true;
}

bool SaveUserUiPathConfig(const UiPathConfig& config,
                          fs::path& saved_path,
                          std::string& error_message) {
    saved_path.clear();
    error_message.clear();

    UiPathConfig normalized;
    if (!NormalizeUiPathConfig(config, normalized, error_message)) {
        return false;
    }

    auto config_path = GetCrimsonUserUiPathConfigPath();
    if (!config_path) {
        error_message = "Could not determine the user configuration directory.";
        return false;
    }

    std::error_code ec;
    fs::create_directories(config_path->parent_path(), ec);
    if (ec) {
        error_message = "Could not create " +
                        config_path->parent_path().string() + ": " +
                        ec.message();
        return false;
    }

    std::random_device random_device;
    const fs::path temporary_path =
        config_path->parent_path() /
        (config_path->filename().string() + ".tmp." +
         std::to_string(std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count()) +
         "." + std::to_string(random_device()));

    json payload = {
        {"default_start_path", normalized.default_start_path},
        {"preferred_roots", normalized.preferred_roots},
    };
    {
        std::ofstream output(temporary_path, std::ios::out | std::ios::trunc);
        if (!output) {
            error_message = "Could not write temporary configuration file: " +
                            temporary_path.string();
            return false;
        }
        output << payload.dump(2) << '\n';
        output.flush();
        if (!output) {
            error_message = "Failed while writing temporary configuration file: " +
                            temporary_path.string();
            output.close();
            fs::remove(temporary_path, ec);
            return false;
        }
    }

    if (!ReplaceFileAtomically(temporary_path, *config_path, error_message)) {
        fs::remove(temporary_path, ec);
        return false;
    }

    saved_path = *config_path;
    return true;
}

UiPathConfig LoadUiPathConfig(const fs::path& current_working_dir,
                              const fs::path& argv0_path) {
    const fs::path fallback_start_path = GetPreferredDefaultStartPath(current_working_dir);
    std::vector<fs::path> candidates;

    if (const char* env_config = std::getenv("CRIMSON_UI_PATHS_CONFIG")) {
        if (*env_config != '\0') {
            candidates.push_back(fs::path(ExpandUserPath(env_config)));
        }
    }

    // GUI-saved preferences are user scoped and take priority over packaged
    // or checkout defaults. CRIMSON_UI_PATHS_CONFIG remains the explicit
    // highest-priority override.
    if (auto user_config_path = GetCrimsonUserUiPathConfigPath()) {
        candidates.push_back(*user_config_path);
    }

    if (auto data_dir = GetExpandedEnvPath("CRIMSON_DATA_DIR")) {
        candidates.push_back(*data_dir / "config" / "ui_paths.json");
    }

    candidates.push_back(current_working_dir / "config" / "ui_paths.json");
    candidates.push_back(current_working_dir / "ui_paths.json");

    if (auto executable_dir = GetExecutableDir(argv0_path)) {
        candidates.push_back(*executable_dir / "config" / "ui_paths.json");
        candidates.push_back(*executable_dir / "share" / "crimson" / "config" /
                             "ui_paths.json");
        fs::path repo_like_root = executable_dir->parent_path();
        if (!repo_like_root.empty()) {
            candidates.push_back(repo_like_root / "config" / "ui_paths.json");
        }
    }

    if (auto executable_dir = GetExecutableDir(argv0_path)) {
        fs::path install_root = executable_dir->parent_path();
        if (!install_root.empty()) {
            candidates.push_back(install_root / "share" / "crimson" / "config" /
                                 "ui_paths.json");
        }
    }

    for (const auto& candidate : candidates) {
        if (auto config = LoadUiPathConfigFile(candidate, fallback_start_path)) {
            return *config;
        }
    }

    UiPathConfig fallback;
    fallback.default_start_path = fallback_start_path.string();
    if (!fallback.default_start_path.empty()) {
        fallback.preferred_roots = {fallback.default_start_path};
    }
    return fallback;
}

bool IsSupportedVideoPath(const fs::path& path) {
    std::string ext = ToLowerCopy(path.extension().string());
    return ext == ".mp4" || ext == ".mov" || ext == ".mkv" || ext == ".avi";
}

std::optional<fs::path> ResolveAffiliatedVideoPath(
    const std::string& source_path_hint,
    const std::string& archive_path) {
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

        if (hint.is_absolute()) {
            if (auto relocated =
                    RelocateStoredPathToRecordingRoot(hint, recording_root)) {
                candidates.push_back(*relocated);
            }
        } else {
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

static std::optional<fs::path> TryStimulusHintCandidates(
    const fs::path& hint,
    const std::string& archive_path,
    const std::string& recording_root) {
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

std::optional<fs::path> ResolveStimulusVideoPath(
    const std::string& stimulus_video_hint,
    const std::string& source_h5_hint,
    const std::string& archive_path,
    const std::string& recording_root) {
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

fs::path InferRecordingRootPath(
    const fs::path& video_path,
    const std::string& archive_path) {
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
