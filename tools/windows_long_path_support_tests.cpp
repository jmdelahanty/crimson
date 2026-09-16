#include "windows_long_path_support.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    try {
        using crimson::windows_path::composePhysicalPath;
        using crimson::windows_path::exceedsLegacyLimit;

        require(composePhysicalPath("E:\\data\\archive.zarr\\",
                                    "/analysis/run/zarr.json") ==
                    "E:/data/archive.zarr/analysis/run/zarr.json",
                "path composition did not normalize separators");
        require(!exceedsLegacyLimit(std::string(259, 'a')),
                "259-character path should fit the legacy limit");
        require(exceedsLegacyLimit(std::string(260, 'a')),
                "260-character path should exceed the legacy limit");

        const std::string root =
            "E:/jeremy/recordings/"
            "sleepyfish_2026_05_05_17_45_30_cam2010095/zarr/"
            "sleepyfish_2026_05_05_17_45_30_cam2010095_analysis.zarr";
        const std::string relative =
            "analysis/subject_shape_runs/"
            "subject_shape_sleepyfish_core_canary_20260713_01/components/"
            "subject_body/bspline_failure_reason_bytes/zarr.json";
        const std::string reported_path = composePhysicalPath(root, relative);
        require(reported_path.size() == 263,
                "sleepyfish regression path should be 263 characters");
        require(exceedsLegacyLimit(reported_path),
                "sleepyfish regression path should exceed MAX_PATH");

        std::cout << "windows_long_path_support_tests: PASS" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "windows_long_path_support_tests: FAIL: "
                  << error.what() << std::endl;
        return 1;
    }
}
