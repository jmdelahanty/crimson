#include "app_update_status.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using crimson::app::loadAppUpdateStatus;
using json = nlohmann::json;

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

struct Fixture {
    fs::path root = fs::temp_directory_path() /
        ("crimson-app-update-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { require(fs::create_directory(root), "unique fixture directory"); }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
    void put(const char* name, const json& value) {
        std::ofstream output(root / name);
        output << value.dump();
        require(output.good(), "write fixture");
    }
};
}

int main() {
    try {
        Fixture f;
        require(!loadAppUpdateStatus({}).install_metadata_found, "source checkout");
        require(!loadAppUpdateStatus(f.root).install_metadata_found, "missing metadata");
        f.put("install_metadata.json", json::array());
        require(!loadAppUpdateStatus(f.root).install_metadata_found, "non-object metadata");
        f.put("install_metadata.json", {{"installed_release_name", "installed"}});
        auto status = loadAppUpdateStatus(f.root);
        require(status.install_metadata_found && !status.comparison_available,
                "missing manifest is unavailable, not up to date");
        f.put("install_metadata.json", {
            {"installed_release_name", "installed"},
            {"latest_manifest_path", (f.root / "latest.json").string()},
            {"current_root", (f.root / "current").string()}});
        status = loadAppUpdateStatus(f.root);
        require(!status.latest_manifest_found && !status.update_available, "offline share");
        f.put("latest.json", {{"release_name", "installed"}});
        status = loadAppUpdateStatus(f.root);
        require(status.comparison_available && !status.update_available, "same release");
        f.put("latest.json", {{"release_name", "published"}});
        status = loadAppUpdateStatus(f.root);
        require(status.comparison_available && status.update_available, "different release");
        require(status.current_root == (f.root / "current").string(), "installer location");
        f.put("latest.json", {{"release_name", 42}});
        status = loadAppUpdateStatus(f.root);
        require(status.latest_manifest_found && !status.comparison_available &&
                    !status.update_available, "malformed release is not up to date");
        f.put("install_metadata.json", {
            {"latest_manifest_path", (f.root / "latest.json").string()}});
        f.put("release.json", {{"release_name", "fallback"}});
        f.put("latest.json", {{"release_name", "fallback"}});
        status = loadAppUpdateStatus(f.root);
        require(status.comparison_available && !status.update_available &&
                status.installed_release_name == "fallback", "release fallback");
        { std::ofstream out(f.root / "latest.json"); out << "{broken"; }
        require(!loadAppUpdateStatus(f.root).latest_manifest_found, "invalid JSON");
        { std::ofstream out(f.root / "latest.json"); out << std::string(1024 * 1024 + 1, ' '); }
        require(!loadAppUpdateStatus(f.root).latest_manifest_found, "bounded metadata");
        std::cout << "App update status tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
