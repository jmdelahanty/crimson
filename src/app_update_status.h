#pragma once

#include <filesystem>
#include <string>

namespace crimson::app {

struct AppUpdateStatus {
    bool install_metadata_found = false;
    bool latest_manifest_found = false;
    bool comparison_available = false;
    bool update_available = false;
    std::string install_root;
    std::string installed_release_name;
    std::string latest_release_name;
    std::string latest_manifest_path;
    std::string current_root;
    std::string status_detail;
};

// Read-only status for a resolved installed app root; never launches an updater.
AppUpdateStatus loadAppUpdateStatus(const std::filesystem::path& install_root);

}  // namespace crimson::app
