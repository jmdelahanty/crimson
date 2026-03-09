#include "zarr_loader.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

struct Args {
    std::string zarr_path;
    std::vector<int32_t> frames;
    int max_detections = 16;
    std::string json_out_path;
};

void printUsage(const char* argv0) {
    std::cout << "Usage: " << argv0
              << " <zarr_path> --frame <idx[,idx2,...]> [--frame <idx>] "
                 "[--max-detections N] [--json-out path]\n";
}

bool parseFrameToken(const std::string& token, std::vector<int32_t>& out) {
    std::stringstream ss(token);
    std::string part;
    while (std::getline(ss, part, ',')) {
        if (part.empty()) {
            continue;
        }
        try {
            int64_t value = std::stoll(part);
            if (value < 0 || value > static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
                return false;
            }
            out.push_back(static_cast<int32_t>(value));
        } catch (...) {
            return false;
        }
    }
    return true;
}

bool parseArgs(int argc, char** argv, Args& args) {
    if (argc < 2) {
        return false;
    }
    args.zarr_path = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--frame") {
            if (i + 1 >= argc) {
                return false;
            }
            if (!parseFrameToken(argv[++i], args.frames)) {
                return false;
            }
        } else if (arg == "--max-detections") {
            if (i + 1 >= argc) {
                return false;
            }
            args.max_detections = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--json-out") {
            if (i + 1 >= argc) {
                return false;
            }
            args.json_out_path = argv[++i];
        } else {
            return false;
        }
    }
    std::sort(args.frames.begin(), args.frames.end());
    args.frames.erase(std::unique(args.frames.begin(), args.frames.end()),
                      args.frames.end());
    return !args.zarr_path.empty() && !args.frames.empty();
}

bool finitePair(const std::array<float, 2>& p) {
    return std::isfinite(p[0]) && std::isfinite(p[1]);
}

std::optional<int32_t> resolveRoiHintForFrame(const ZarrDetectionLoader& loader,
                                              int32_t frame,
                                              int32_t raw_hint) {
    if (raw_hint < 0 || frame < 0) {
        return std::nullopt;
    }
    const auto& roi_frame_indices = loader.getKeypointRoiFrameIndices();
    auto roiMatchesFrame = [&](int32_t roi_index) -> bool {
        return roi_index >= 0 &&
               static_cast<size_t>(roi_index) < roi_frame_indices.size() &&
               roi_frame_indices[roi_index] == frame;
    };

    if (roiMatchesFrame(raw_hint)) {
        return raw_hint;
    }

    auto from_frame_detection = loader.getKeypointRoiIndexForFrameDetection(
        static_cast<size_t>(frame), static_cast<size_t>(raw_hint), false);
    if (from_frame_detection.has_value() && roiMatchesFrame(*from_frame_detection)) {
        return from_frame_detection;
    }

    int32_t from_global_detection =
        loader.getKeypointRoiIndexForDetection(static_cast<size_t>(raw_hint));
    if (from_global_detection >= 0 && roiMatchesFrame(from_global_detection)) {
        return from_global_detection;
    }

    if (from_frame_detection.has_value()) {
        return from_frame_detection;
    }
    if (from_global_detection >= 0) {
        return from_global_detection;
    }
    return std::nullopt;
}

json pointToJson(const std::array<float, 2>& p) {
    return json::array({p[0], p[1]});
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parseArgs(argc, argv, args)) {
        printUsage(argv[0]);
        return 1;
    }

    ZarrDetectionLoader loader;
    std::string error_message;
    if (!loader.loadZarrFile(args.zarr_path, error_message)) {
        std::cerr << "[ERROR] Failed to load zarr: " << error_message << "\n";
        return 2;
    }

    json report;
    report["zarr_path"] = std::filesystem::absolute(args.zarr_path).string();
    report["total_frames"] = loader.getTotalFrames();
    report["active_detection_dataset"] = static_cast<int>(loader.getActiveDetectionDataset());
    report["has_keypoint_data"] = loader.hasKeypointData();
    report["keypoint_run_name"] = loader.getKeypointsRunName();
    report["keypoints_per_detection"] = loader.getKeypointsPerDetection();
    report["has_heading_data"] = loader.hasHeadingData();
    report["has_crop_images"] = loader.hasCropImages();
    report["movement_run_name"] = loader.getMovementRunName();
    report["movement_track_id"] = loader.getMovementTrackId();
    report["movement_series_count"] = loader.getMovementSeriesCount();
    report["frames"] = json::array();

    const auto& movement_frames = loader.getMovementFrameIndices();
    const auto& movement_detection_indices = loader.getMovementDetectionIndices();
    const auto& crop_frame_indices = loader.getCropFrameIndices();

    std::cout << "[INFO] Zarr: " << std::filesystem::absolute(args.zarr_path) << "\n";
    std::cout << "[INFO] Keypoints run: " << loader.getKeypointsRunName()
              << " (per-detection: " << loader.getKeypointsPerDetection() << ")\n";
    std::cout << "[INFO] Movement run/track: " << loader.getMovementRunName()
              << " / " << loader.getMovementTrackId() << "\n";
    std::cout << "[INFO] Frames requested: " << args.frames.size() << "\n";

    for (int32_t frame : args.frames) {
        json frame_json;
        frame_json["frame"] = frame;

        if (frame < 0 || static_cast<size_t>(frame) >= loader.getTotalFrames()) {
            frame_json["error"] = "frame out of range";
            report["frames"].push_back(frame_json);
            std::cout << "\n[FRAME " << frame << "] out of range (total_frames="
                      << loader.getTotalFrames() << ")\n";
            continue;
        }

        std::vector<json> movement_hints;
        for (size_t i = 0; i < movement_frames.size() && i < movement_detection_indices.size(); ++i) {
            if (movement_frames[i] != frame) {
                continue;
            }
            const int32_t raw_hint = movement_detection_indices[i];
            json hint;
            hint["sample_index"] = i;
            hint["raw_detection_or_roi_hint"] = raw_hint;
            auto resolved = resolveRoiHintForFrame(loader, frame, raw_hint);
            hint["resolved_roi_index"] = resolved.has_value() ? json(*resolved) : json(nullptr);
            movement_hints.push_back(std::move(hint));
        }
        frame_json["movement_hints"] = movement_hints;

        std::vector<int32_t> crop_roi_hints;
        for (size_t roi = 0; roi < crop_frame_indices.size(); ++roi) {
            if (crop_frame_indices[roi] == frame) {
                crop_roi_hints.push_back(static_cast<int32_t>(roi));
            }
        }
        frame_json["crop_roi_hints"] = crop_roi_hints;

        auto detections = loader.getRawDetections(static_cast<size_t>(frame), false, true);
        frame_json["detection_count"] = detections.boxes.size();
        frame_json["keypoint_count"] = detections.keypoints_pixels.size();
        frame_json["eye_mask_count"] = detections.eye_masks.size();
        frame_json["keypoint_roi_index_count"] = detections.keypoint_roi_indices.size();
        frame_json["keypoint_labels"] = detections.keypoint_labels;
        frame_json["detections"] = json::array();

        const size_t det_count = std::max(
            std::max(detections.boxes.size(), detections.keypoints_pixels.size()),
            std::max(detections.eye_masks.size(), detections.keypoint_roi_indices.size()));
        const size_t emit_count =
            std::min(det_count, static_cast<size_t>(args.max_detections));

        std::cout << "\n[FRAME " << frame << "] detections=" << detections.boxes.size()
                  << " keypoint_rows=" << detections.keypoints_pixels.size()
                  << " eye_masks=" << detections.eye_masks.size()
                  << " kp_roi_indices=" << detections.keypoint_roi_indices.size() << "\n";
        if (!movement_hints.empty()) {
            std::cout << "  movement_hints:";
            for (const auto& hint : movement_hints) {
                std::cout << " [raw=" << hint["raw_detection_or_roi_hint"]
                          << " resolved="
                          << (hint["resolved_roi_index"].is_null()
                                  ? std::string("null")
                                  : hint["resolved_roi_index"].dump())
                          << "]";
            }
            std::cout << "\n";
        }
        if (!crop_roi_hints.empty()) {
            std::cout << "  crop_roi_hints:";
            for (int32_t roi : crop_roi_hints) {
                std::cout << " " << roi;
            }
            std::cout << "\n";
        }

        for (size_t det_idx = 0; det_idx < emit_count; ++det_idx) {
            json det_json;
            det_json["det_index"] = det_idx;

            if (det_idx < detections.boxes.size()) {
                det_json["bbox_xyxy"] = detections.boxes[det_idx];
            } else {
                det_json["bbox_xyxy"] = nullptr;
            }
            if (det_idx < detections.detection_source.size()) {
                det_json["detection_source"] = detections.detection_source[det_idx];
            }
            if (det_idx < detections.heading_valid.size()) {
                det_json["heading_valid"] = detections.heading_valid[det_idx];
            }
            if (det_idx < detections.headings_deg.size()) {
                det_json["heading_deg"] = detections.headings_deg[det_idx];
            }
            if (det_idx < detections.swim_bladder_pixels.size()) {
                det_json["swim_bladder_xy"] =
                    pointToJson(detections.swim_bladder_pixels[det_idx]);
            }
            if (det_idx < detections.keypoint_roi_indices.size()) {
                det_json["roi_from_keypoint_roi_indices"] =
                    detections.keypoint_roi_indices[det_idx];
            }
            auto roi_from_loader = loader.getKeypointRoiIndexForFrameDetection(
                static_cast<size_t>(frame), det_idx, false);
            det_json["roi_from_loader"] =
                roi_from_loader.has_value() ? json(*roi_from_loader) : json(nullptr);

            if (det_idx < detections.eye_masks.size()) {
                const auto& eye_mask = detections.eye_masks[det_idx];
                det_json["eye_mask_roi_index"] = eye_mask.roi_index;
                det_json["eye_mask_offset_xy"] =
                    json::array({eye_mask.offset_x, eye_mask.offset_y});
                det_json["eye_mask_roi_size_wh"] =
                    json::array({eye_mask.roi_width, eye_mask.roi_height});
            }

            int finite_keypoints = 0;
            if (det_idx < detections.keypoints_pixels.size()) {
                const auto& kp_set = detections.keypoints_pixels[det_idx];
                json keypoint_sample = json::array();
                const size_t sample_count = std::min<size_t>(kp_set.size(), 5);
                for (size_t kp = 0; kp < kp_set.size(); ++kp) {
                    if (finitePair(kp_set[kp])) {
                        finite_keypoints++;
                    }
                    if (kp < sample_count) {
                        keypoint_sample.push_back(pointToJson(kp_set[kp]));
                    }
                }
                det_json["keypoints_sample_xy"] = keypoint_sample;
            }
            det_json["finite_keypoint_count"] = finite_keypoints;
            frame_json["detections"].push_back(det_json);

            std::ostringstream line;
            line << "  det " << det_idx;
            if (det_json.contains("roi_from_loader")) {
                line << " roi(loader)="
                     << (det_json["roi_from_loader"].is_null()
                             ? "null"
                             : det_json["roi_from_loader"].dump());
            }
            if (det_json.contains("roi_from_keypoint_roi_indices")) {
                line << " roi(vec)="
                     << det_json["roi_from_keypoint_roi_indices"].dump();
            }
            if (det_json.contains("eye_mask_roi_index")) {
                line << " eye_roi=" << det_json["eye_mask_roi_index"].dump();
            }
            if (det_json.contains("heading_valid")) {
                line << " heading_valid=" << det_json["heading_valid"].dump();
            }
            if (det_json.contains("heading_deg")) {
                line << " heading=" << std::fixed << std::setprecision(2)
                     << det_json["heading_deg"].get<double>();
            }
            line << " finite_kp=" << finite_keypoints;
            std::cout << line.str() << "\n";
        }

        report["frames"].push_back(std::move(frame_json));
    }

    if (!args.json_out_path.empty()) {
        std::ofstream out(args.json_out_path);
        if (!out) {
            std::cerr << "[ERROR] Failed to open json output: "
                      << args.json_out_path << "\n";
            return 3;
        }
        out << report.dump(2) << "\n";
        std::cout << "\n[INFO] Wrote JSON report: " << args.json_out_path << "\n";
    } else {
        std::cout << "\n[JSON]\n" << report.dump(2) << "\n";
    }

    return 0;
}

