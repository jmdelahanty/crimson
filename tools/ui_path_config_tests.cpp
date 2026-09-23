#include "ui_path_config.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

void setEnvironmentValue(const std::string& name,
                         const std::optional<std::string>& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value ? value->c_str() : "");
#else
    if (value) {
        setenv(name.c_str(), value->c_str(), 1);
    } else {
        unsetenv(name.c_str());
    }
#endif
}

class ScopedEnvironmentValue {
public:
    ScopedEnvironmentValue(std::string name,
                           std::optional<std::string> value)
        : name_(std::move(name)) {
        if (const char* original = std::getenv(name_.c_str())) {
            original_ = std::string(original);
        }
        setEnvironmentValue(name_, value);
    }

    ~ScopedEnvironmentValue() {
        setEnvironmentValue(name_, original_);
    }

private:
    std::string name_;
    std::optional<std::string> original_;
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void writeConfig(const fs::path& path,
                 const fs::path& default_path,
                 const std::vector<fs::path>& roots) {
    fs::create_directories(path.parent_path());
    std::vector<std::string> root_strings;
    root_strings.reserve(roots.size());
    for (const auto& root : roots) {
        root_strings.push_back(root.string());
    }
    const json payload = {
        {"default_start_path", default_path.string()},
        {"preferred_roots", root_strings},
    };
    std::ofstream output(path);
    output << payload.dump(2) << '\n';
    require(static_cast<bool>(output), "failed to write test configuration");
}

}  // namespace

int main() {
    const fs::path test_root =
        fs::weakly_canonical(fs::temp_directory_path()) /
        ("crimson-ui-path-config-tests-" +
         std::to_string(std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count()));

    try {
        const fs::path checkout_root = test_root / "checkout";
        const fs::path packaged_path = test_root / "packaged-root";
        const fs::path user_path = test_root / "user-root";
        const fs::path second_user_path = test_root / "second-user-root";
        const fs::path xdg_config_home = test_root / "xdg";
        fs::create_directories(checkout_root / "config");
        fs::create_directories(packaged_path);
        fs::create_directories(user_path);
        fs::create_directories(second_user_path);

        UiPathConfig editable;
        editable.default_start_path = "  " + user_path.string() + "  ";
        editable.preferred_roots = {
            user_path.string(),
            "",
            user_path.string(),
            second_user_path.string(),
        };
        editable.loaded_from = "packaged-default";

        UiPathConfig normalized;
        std::string error_message;
        require(NormalizeUiPathConfig(editable, normalized, error_message),
                "valid editable config was rejected: " + error_message);
        require(normalized.default_start_path == user_path.string(),
                "default path was not trimmed and normalized");
        require(normalized.preferred_roots.size() == 2,
                "empty and duplicate preferred roots were not removed");
        require(normalized.loaded_from == editable.loaded_from,
                "normalization did not preserve config provenance");

        UiPathConfig invalid = editable;
        invalid.preferred_roots.push_back(
            (test_root / "missing-directory").string());
        require(!NormalizeUiPathConfig(invalid, normalized, error_message),
                "nonexistent preferred root was accepted");
        require(error_message.find("not an existing directory") !=
                    std::string::npos,
                "invalid-root error did not explain the problem");

        ScopedEnvironmentValue xdg_guard("XDG_CONFIG_HOME",
                                         xdg_config_home.string());
        ScopedEnvironmentValue explicit_guard("CRIMSON_UI_PATHS_CONFIG",
                                              std::nullopt);
        ScopedEnvironmentValue data_guard("CRIMSON_DATA_DIR", std::nullopt);

        writeConfig(checkout_root / "config" / "ui_paths.json",
                    packaged_path,
                    {packaged_path});

        UiPathConfig user_config;
        user_config.default_start_path = user_path.string();
        user_config.preferred_roots = {
            user_path.string(), second_user_path.string()};
        fs::path saved_path;
        require(SaveUserUiPathConfig(user_config,
                                     saved_path,
                                     error_message),
                "failed to save user config: " + error_message);
        require(saved_path == xdg_config_home / "crimson" / "ui_paths.json",
                "user config was saved to the wrong path");
        require(fs::is_regular_file(saved_path),
                "saved user config does not exist");

        UiPathConfig loaded = LoadUiPathConfig(
            checkout_root, test_root / "missing" / "redgui");
        require(loaded.default_start_path == user_path.string(),
                "user config did not take priority over checkout defaults");
        require(loaded.loaded_from == saved_path.string(),
                "loader did not report the user config source");

        const fs::path explicit_path = test_root / "explicit-ui-paths.json";
        writeConfig(explicit_path, packaged_path, {packaged_path});
        setEnvironmentValue("CRIMSON_UI_PATHS_CONFIG",
                            explicit_path.string());
        loaded = LoadUiPathConfig(checkout_root,
                                  test_root / "missing" / "redgui");
        require(loaded.default_start_path == packaged_path.string(),
                "explicit config did not override user config");
        require(loaded.loaded_from == explicit_path.string(),
                "loader did not report the explicit config source");

        const fs::path recording_root =
            test_root / "mounted-share" / "recording-identity";
        const fs::path archive_path =
            recording_root / "zarr" / "analysis.zarr";
        const fs::path relocated_video = recording_root / "cams" / "main.mp4";
        fs::create_directories(archive_path);
        fs::create_directories(relocated_video.parent_path());
        {
            std::ofstream video(relocated_video);
            video << "fixture";
            require(static_cast<bool>(video),
                    "failed to write relocated video fixture");
        }
        const auto resolved_video = ResolveAffiliatedVideoPath(
            "/groups/lab/recording-identity/cams/main.mp4",
            archive_path.string());
        require(resolved_video.has_value(),
                "relocated absolute affiliated video was not resolved");
        require(*resolved_video == relocated_video,
                "relocated affiliated video resolved to the wrong path");

        const auto unrelated_video = ResolveAffiliatedVideoPath(
            "/groups/lab/different-recording/cams/main.mp4",
            archive_path.string());
        require(!unrelated_video.has_value(),
                "absolute video from a different recording was remapped");

        fs::remove_all(test_root);
        std::cout << "ui_path_config_tests: PASS" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::error_code ec;
        fs::remove_all(test_root, ec);
        std::cerr << "ui_path_config_tests: FAIL: " << error.what()
                  << std::endl;
        return 1;
    }
}
