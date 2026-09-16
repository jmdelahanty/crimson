#include "windows_long_path_support.h"

#include <algorithm>
#include <iostream>
#include <mutex>
#include <optional>
#include <unordered_set>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace crimson::windows_path {
namespace {

std::string normalizeSeparators(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

#if defined(_WIN32)
std::optional<std::string> fileRootFromKvstoreSpec(
    const nlohmann::json& spec) {
    if (!spec.is_object()) {
        return std::nullopt;
    }
    if (spec.value("driver", std::string{}) == "file" &&
        spec.contains("path") && spec["path"].is_string()) {
        return spec["path"].get<std::string>();
    }
    if (spec.contains("kvstore")) {
        return fileRootFromKvstoreSpec(spec["kvstore"]);
    }
    return std::nullopt;
}

enum class LongPathPolicyState {
    Enabled,
    Disabled,
    Unavailable,
};

LongPathPolicyState queryLongPathPolicy() {
    DWORD value = 0;
    DWORD value_size = sizeof(value);
    const LSTATUS status = RegGetValueW(
        HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\FileSystem",
        L"LongPathsEnabled",
        RRF_RT_REG_DWORD,
        nullptr,
        &value,
        &value_size);
    if (status != ERROR_SUCCESS) {
        return LongPathPolicyState::Unavailable;
    }
    return value == 1 ? LongPathPolicyState::Enabled
                      : LongPathPolicyState::Disabled;
}

const char* policyLabel(LongPathPolicyState state) {
    switch (state) {
        case LongPathPolicyState::Enabled:
            return "enabled";
        case LongPathPolicyState::Disabled:
            return "disabled";
        case LongPathPolicyState::Unavailable:
            return "unavailable";
    }
    return "unavailable";
}

bool manifestDeclaresLongPathAwareness() {
#if defined(CRIMSON_WINDOWS_LONG_PATH_AWARE)
    return true;
#else
    return false;
#endif
}
#endif

}  // namespace

std::string composePhysicalPath(const std::string& root,
                                const std::string& relative_path) {
    std::string normalized_root = normalizeSeparators(root);
    std::string normalized_relative = normalizeSeparators(relative_path);
    while (!normalized_root.empty() && normalized_root.back() == '/') {
        normalized_root.pop_back();
    }
    while (!normalized_relative.empty() && normalized_relative.front() == '/') {
        normalized_relative.erase(normalized_relative.begin());
    }
    if (normalized_root.empty()) {
        return normalized_relative;
    }
    if (normalized_relative.empty()) {
        return normalized_root;
    }
    return normalized_root + "/" + normalized_relative;
}

bool exceedsLegacyLimit(const std::string& physical_path) {
    return normalizeSeparators(physical_path).size() > kLegacyMaxUsablePath;
}

void logConfigurationForZarrRoot(const std::string& root_path) {
#if defined(_WIN32)
    const std::string normalized_root = normalizeSeparators(root_path);
    const bool manifest_enabled = manifestDeclaresLongPathAwareness();
    const LongPathPolicyState policy = queryLongPathPolicy();
    std::cout << "  [WindowsPath] root_chars=" << normalized_root.size()
              << " longPathAware_manifest="
              << (manifest_enabled ? "enabled" : "disabled")
              << " LongPathsEnabled=" << policyLabel(policy) << std::endl;
    if (!manifest_enabled || policy != LongPathPolicyState::Enabled) {
        std::cout
            << "  [WINDOWS_LONG_PATH_WARNING] Deep Zarr paths longer than "
            << kLegacyMaxUsablePath
            << " characters may be reported as missing. Crimson needs both "
               "an embedded longPathAware manifest and Windows "
               "LongPathsEnabled=1; a shorter drive mapping remains a safe "
               "workaround."
            << std::endl;
    }
#else
    (void)root_path;
#endif
}

void noteZarrPathAccess(const nlohmann::json& kvstore_spec,
                        const std::string& relative_path) {
#if defined(_WIN32)
    const auto root = fileRootFromKvstoreSpec(kvstore_spec);
    if (!root.has_value()) {
        return;
    }
    const std::string physical_path =
        composePhysicalPath(*root, relative_path);
    if (!exceedsLegacyLimit(physical_path)) {
        return;
    }

    static std::mutex warned_roots_mutex;
    static std::unordered_set<std::string> warned_roots;
    {
        std::lock_guard<std::mutex> lock(warned_roots_mutex);
        if (!warned_roots.insert(normalizeSeparators(*root)).second) {
            return;
        }
    }

    const bool manifest_enabled = manifestDeclaresLongPathAwareness();
    const LongPathPolicyState policy = queryLongPathPolicy();
    std::cout
        << "  [WINDOWS_LONG_PATH] physical_path_chars="
        << physical_path.size() << " exceeds legacy usable limit "
        << kLegacyMaxUsablePath << ": " << physical_path << std::endl;
    std::cout
        << "  [WINDOWS_LONG_PATH] support="
        << ((manifest_enabled && policy == LongPathPolicyState::Enabled)
                ? "enabled"
                : "incomplete")
        << " (longPathAware_manifest="
        << (manifest_enabled ? "enabled" : "disabled")
        << ", LongPathsEnabled=" << policyLabel(policy) << ")."
        << std::endl;
#else
    (void)kvstore_spec;
    (void)relative_path;
#endif
}

}  // namespace crimson::windows_path
