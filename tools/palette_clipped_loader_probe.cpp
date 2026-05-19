#include "zarr_loader.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct ExpectedFrame {
    int64_t parent_frame = -1;
    std::string clip_id;
    int64_t clip_local_frame = -1;
    std::optional<int64_t> recording_frame_id;
    std::optional<size_t> min_boxes;
};

struct ProbeOptions {
    std::optional<std::string> expected_collection;
    std::optional<size_t> expected_selected_runs;
    std::optional<size_t> expected_mapped_frames;
    std::optional<size_t> expected_total_parent_frames;
    std::optional<size_t> expected_unselected_frame_pairs;
    std::optional<bool> expected_coordinates_normalized;
    std::vector<ExpectedFrame> expected_frames;
    std::vector<int64_t> print_frames;
};

std::vector<std::string> splitColon(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    std::istringstream stream(text);
    while (std::getline(stream, current, ':')) {
        parts.push_back(current);
    }
    return parts;
}

bool parseInt64(const std::string& text, int64_t* value) {
    if (value == nullptr) {
        return false;
    }
    try {
        size_t consumed = 0;
        const int64_t parsed = std::stoll(text, &consumed);
        if (consumed != text.size()) {
            return false;
        }
        *value = parsed;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool parseSize(const std::string& text, size_t* value) {
    if (value == nullptr) {
        return false;
    }
    try {
        size_t consumed = 0;
        const unsigned long long parsed = std::stoull(text, &consumed);
        if (consumed != text.size()) {
            return false;
        }
        *value = static_cast<size_t>(parsed);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool parseBool(const std::string& text, bool* value) {
    if (value == nullptr) {
        return false;
    }
    if (text == "true" || text == "1" || text == "yes") {
        *value = true;
        return true;
    }
    if (text == "false" || text == "0" || text == "no") {
        *value = false;
        return true;
    }
    return false;
}

bool parseExpectedFrame(const std::string& text,
                        ExpectedFrame* expected,
                        std::string* error_message) {
    if (expected == nullptr) {
        return false;
    }
    const auto parts = splitColon(text);
    if (parts.size() < 3 || parts.size() > 5) {
        if (error_message != nullptr) {
            *error_message =
                "expected frame must be parent:clip_id:clip_local"
                "[:recording_frame_id[:min_boxes]]";
        }
        return false;
    }
    int64_t parent = -1;
    int64_t clip_local = -1;
    if (!parseInt64(parts[0], &parent) ||
        !parseInt64(parts[2], &clip_local)) {
        if (error_message != nullptr) {
            *error_message =
                "expected frame parent and clip_local must be integers";
        }
        return false;
    }
    expected->parent_frame = parent;
    expected->clip_id = parts[1];
    expected->clip_local_frame = clip_local;
    if (parts.size() >= 4 && !parts[3].empty()) {
        int64_t recording = -1;
        if (!parseInt64(parts[3], &recording)) {
            if (error_message != nullptr) {
                *error_message = "recording_frame_id must be an integer";
            }
            return false;
        }
        expected->recording_frame_id = recording;
    }
    if (parts.size() >= 5 && !parts[4].empty()) {
        size_t min_boxes = 0;
        if (!parseSize(parts[4], &min_boxes)) {
            if (error_message != nullptr) {
                *error_message = "min_boxes must be an unsigned integer";
            }
            return false;
        }
        expected->min_boxes = min_boxes;
    }
    return true;
}

bool consumeValue(int argc,
                  char** argv,
                  int* index,
                  const char* option_name,
                  std::string* value,
                  std::string* error_message) {
    if (index == nullptr || value == nullptr) {
        return false;
    }
    if (*index + 1 >= argc) {
        if (error_message != nullptr) {
            *error_message = std::string(option_name) + " requires a value";
        }
        return false;
    }
    ++(*index);
    *value = argv[*index];
    return true;
}

bool parseOptions(int argc,
                  char** argv,
                  ProbeOptions* options,
                  std::string* error_message) {
    if (options == nullptr) {
        return false;
    }
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;
        if (arg == "--expect-collection") {
            if (!consumeValue(argc, argv, &i, arg.c_str(), &value,
                              error_message)) {
                return false;
            }
            options->expected_collection = value;
        } else if (arg == "--expect-selected-runs") {
            size_t parsed = 0;
            if (!consumeValue(argc, argv, &i, arg.c_str(), &value,
                              error_message) ||
                !parseSize(value, &parsed)) {
                if (error_message != nullptr) {
                    *error_message = arg + " requires an unsigned integer";
                }
                return false;
            }
            options->expected_selected_runs = parsed;
        } else if (arg == "--expect-mapped-frames") {
            size_t parsed = 0;
            if (!consumeValue(argc, argv, &i, arg.c_str(), &value,
                              error_message) ||
                !parseSize(value, &parsed)) {
                if (error_message != nullptr) {
                    *error_message = arg + " requires an unsigned integer";
                }
                return false;
            }
            options->expected_mapped_frames = parsed;
        } else if (arg == "--expect-total-parent-frames") {
            size_t parsed = 0;
            if (!consumeValue(argc, argv, &i, arg.c_str(), &value,
                              error_message) ||
                !parseSize(value, &parsed)) {
                if (error_message != nullptr) {
                    *error_message = arg + " requires an unsigned integer";
                }
                return false;
            }
            options->expected_total_parent_frames = parsed;
        } else if (arg == "--expect-unselected-frame-pairs") {
            size_t parsed = 0;
            if (!consumeValue(argc, argv, &i, arg.c_str(), &value,
                              error_message) ||
                !parseSize(value, &parsed)) {
                if (error_message != nullptr) {
                    *error_message = arg + " requires an unsigned integer";
                }
                return false;
            }
            options->expected_unselected_frame_pairs = parsed;
        } else if (arg == "--expect-coordinates-normalized") {
            bool parsed = false;
            if (!consumeValue(argc, argv, &i, arg.c_str(), &value,
                              error_message) ||
                !parseBool(value, &parsed)) {
                if (error_message != nullptr) {
                    *error_message = arg + " requires true or false";
                }
                return false;
            }
            options->expected_coordinates_normalized = parsed;
        } else if (arg == "--expect-frame") {
            ExpectedFrame expected;
            if (!consumeValue(argc, argv, &i, arg.c_str(), &value,
                              error_message) ||
                !parseExpectedFrame(value, &expected, error_message)) {
                return false;
            }
            options->expected_frames.push_back(std::move(expected));
        } else if (arg.rfind("--", 0) == 0) {
            if (error_message != nullptr) {
                *error_message = "unknown option: " + arg;
            }
            return false;
        } else {
            int64_t frame = -1;
            if (!parseInt64(arg, &frame)) {
                if (error_message != nullptr) {
                    *error_message = "invalid frame argument: " + arg;
                }
                return false;
            }
            options->print_frames.push_back(frame);
        }
    }
    return true;
}

void printFrameProbe(const ZarrDetectionLoader& loader, int64_t parent_frame) {
    const auto* row = loader.resolveClippedFrame(parent_frame);
    std::cout << "frame " << parent_frame << ": ";
    if (row == nullptr) {
        std::cout << "no clipped mapping" << std::endl;
        return;
    }

    const auto* selected =
        loader.getClippedResolver().selectedRun(row->selected_run_index);
    std::cout << "camera=" << row->camera_serial
              << " clip=" << row->clip_id
              << " clip_local=" << row->clip_local_frame_index
              << " recording_frame_id=" << row->recording_frame_id;
    if (selected != nullptr) {
        std::cout << " refined_group=" << selected->refined_group_path;
    }

    const auto boxes =
        loader.getBoundingBoxesForFrame(static_cast<size_t>(parent_frame));
    std::cout << " boxes=" << boxes.size();
    if (!boxes.empty()) {
        const auto& box = boxes.front();
        std::cout << " first_xywh=("
                  << box.x_min << ","
                  << box.y_min << ","
                  << box.width << ","
                  << box.height << ")";
    }
    std::cout << std::endl;
}

bool checkEqualString(const std::string& label,
                      const std::string& actual,
                      const std::string& expected,
                      int* failures) {
    if (actual == expected) {
        std::cout << "[OK] " << label << "=" << actual << std::endl;
        return true;
    }
    std::cerr << "[FAIL] " << label << ": expected " << expected
              << ", got " << actual << std::endl;
    if (failures != nullptr) {
        ++(*failures);
    }
    return false;
}

bool checkEqualSize(const std::string& label,
                    size_t actual,
                    size_t expected,
                    int* failures) {
    if (actual == expected) {
        std::cout << "[OK] " << label << "=" << actual << std::endl;
        return true;
    }
    std::cerr << "[FAIL] " << label << ": expected " << expected
              << ", got " << actual << std::endl;
    if (failures != nullptr) {
        ++(*failures);
    }
    return false;
}

bool checkEqualBool(const std::string& label,
                    bool actual,
                    bool expected,
                    int* failures) {
    if (actual == expected) {
        std::cout << "[OK] " << label << "="
                  << (actual ? "true" : "false") << std::endl;
        return true;
    }
    std::cerr << "[FAIL] " << label << ": expected "
              << (expected ? "true" : "false")
              << ", got " << (actual ? "true" : "false") << std::endl;
    if (failures != nullptr) {
        ++(*failures);
    }
    return false;
}

void checkExpectedFrame(const ZarrDetectionLoader& loader,
                        const ExpectedFrame& expected,
                        int* failures) {
    const auto* row = loader.resolveClippedFrame(expected.parent_frame);
    if (row == nullptr) {
        std::cerr << "[FAIL] frame " << expected.parent_frame
                  << ": no clipped mapping" << std::endl;
        if (failures != nullptr) {
            ++(*failures);
        }
        return;
    }

    bool ok = true;
    if (row->clip_id != expected.clip_id) {
        std::cerr << "[FAIL] frame " << expected.parent_frame
                  << " clip_id: expected " << expected.clip_id
                  << ", got " << row->clip_id << std::endl;
        ok = false;
    }
    if (row->clip_local_frame_index != expected.clip_local_frame) {
        std::cerr << "[FAIL] frame " << expected.parent_frame
                  << " clip_local: expected " << expected.clip_local_frame
                  << ", got " << row->clip_local_frame_index << std::endl;
        ok = false;
    }
    if (expected.recording_frame_id.has_value() &&
        row->recording_frame_id != *expected.recording_frame_id) {
        std::cerr << "[FAIL] frame " << expected.parent_frame
                  << " recording_frame_id: expected "
                  << *expected.recording_frame_id
                  << ", got " << row->recording_frame_id << std::endl;
        ok = false;
    }
    if (expected.min_boxes.has_value()) {
        const auto boxes =
            loader.getBoundingBoxesForFrame(
                static_cast<size_t>(expected.parent_frame));
        if (boxes.size() < *expected.min_boxes) {
            std::cerr << "[FAIL] frame " << expected.parent_frame
                      << " boxes: expected at least "
                      << *expected.min_boxes << ", got "
                      << boxes.size() << std::endl;
            ok = false;
        }
    }

    if (ok) {
        std::cout << "[OK] frame " << expected.parent_frame
                  << " -> " << row->clip_id
                  << " local " << row->clip_local_frame_index;
        if (expected.recording_frame_id.has_value()) {
            std::cout << " recording_frame_id=" << row->recording_frame_id;
        }
        if (expected.min_boxes.has_value()) {
            const auto boxes =
                loader.getBoundingBoxesForFrame(
                    static_cast<size_t>(expected.parent_frame));
            std::cout << " boxes=" << boxes.size();
        }
        std::cout << std::endl;
    } else if (failures != nullptr) {
        ++(*failures);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <analysis.zarr> [options] [parent_frame ...]\n"
                  << "Options:\n"
                  << "  --expect-collection ID\n"
                  << "  --expect-selected-runs N\n"
                  << "  --expect-mapped-frames N\n"
                  << "  --expect-total-parent-frames N\n"
                  << "  --expect-unselected-frame-pairs N\n"
                  << "  --expect-coordinates-normalized true|false\n"
                  << "  --expect-frame parent:clip_id:clip_local"
                     "[:recording_frame_id[:min_boxes]]"
                  << std::endl;
        return 1;
    }

    ProbeOptions options;
    std::string option_error;
    if (!parseOptions(argc, argv, &options, &option_error)) {
        std::cerr << "argument error: " << option_error << std::endl;
        return 1;
    }

    ZarrDetectionLoader loader;
    std::string error_message;
    if (!loader.loadZarrFile(argv[1], error_message)) {
        std::cerr << "load failed: " << error_message << std::endl;
        return 2;
    }

    if (!loader.hasClippedCollection()) {
        std::cerr << "archive did not load as a clipped collection" << std::endl;
        return 3;
    }

    const auto& resolver = loader.getClippedResolver();
    std::cout << "collection=" << resolver.collectionId() << std::endl;
    std::cout << "selected_runs=" << resolver.selectedRunCount() << std::endl;
    std::cout << "mapped_frames=" << resolver.mappedFrameCount() << std::endl;
    std::cout << "total_parent_frames=" << loader.getTotalFrames()
              << std::endl;
    std::cout << "unselected_frame_pairs="
              << resolver.unselectedFramePairCount() << std::endl;
    std::cout << "coordinates_normalized="
              << (loader.coordinatesAreNormalized() ? "true" : "false")
              << std::endl;

    int failures = 0;
    if (options.expected_collection.has_value()) {
        checkEqualString("collection", resolver.collectionId(),
                         *options.expected_collection, &failures);
    }
    if (options.expected_selected_runs.has_value()) {
        checkEqualSize("selected_runs", resolver.selectedRunCount(),
                       *options.expected_selected_runs, &failures);
    }
    if (options.expected_mapped_frames.has_value()) {
        checkEqualSize("mapped_frames", resolver.mappedFrameCount(),
                       *options.expected_mapped_frames, &failures);
    }
    if (options.expected_total_parent_frames.has_value()) {
        checkEqualSize("total_parent_frames", loader.getTotalFrames(),
                       *options.expected_total_parent_frames, &failures);
    }
    if (options.expected_unselected_frame_pairs.has_value()) {
        checkEqualSize("unselected_frame_pairs",
                       resolver.unselectedFramePairCount(),
                       *options.expected_unselected_frame_pairs, &failures);
    }
    if (options.expected_coordinates_normalized.has_value()) {
        checkEqualBool("coordinates_normalized",
                       loader.coordinatesAreNormalized(),
                       *options.expected_coordinates_normalized,
                       &failures);
    }
    for (const auto& expected : options.expected_frames) {
        checkExpectedFrame(loader, expected, &failures);
    }
    if (failures > 0) {
        std::cerr << failures << " probe expectation(s) failed" << std::endl;
        return 5;
    }

    if (options.print_frames.empty()) {
        const int64_t total =
            static_cast<int64_t>(std::min<size_t>(
                loader.getTotalFrames(),
                static_cast<size_t>(std::numeric_limits<int64_t>::max())));
        options.print_frames = {0, 1, 1000, 10000, 53999, 54000};
        if (total > 0) {
            options.print_frames.push_back(total - 1);
        }
    }

    for (const int64_t frame : options.print_frames) {
        if (frame < 0 ||
            static_cast<size_t>(frame) >= loader.getTotalFrames()) {
            std::cout << "frame " << frame << ": out of range" << std::endl;
            continue;
        }
        printFrameProbe(loader, frame);
    }

    return 0;
}
