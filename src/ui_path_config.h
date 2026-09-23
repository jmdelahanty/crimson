#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct UiPathConfig {
    std::string default_start_path;
    std::vector<std::string> preferred_roots;
    std::string loaded_from;
};

inline bool IsFiniteFloat(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

std::string ToLowerCopy(std::string value);
bool IsRegularFileNoThrow(const std::filesystem::path& path);
bool IsDirectoryNoThrow(const std::filesystem::path& path);
std::string ExpandUserPath(const std::string& path);
std::optional<std::filesystem::path> ResolveExecutablePath(
    const std::filesystem::path& argv0_path);
std::optional<std::filesystem::path> ResolveCrimsonResourcePath(
    const std::filesystem::path& current_working_dir,
    const std::filesystem::path& argv0_path,
    const std::filesystem::path& relative_path);
std::filesystem::path GetDefaultCrimsonBufferDumpRoot();

std::optional<std::filesystem::path> GetCrimsonUserUiPathConfigPath();

bool NormalizeUiPathConfig(const UiPathConfig& input,
                           UiPathConfig& normalized,
                           std::string& error_message);

bool SaveUserUiPathConfig(const UiPathConfig& config,
                          std::filesystem::path& saved_path,
                          std::string& error_message);

UiPathConfig LoadUiPathConfig(const std::filesystem::path& current_working_dir,
                              const std::filesystem::path& argv0_path);

bool IsSupportedVideoPath(const std::filesystem::path& path);

std::optional<std::filesystem::path> ResolveAffiliatedVideoPath(
    const std::string& source_path_hint,
    const std::string& archive_path);

std::optional<std::filesystem::path> ResolveStimulusVideoPath(
    const std::string& stimulus_video_hint,
    const std::string& source_h5_hint,
    const std::string& archive_path,
    const std::string& recording_root);

std::filesystem::path InferRecordingRootPath(
    const std::filesystem::path& video_path,
    const std::string& archive_path);
