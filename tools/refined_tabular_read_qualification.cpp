#include "refined_keypoint_repository.h"
#include "zarr_loader.h"

#include <nlohmann/json.hpp>

#include <sys/resource.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using json = nlohmann::json;

struct Options {
    std::string archive;
    std::string layout;
    std::string output;
    size_t queries = 20000;
    uint64_t seed = 2010095;
    bool edit = false;
};

struct QueryResult {
    std::vector<double> latency_us;
    uint64_t checksum = 1469598103934665603ULL;
    uint64_t boxes = 0;
    uint64_t keypoint_sets = 0;
    uint64_t keypoints = 0;
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
    } catch (...) {
        return false;
    }
}

bool parseUint64(const std::string& text, uint64_t* value) {
    try {
        size_t consumed = 0;
        const auto parsed = std::stoull(text, &consumed);
        if (consumed != text.size()) {
            return false;
        }
        *value = static_cast<uint64_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool parseOptions(int argc, char** argv, Options* options) {
    if (argc < 2) {
        return false;
    }
    options->archive = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                return {};
            }
            return argv[++i];
        };
        if (arg == "--layout") {
            options->layout = value();
        } else if (arg == "--output") {
            options->output = value();
        } else if (arg == "--queries") {
            const std::string text = value();
            if (!parseSize(text, &options->queries) || options->queries == 0) {
                return false;
            }
        } else if (arg == "--seed") {
            const std::string text = value();
            if (!parseUint64(text, &options->seed)) {
                return false;
            }
        } else if (arg == "--edit") {
            options->edit = true;
        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            return false;
        }
    }
    return !options->archive.empty() && !options->output.empty();
}

double elapsedMs(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

uint64_t currentRssKb() {
#if defined(__linux__)
    std::ifstream input("/proc/self/status");
    std::string line;
    while (std::getline(input, line)) {
        if (line.rfind("VmRSS:", 0) != 0) {
            continue;
        }
        std::istringstream stream(line.substr(6));
        uint64_t kb = 0;
        stream >> kb;
        return kb;
    }
#endif
    return 0;
}

uint64_t peakRssKb() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
#if defined(__APPLE__)
    return static_cast<uint64_t>(usage.ru_maxrss) / 1024ULL;
#else
    return static_cast<uint64_t>(usage.ru_maxrss);
#endif
}

void hashBytes(uint64_t* hash, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        *hash ^= bytes[i];
        *hash *= 1099511628211ULL;
    }
}

template <typename T>
void hashValue(uint64_t* hash, const T& value) {
    hashBytes(hash, &value, sizeof(value));
}

void hashFrame(uint64_t* hash,
               size_t requested_frame,
               const ZarrDetectionLoader::FrameDetections& frame) {
    hashValue(hash, requested_frame);
    hashValue(hash, frame.frame_id);
    const uint64_t box_count = frame.boxes.size();
    hashValue(hash, box_count);
    for (const auto& box : frame.boxes) {
        for (float value : box) {
            hashValue(hash, value);
        }
    }
    const uint64_t set_count = frame.keypoints_pixels.size();
    hashValue(hash, set_count);
    for (const auto& points : frame.keypoints_pixels) {
        const uint64_t point_count = points.size();
        hashValue(hash, point_count);
        for (const auto& point : points) {
            hashValue(hash, point[0]);
            hashValue(hash, point[1]);
        }
    }
    for (float heading : frame.headings_deg) {
        hashValue(hash, heading);
    }
}

json latencySummary(const std::vector<double>& samples) {
    if (samples.empty()) {
        return json::object();
    }
    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    auto percentile = [&](double fraction) {
        const double position = fraction * static_cast<double>(sorted.size() - 1);
        const size_t lower = static_cast<size_t>(std::floor(position));
        const size_t upper = static_cast<size_t>(std::ceil(position));
        const double weight = position - static_cast<double>(lower);
        return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
    };
    double sum = 0.0;
    for (double value : samples) {
        sum += value;
    }
    return {
        {"count", samples.size()},
        {"mean_us", sum / static_cast<double>(samples.size())},
        {"p50_us", percentile(0.50)},
        {"p95_us", percentile(0.95)},
        {"p99_us", percentile(0.99)},
        {"max_us", sorted.back()},
    };
}

QueryResult runQueries(const ZarrDetectionLoader& loader,
                       const std::vector<size_t>& frames) {
    QueryResult result;
    result.latency_us.reserve(frames.size());
    for (size_t frame_id : frames) {
        const auto start = Clock::now();
        const auto frame = loader.getRawDetections(
            frame_id,
            false,
            false,
            false,
            true,
            false);
        const auto end = Clock::now();
        result.latency_us.push_back(
            std::chrono::duration<double, std::micro>(end - start).count());
        result.boxes += frame.boxes.size();
        result.keypoint_sets += frame.keypoints_pixels.size();
        for (const auto& points : frame.keypoints_pixels) {
            result.keypoints += points.size();
        }
        hashFrame(&result.checksum, frame_id, frame);
    }
    return result;
}

std::string datasetName(ZarrDetectionLoader::DetectionDataset dataset) {
    switch (dataset) {
        case ZarrDetectionLoader::DetectionDataset::RawDetect:
            return "raw_detect";
        case ZarrDetectionLoader::DetectionDataset::RefinedFiltered:
            return "refined_filtered";
        case ZarrDetectionLoader::DetectionDataset::RefinedInterpolated:
            return "refined_interpolated";
        case ZarrDetectionLoader::DetectionDataset::RefinedManual:
            return "refined_manual";
        case ZarrDetectionLoader::DetectionDataset::RefinedRoot:
            return "refined_root";
    }
    return "unknown";
}

json queryJson(const QueryResult& result) {
    std::ostringstream checksum;
    checksum << std::hex << std::setw(16) << std::setfill('0')
             << result.checksum;
    return {
        {"latency", latencySummary(result.latency_us)},
        {"checksum_fnv1a64", checksum.str()},
        {"boxes", result.boxes},
        {"keypoint_sets", result.keypoint_sets},
        {"keypoints", result.keypoints},
    };
}

json qualifyEdit(const ZarrDetectionLoader& loader, size_t max_scan_frames) {
    json output = {
        {"requested", true},
        {"success", false},
    };
    RefinedKeypointRepository repository(loader);
    std::string reason;
    if (!repository.canEditActiveRun(&reason)) {
        output["error"] = reason;
        return output;
    }

    const size_t scan_limit = std::min(loader.getTotalFrames(), max_scan_frames);
    for (size_t frame_id = 0; frame_id < scan_limit; ++frame_id) {
        const auto frame = loader.getRawDetections(
            frame_id, false, false, false, true, false);
        if (!frame.has_keypoints || frame.keypoints_pixels.empty() ||
            frame.keypoints_pixels[0].empty()) {
            continue;
        }
        const auto selection = repository.resolveFrameDetectionSelection(
            frame_id, 0, false);
        if (!selection.valid || !selection.editable ||
            !selection.roi_metadata.has_crop_metadata) {
            continue;
        }

        std::vector<std::array<double, 2>> original_roi;
        original_roi.reserve(frame.keypoints_pixels[0].size());
        for (const auto& point : frame.keypoints_pixels[0]) {
            original_roi.push_back({
                static_cast<double>(point[0]) - selection.roi_metadata.offset_x,
                static_cast<double>(point[1]) - selection.roi_metadata.offset_y,
            });
        }
        auto edited_roi = original_roi;
        edited_roi[0][0] += 0.03125;

        RefinedKeypointEditResult edit_result;
        std::string error;
        const auto edit_start = Clock::now();
        const bool edit_ok = repository.writeManualCorrection(
            selection, edited_roi, error, &edit_result);
        const auto edit_end = Clock::now();
        if (!edit_ok) {
            output["frame"] = frame_id;
            output["roi_index"] = selection.roi_index;
            output["error"] = error;
            output["edit_ms"] = elapsedMs(edit_start, edit_end);
            return output;
        }

        RefinedKeypointEditResult restore_result;
        std::string restore_error;
        const auto restore_start = Clock::now();
        const bool restore_ok = repository.writeManualCorrection(
            selection, original_roi, restore_error, &restore_result);
        const auto restore_end = Clock::now();

        output = {
            {"requested", true},
            {"success", edit_ok && restore_ok},
            {"frame", frame_id},
            {"roi_index", selection.roi_index},
            {"keypoint_count", original_roi.size()},
            {"edit_ms", elapsedMs(edit_start, edit_end)},
            {"restore_ms", elapsedMs(restore_start, restore_end)},
            {"edit_changed", edit_result.changed},
            {"restore_changed", restore_result.changed},
            {"edit_summary_updated", edit_result.summary_updated},
            {"restore_summary_updated", restore_result.summary_updated},
        };
        if (!restore_ok) {
            output["restore_error"] = restore_error;
        }
        return output;
    }

    output["error"] = "No editable refined-keypoint row found in scan range.";
    return output;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, &options)) {
        std::cerr
            << "Usage: " << argv[0]
            << " <analysis.zarr> --output RESULT.json [--layout LABEL]"
               " [--queries N] [--seed N] [--edit]"
            << std::endl;
        return 1;
    }

    json result = {
        {"schema_id", "crimson_refined_tabular_read_qualification_v1"},
        {"archive", options.archive},
        {"layout", options.layout},
        {"query_count", options.queries},
        {"random_seed", options.seed},
        {"baseline_rss_kb", currentRssKb()},
        {"baseline_peak_rss_kb", peakRssKb()},
    };

    ZarrDetectionLoader loader;
    std::string error;
    const auto load_start = Clock::now();
    const bool loaded = loader.loadZarrFile(options.archive, error);
    const auto load_end = Clock::now();
    result["load_ok"] = loaded;
    result["load_ms"] = elapsedMs(load_start, load_end);
    result["post_load_rss_kb"] = currentRssKb();
    result["post_load_peak_rss_kb"] = peakRssKb();
    if (!loaded) {
        result["error"] = error;
    } else {
        result["total_frames"] = loader.getTotalFrames();
        result["fps"] = loader.getFPS();
        result["image_width"] = loader.getImageWidth();
        result["image_height"] = loader.getImageHeight();
        result["active_detection_dataset"] =
            datasetName(loader.getActiveDetectionDataset());
        result["detect_run"] = loader.getDetectRunName();
        result["keypoint_run"] = loader.getKeypointsRunName();
        result["refined_keypoints"] = loader.isRefinedKeypoints();
        result["has_keypoints"] = loader.hasKeypointData();

        const size_t sequential_count =
            std::min(options.queries, loader.getTotalFrames());
        std::vector<size_t> sequential_frames(sequential_count);
        for (size_t i = 0; i < sequential_count; ++i) {
            sequential_frames[i] = i;
        }

        std::vector<size_t> random_frames;
        random_frames.reserve(options.queries);
        std::mt19937_64 generator(options.seed);
        std::uniform_int_distribution<size_t> distribution(
            0, loader.getTotalFrames() - 1);
        for (size_t i = 0; i < options.queries; ++i) {
            random_frames.push_back(distribution(generator));
        }

        result["sequential_queries"] = queryJson(
            runQueries(loader, sequential_frames));
        result["random_queries"] = queryJson(
            runQueries(loader, random_frames));
        result["post_queries_rss_kb"] = currentRssKb();
        result["post_queries_peak_rss_kb"] = peakRssKb();
        result["edit"] = options.edit
            ? qualifyEdit(loader, std::min<size_t>(loader.getTotalFrames(), 100000))
            : json{{"requested", false}};
        result["final_rss_kb"] = currentRssKb();
        result["final_peak_rss_kb"] = peakRssKb();

        const bool correct_dataset =
            loader.getActiveDetectionDataset() ==
            ZarrDetectionLoader::DetectionDataset::RefinedRoot;
        result["qualification_ok"] =
            correct_dataset && loader.isRefinedKeypoints() &&
            loader.hasKeypointData() &&
            (!options.edit || result["edit"].value("success", false));
    }

    const std::filesystem::path output_path(options.output);
    if (!output_path.parent_path().empty()) {
        std::filesystem::create_directories(output_path.parent_path());
    }
    std::ofstream output(output_path);
    if (!output) {
        std::cerr << "Could not open output: " << options.output << std::endl;
        return 3;
    }
    output << std::setw(2) << result << '\n';
    output.close();

    std::cout << "[RefinedTabularQualification] layout=" << options.layout
              << " load_ms=" << result.value("load_ms", 0.0)
              << " rss_mb=" << result.value("post_load_rss_kb", 0ULL) / 1024.0
              << " output=" << options.output << std::endl;
    if (!loaded) {
        std::cerr << "Load failed: " << error << std::endl;
        return 2;
    }
    return result.value("qualification_ok", false) ? 0 : 4;
}
