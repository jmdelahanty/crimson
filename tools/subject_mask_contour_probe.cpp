#include "zarr_loader.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace {

struct Options {
    std::string archive_path;
    std::string run_name;
    std::string storage = "dense";
    std::string expected_representation;
    std::optional<size_t> expected_components;
    std::optional<size_t> frame;
    bool require_row_contours = false;
    int timeout_seconds = 60;
};

bool parseSize(const std::string& text, size_t* value) {
    try {
        size_t consumed = 0;
        const auto parsed = std::stoull(text, &consumed);
        if (consumed != text.size()) {
            return false;
        }
        *value = static_cast<size_t>(parsed);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool consumeValue(int argc,
                  char** argv,
                  int* index,
                  std::string* value) {
    if (*index + 1 >= argc) {
        return false;
    }
    ++(*index);
    *value = argv[*index];
    return true;
}

bool parseOptions(int argc, char** argv, Options* options) {
    if (argc < 2) {
        return false;
    }
    options->archive_path = argv[1];
    for (int index = 2; index < argc; ++index) {
        const std::string arg = argv[index];
        std::string value;
        if (arg == "--run") {
            if (!consumeValue(argc, argv, &index, &value)) {
                return false;
            }
            options->run_name = value;
        } else if (arg == "--storage") {
            if (!consumeValue(argc, argv, &index, &value)) {
                return false;
            }
            options->storage = value;
        } else if (arg == "--expect-representation") {
            if (!consumeValue(argc, argv, &index, &value) ||
                (value != "sampled" && value != "ragged")) {
                return false;
            }
            options->expected_representation = value;
        } else if (arg == "--expect-components") {
            size_t parsed = 0;
            if (!consumeValue(argc, argv, &index, &value) ||
                !parseSize(value, &parsed)) {
                return false;
            }
            options->expected_components = parsed;
        } else if (arg == "--frame") {
            size_t parsed = 0;
            if (!consumeValue(argc, argv, &index, &value) ||
                !parseSize(value, &parsed)) {
                return false;
            }
            options->frame = parsed;
        } else if (arg == "--require-row-contours") {
            options->require_row_contours = true;
        } else if (arg == "--timeout-seconds") {
            size_t parsed = 0;
            if (!consumeValue(argc, argv, &index, &value) ||
                !parseSize(value, &parsed) || parsed == 0 || parsed > 3600) {
                return false;
            }
            options->timeout_seconds = static_cast<int>(parsed);
        } else {
            return false;
        }
    }
    return !options->run_name.empty();
}

void printUsage(const char* executable) {
    std::cerr
        << "Usage: " << executable
        << " <analysis.zarr> --run NAME [--storage dense|auto]"
           " [--expect-representation sampled|ragged]"
           " [--expect-components N] [--frame N]"
           " [--require-row-contours] [--timeout-seconds N]"
        << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, &options)) {
        printUsage(argv[0]);
        return 1;
    }

    ZarrDetectionLoader loader;
    loader.setRequestedRefinedSubjectMaskRunName(options.run_name);
    loader.setRequestedRefinedSubjectMaskStorage(options.storage);
    std::string error_message;
    if (!loader.loadZarrFile(options.archive_path, error_message)) {
        std::cerr << "load failed: " << error_message << std::endl;
        return 2;
    }
    if (!loader.eyeMasksUseRefinedSubjectMasks() || !loader.hasEyeMasks()) {
        std::cerr << "requested refined subject-mask run did not load"
                  << std::endl;
        return 3;
    }

    loader.requestRefinedSubjectMaskOptionalOverlayPrefetch();
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(options.timeout_seconds);
    std::string overlay_status;
    do {
        overlay_status = loader.getRefinedSubjectMaskOptionalOverlayStatus();
        if (overlay_status == "optional overlays ready") {
            break;
        }
        if (overlay_status.rfind("optional overlays failed", 0) == 0) {
            std::cerr << overlay_status << std::endl;
            return 4;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    } while (std::chrono::steady_clock::now() < deadline);
    if (overlay_status != "optional overlays ready") {
        std::cerr << "optional overlay load timed out: " << overlay_status
                  << std::endl;
        return 5;
    }

    const auto& components = loader.getRefinedSubjectMaskOverlayComponents();
    const size_t available = static_cast<size_t>(std::count_if(
        components.begin(),
        components.end(),
        [](const auto& component) { return component.contours_available; }));
    const size_t sampled = static_cast<size_t>(std::count_if(
        components.begin(),
        components.end(),
        [](const auto& component) {
            return component.contours_available &&
                   component.sampled_contours_used;
        }));
    const size_t ragged = available - sampled;

    std::cout << "run=" << loader.getEyeMaskRunName() << std::endl;
    std::cout << "contour_components=" << available << std::endl;
    std::cout << "sampled_components=" << sampled << std::endl;
    std::cout << "ragged_components=" << ragged << std::endl;
    for (const auto& component : components) {
        const char* representation = !component.contours_available
            ? "none"
            : (component.sampled_contours_used ? "sampled" : "ragged");
        std::cout << "component=" << component.label
                  << " representation=" << representation
                  << " available="
                  << (component.contours_available ? "true" : "false")
                  << " points_per_row="
                  << component.sampled_contour_point_count << std::endl;
    }

    if (options.expected_components.has_value() &&
        available != *options.expected_components) {
        std::cerr << "expected " << *options.expected_components
                  << " contour components but loaded " << available
                  << std::endl;
        return 6;
    }
    if (options.expected_representation == "sampled" &&
        (available == 0 || sampled != available)) {
        std::cerr << "expected all available contours to use sampled arrays"
                  << std::endl;
        return 7;
    }
    if (options.expected_representation == "ragged" &&
        (available == 0 || ragged != available)) {
        std::cerr << "expected all available contours to use ragged arrays"
                  << std::endl;
        return 8;
    }

    if (options.frame.has_value()) {
        const auto detections = loader.getRawDetections(
            *options.frame,
            false,
            true,
            false,
            true,
            true);
        size_t rows_with_contours = 0;
        size_t contour_points = 0;
        for (const auto& mask : detections.eye_masks) {
            bool row_has_contour = false;
            for (const auto& component : mask.subject_mask_components) {
                if (component.has_contour) {
                    row_has_contour = true;
                    contour_points += component.contour_xy.size();
                }
            }
            if (row_has_contour) {
                ++rows_with_contours;
            }
        }
        std::cout << "frame=" << *options.frame << std::endl;
        std::cout << "rows_with_contours=" << rows_with_contours << std::endl;
        std::cout << "contour_points=" << contour_points << std::endl;
        if (options.require_row_contours && rows_with_contours == 0) {
            std::cerr << "requested frame produced no component contours"
                      << std::endl;
            return 9;
        }
    }
    return 0;
}
