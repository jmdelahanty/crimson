#include "app_update_status.h"

#include <fstream>
#include <optional>
#include <nlohmann/json.hpp>

namespace crimson::app {
namespace {
using json = nlohmann::json;

std::optional<json> readJsonFileNoThrow(const std::filesystem::path& path) {
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(path, ec);
    if (path.empty() || ec || bytes > 1024 * 1024) {
        return std::nullopt;
    }

    std::ifstream stream(path);
    if (!stream.is_open()) {
        return std::nullopt;
    }

    try {
        json payload;
        stream >> payload;
        if (!payload.is_object()) return std::nullopt;
        return payload;
    } catch (...) {
        return std::nullopt;
    }
}

std::string jsonStringOrEmpty(const json& payload, const char* key) {
    auto it = payload.find(key);
    if (it == payload.end() || !it->is_string()) {
        return "";
    }
    return it->get<std::string>();
}

}  // namespace

AppUpdateStatus loadAppUpdateStatus(const std::filesystem::path& install_root) {
    AppUpdateStatus status;

    if (install_root.empty()) {
        return status;
    }

    status.install_root = install_root.string();

    const auto install_metadata_path = install_root / "install_metadata.json";
    auto install_metadata = readJsonFileNoThrow(install_metadata_path);
    if (!install_metadata) {
        status.status_detail =
            "No install_metadata.json found; update checks are unavailable.";
        return status;
    }

    status.install_metadata_found = true;
    status.installed_release_name =
        jsonStringOrEmpty(*install_metadata, "installed_release_name");
    status.latest_manifest_path =
        jsonStringOrEmpty(*install_metadata, "latest_manifest_path");
    status.current_root = jsonStringOrEmpty(*install_metadata, "current_root");

    if (status.installed_release_name.empty()) {
        const auto release_metadata_path = install_root / "release.json";
        if (auto release_metadata = readJsonFileNoThrow(release_metadata_path)) {
            status.installed_release_name =
                jsonStringOrEmpty(*release_metadata, "release_name");
        }
    }

    if (status.latest_manifest_path.empty()) {
        status.status_detail =
            "Install metadata does not specify latest.json; update checks are unavailable.";
        return status;
    }

    auto latest_manifest =
        readJsonFileNoThrow(std::filesystem::path(status.latest_manifest_path));
    if (!latest_manifest) {
        status.status_detail =
            "Could not read latest.json from the configured share path.";
        return status;
    }

    status.latest_manifest_found = true;
    status.latest_release_name =
        jsonStringOrEmpty(*latest_manifest, "release_name");

    if (status.installed_release_name.empty() ||
        status.latest_release_name.empty()) {
        status.status_detail =
            "Release metadata is incomplete; update status is unavailable.";
        return status;
    }

    status.comparison_available = true;
    status.update_available =
        status.installed_release_name != status.latest_release_name;
    status.status_detail = status.update_available
                               ? "A different Crimson app drop is published."
                               : "Crimson is up to date.";
    return status;
}

}  // namespace crimson::app
