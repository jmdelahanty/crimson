#include "zarr_loader_internal.h"
#include <iostream>

using json = nlohmann::json;

bool ZarrDetectionLoader::loadMovementData(const ts::kvstore::KvStore& store) {
    data_.has_movement_data = false;
    data_.movement_series.clear();
    data_.movement_selected_index = std::numeric_limits<size_t>::max();
    data_.movement_crop_run_name.clear();
    data_.crop_data = {};

    bool loaded = loadLegacyMovementData(store);

    if (!loaded) {
        return false;
    }

    finalizeMovementSelection();
    if (data_.has_movement_data) {
        std::string crop_candidate = data_.movement_crop_run_name;
        if (crop_candidate.empty() && !data_.keypoints_source_crop_run.empty()) {
            crop_candidate = NormalizeCropRunName(data_.keypoints_source_crop_run);
        }
        if (!crop_candidate.empty()) {
            loadMovementCropRun(store, crop_candidate);
        }
    }
    return data_.has_movement_data;
}
bool ZarrDetectionLoader::loadLegacyMovementData(const ts::kvstore::KvStore& store) {
    const std::vector<std::pair<std::string, std::string>> categories = {
        {"analysis/movement_runs/offline", "offline"},
        {"analysis/movement_runs/online_refined", "online_refined"},
        {"analysis/movement_runs/online", "online"}
    };

    bool loaded_any = false;

    for (const auto& [group_path, category_name] : categories) {
        std::vector<std::string> run_candidates;
        if (auto group_attrs = readGroupAttrs(store, group_path)) {
            const std::string latest = extractLatestRunName(*group_attrs);
            if (!latest.empty()) {
                run_candidates.push_back(latest);
            }
        }
        if (!root_path_.empty()) {
            auto runs = collect_runs_fs(root_path_, group_path, {});
            run_candidates.insert(run_candidates.end(), runs.begin(), runs.end());
        }
        std::sort(run_candidates.begin(), run_candidates.end());
        run_candidates.erase(std::unique(run_candidates.begin(), run_candidates.end()), run_candidates.end());
        if (run_candidates.empty()) {
            continue;
        }

        // Only load the most recent run for legacy data
        const std::string& run_name = run_candidates.back();
        std::string run_base = group_path + "/" + run_name + "/";

        float pixels_per_mm = 0.0f;
        std::vector<std::string> track_ids;
        auto append_track = [&](const std::string& track) {
            if (!track.empty()) {
                track_ids.push_back(track);
            }
        };

        if (auto run_attrs_opt = readGroupAttrs(store, run_base)) {
            const json& run_attrs = *run_attrs_opt;
            auto readPixelsPerMm = [&](const char* key) {
                if (run_attrs.contains(key) && run_attrs[key].is_number()) {
                    pixels_per_mm = static_cast<float>(run_attrs[key].get<double>());
                }
            };
            readPixelsPerMm("pixels_per_mm");
            readPixelsPerMm("pixels_per_mm_camera");
            readPixelsPerMm("pixel_to_mm");

            std::string crop_candidate;
            crop_candidate = ExtractCropRunFromObject(run_attrs);
            if (run_attrs.contains("inputs") && run_attrs["inputs"].is_object()) {
                const auto& inputs = run_attrs["inputs"];
                std::string from_inputs = ExtractCropRunFromObject(inputs);
                if (!from_inputs.empty()) {
                    crop_candidate = from_inputs;
                }
                if (crop_candidate.empty()) {
                    std::string keypoint_run;
                    if (inputs.contains("keypoint_run") && inputs["keypoint_run"].is_string()) {
                        keypoint_run = inputs["keypoint_run"].get<std::string>();
                    } else if (inputs.contains("base_keypoint_run") && inputs["base_keypoint_run"].is_string()) {
                        keypoint_run = inputs["base_keypoint_run"].get<std::string>();
                    }
                    if (!keypoint_run.empty()) {
                        std::string resolved = ResolveCropRunFromKeypointRun(store, keypoint_run);
                        if (!resolved.empty()) {
                            crop_candidate = resolved;
                        }
                    }
                }
            }
            if (!crop_candidate.empty()) {
                data_.movement_crop_run_name = crop_candidate;
            }

            const std::vector<std::string> track_keys = {"primary_track", "default_track", "track_id"};
            for (const auto& key : track_keys) {
                if (run_attrs.contains(key)) {
                    const auto& value = run_attrs[key];
                    if (value.is_string()) {
                        append_track(value.get<std::string>());
                    } else if (value.is_number_integer()) {
                        append_track("id_" + std::to_string(value.get<int>()));
                    }
                }
            }
        }

        if (!root_path_.empty()) {
            namespace fs = std::filesystem;
            fs::path track_root = fs::path(root_path_) / run_base / "tracks";
            if (fs::exists(track_root) && fs::is_directory(track_root)) {
                for (const auto& entry : fs::directory_iterator(track_root)) {
                    if (entry.is_directory()) {
                        append_track(entry.path().filename().string());
                    }
                }
            }
        }

        if (track_ids.empty()) {
            append_track("id_0");
        }

        std::sort(track_ids.begin(), track_ids.end());
        track_ids.erase(std::unique(track_ids.begin(), track_ids.end()), track_ids.end());

        std::vector<int64_t> run_camera_frame_ids;
        readInt64Array(store, run_base + "camera_frame_ids", run_camera_frame_ids);

        auto readDistanceArray = [&](const std::vector<std::string>& names,
                                     std::vector<float>& dest) -> bool {
            for (const auto& name : names) {
                if (readFloatArray(store, run_base + name, dest) && !dest.empty()) {
                    return true;
                }
            }
            dest.clear();
            return false;
        };

        auto loadDistanceValues = [&](const std::vector<std::string>& mm_names,
                                      const std::vector<std::string>& px_names,
                                      std::vector<float>& dest,
                                      const char* label) -> bool {
            if (readDistanceArray(mm_names, dest)) {
                return true;
            }
            if (!px_names.empty()) {
                std::vector<float> px;
                if (readDistanceArray(px_names, px) && !px.empty()) {
                    if (pixels_per_mm > 1e-6f) {
                        dest.resize(px.size());
                        for (size_t i = 0; i < px.size(); ++i) {
                            dest[i] = px[i] / pixels_per_mm;
                        }
                        return true;
                    }
                    std::cout << "  [LegacyMovement] Unable to convert " << label << " for run '"
                              << run_name << "' (pixels_per_mm missing)" << std::endl;
                }
            }
            return false;
        };

        const std::vector<std::string> smoothed_distance_mm_names = {"distance_to_target_smoothed_mm"};
        const std::vector<std::string> smoothed_distance_px_names = {"distance_to_target_smoothed_px"};
        const std::vector<std::string> raw_distance_mm_names = {"distance_to_target_mm"};
        const std::vector<std::string> raw_distance_px_names = {"distance_to_target_px"};

        std::vector<float> run_distance_to_target_mm;
        bool using_smoothed_distance = loadDistanceValues(
                                       smoothed_distance_mm_names,
                                       smoothed_distance_px_names,
                                       run_distance_to_target_mm,
                                       "distance_to_target_smoothed_px");
        bool run_distance_loaded = using_smoothed_distance;
        if (!run_distance_loaded) {
            run_distance_loaded = loadDistanceValues(
                raw_distance_mm_names,
                raw_distance_px_names,
                run_distance_to_target_mm,
                "distance_to_target_px");
            using_smoothed_distance = false;
        }
        const char* distance_label = using_smoothed_distance ? "distance_to_target_smoothed_mm"
                                                             : "distance_to_target_mm";

        std::vector<uint8_t> run_has_offline_flags;
        readBoolArray(store, run_base + "has_offline", run_has_offline_flags);

        std::unordered_map<int64_t, size_t> run_camera_lookup;
        const std::vector<int64_t>* run_camera_ptr = nullptr;
        const std::unordered_map<int64_t, size_t>* run_lookup_ptr = nullptr;
        const std::vector<float>* run_distance_ptr = nullptr;
        const std::vector<uint8_t>* run_offline_ptr = nullptr;

        if (!run_camera_frame_ids.empty()) {
            run_camera_ptr = &run_camera_frame_ids;
            run_camera_lookup.reserve(run_camera_frame_ids.size());
            for (size_t i = 0; i < run_camera_frame_ids.size(); ++i) {
                run_camera_lookup.emplace(run_camera_frame_ids[i], i);
            }
            if (!run_camera_lookup.empty()) {
                run_lookup_ptr = &run_camera_lookup;
            }
        }

        if (run_camera_ptr && !run_distance_to_target_mm.empty()) {
            if (run_distance_to_target_mm.size() == run_camera_frame_ids.size()) {
                run_distance_ptr = &run_distance_to_target_mm;
            } else {
                std::cout << "  [LegacyMovement] Ignoring " << distance_label << " for run '"
                          << run_name << "' due to size mismatch (camera_frame_ids="
                          << run_camera_frame_ids.size()
                          << ", " << distance_label << "=" << run_distance_to_target_mm.size()
                          << ")" << std::endl;
            }
        }

        if (run_distance_ptr && !run_has_offline_flags.empty()) {
            if (run_has_offline_flags.size() == run_distance_to_target_mm.size()) {
                run_offline_ptr = &run_has_offline_flags;
            } else {
                std::cout << "  [LegacyMovement] Ignoring has_offline mask for run '"
                          << run_name << "' due to size mismatch (" << distance_label << "="
                          << run_distance_to_target_mm.size()
                          << ", has_offline=" << run_has_offline_flags.size() << ")"
                          << std::endl;
            }
        }

        const std::vector<std::string> frame_names = {"frames", "frame_indices"};
        const std::vector<std::string> time_float_names = {"time_seconds"};
        const std::vector<std::string> timestamp_ns_names = {"timestamp_ns_session"};
        const std::vector<std::string> smoothed_mm_names = {
            "speed_smoothed_mm",
            "smoothed_speed_mm",
            "speed_smoothed_mm_per_s",
            "smoothed_speed_mm_per_s",
            "speed_smoothed_mmps",
            "smoothed_speed_mmps"
        };
        const std::vector<std::string> smoothed_px_names = {
            "speed_smoothed_px",
            "smoothed_speed_px",
            "speed_smoothed_px_per_s",
            "smoothed_speed_px_per_s",
            "speed_smoothed_pxps",
            "smoothed_speed_pxps"
        };
        const std::vector<std::string> instant_mm_names = {
            "speed_raw_mm",
            "speed_filtered_mm",
            "instantaneous_speed_mm",
            "instantaneous_speed_mm_per_s",
            "instantaneous_speed_mmps"
        };
        const std::vector<std::string> instant_px_names = {
            "speed_raw_px",
            "speed_filtered_px",
            "instantaneous_speed_px",
            "instantaneous_speed_px_per_s",
            "instantaneous_speed_pxps"
        };

        for (const auto& track_id : track_ids) {
            std::string track_base = run_base + "tracks/" + track_id + "/";
            bool loaded_track = loadMovementTrack(
                store,
                "[LegacyMovement]",
                run_name,
                track_id,
                track_base,
                frame_names,
                time_float_names,
                timestamp_ns_names,
                smoothed_mm_names,
                smoothed_px_names,
                instant_mm_names,
                instant_px_names,
                pixels_per_mm,
                0.0,
                category_name,
                category_name,
                std::string(),
                0.0,
                0,
                0,
                false,
                run_camera_ptr,
                run_lookup_ptr,
                run_distance_ptr,
                run_offline_ptr);
            loaded_any = loaded_any || loaded_track;
        }
    }

    if (loaded_any && !data_.movement_crop_run_name.empty()) {
        loadMovementCropRun(store, data_.movement_crop_run_name);
    }

    return loaded_any;
}

bool ZarrDetectionLoader::loadMovementTrack(
    const ts::kvstore::KvStore& store,
    const std::string& log_tag,
    const std::string& run_name,
    const std::string& track_id,
    const std::string& track_base,
    const std::vector<std::string>& frame_names,
    const std::vector<std::string>& time_float_names,
    const std::vector<std::string>& timestamp_ns_names,
    const std::vector<std::string>& smoothed_mm_names,
    const std::vector<std::string>& smoothed_px_names,
    const std::vector<std::string>& instant_mm_names,
    const std::vector<std::string>& instant_px_names,
    float pixels_per_mm,
    double run_fps,
    const std::string& category,
    const std::string& detection_variant,
    const std::string& source_detect_run,
    double smoothing_seconds,
    int video_width,
    int video_height,
    bool from_speed_runs,
    const std::vector<int64_t>* run_camera_frame_ids,
    const std::unordered_map<int64_t, size_t>* run_camera_lookup,
    const std::vector<float>* run_distance_to_target_mm,
    const std::vector<uint8_t>* run_has_offline_flags) {

    auto readInt32Or64List = [&](const std::vector<std::string>& candidates,
                                 std::vector<int32_t>& dest) {
        dest.clear();
        for (const auto& name : candidates) {
            if (readInt32Array(store, track_base + name, dest) && !dest.empty()) {
                return true;
            }
            std::vector<int64_t> tmp64;
            if (readInt64Array(store, track_base + name, tmp64) && !tmp64.empty()) {
                dest.resize(tmp64.size());
                for (size_t i = 0; i < tmp64.size(); ++i) {
                    dest[i] = clampToInt32(tmp64[i]);
                }
                return true;
            }
        }
        return false;
    };

    std::vector<int32_t> frame_indices;
    readInt32Or64List(frame_names, frame_indices);

    std::vector<int32_t> detection_indices;
    readInt32Or64List({"detection_indices", "roi_indices", "detection_index"}, detection_indices);

    std::vector<float> time_seconds;
    bool time_loaded = false;
    for (const auto& name : time_float_names) {
        if (readFloatArray(store, track_base + name, time_seconds) && !time_seconds.empty()) {
            time_loaded = true;
            break;
        }
    }
    if (!time_loaded) {
        std::vector<int64_t> timestamps_ns;
        for (const auto& name : timestamp_ns_names) {
            if (readInt64Array(store, track_base + name, timestamps_ns) && !timestamps_ns.empty()) {
                time_seconds.resize(timestamps_ns.size());
                constexpr double kNsToSeconds = 1e-9;
                for (size_t i = 0; i < timestamps_ns.size(); ++i) {
                    time_seconds[i] = static_cast<float>(timestamps_ns[i] * kNsToSeconds);
                }
                time_loaded = true;
                break;
            }
        }
    }
    if (!time_loaded && run_fps > 0.0 && !frame_indices.empty()) {
        time_seconds.resize(frame_indices.size());
        double inv_fps = 1.0 / run_fps;
        for (size_t i = 0; i < frame_indices.size(); ++i) {
            time_seconds[i] = static_cast<float>(static_cast<double>(frame_indices[i]) * inv_fps);
        }
        time_loaded = true;
    }
    if (!time_loaded) {
        return false;
    }

    auto readSpeedArray = [&](const std::vector<std::string>& names,
                              std::vector<float>& dest) -> bool {
        for (const auto& name : names) {
            if (readFloatArray(store, track_base + name, dest) && !dest.empty()) {
                return true;
            }
        }
        dest.clear();
        return false;
    };

    auto readSpeedValues = [&](const std::vector<std::string>& mm_names,
                               const std::vector<std::string>& px_names,
                               std::vector<float>& dest,
                               const char* label) -> bool {
        if (readSpeedArray(mm_names, dest)) {
            return true;
        }
        if (!px_names.empty()) {
            std::vector<float> px;
            if (readSpeedArray(px_names, px) && !px.empty()) {
                if (pixels_per_mm > 1e-6f) {
                    dest.resize(px.size());
                    for (size_t i = 0; i < px.size(); ++i) {
                        dest[i] = px[i] / pixels_per_mm;
                    }
                    return true;
                }
                std::cout << "  " << log_tag << " Unable to convert " << label
                          << " for run '" << run_name << "' track '" << track_id
                          << "' (pixels_per_mm missing)" << std::endl;
            }
        }
        return false;
    };

    std::vector<float> smoothed_mm;
    std::vector<float> instant_mm;
    bool has_smoothed = readSpeedValues(smoothed_mm_names, smoothed_px_names, smoothed_mm, "smoothed speed");
    bool has_instant = readSpeedValues(instant_mm_names, instant_px_names, instant_mm, "instantaneous speed");

    std::vector<float> heading_degrees;
    readFloatArray(store, track_base + "heading_degrees", heading_degrees);

    std::vector<float> smoothed_heading_degrees;
    readFloatArray(store, track_base + "smoothed_heading_degrees", smoothed_heading_degrees);

    std::vector<uint8_t> keypoint_success;
    readBoolArray(store, track_base + "keypoint_success", keypoint_success);

    std::vector<float> heading_per_second_degrees;
    readFloatArray(store, track_base + "heading_per_second_degrees", heading_per_second_degrees);

    std::vector<float> heading_per_second_resultant;
    readFloatArray(store, track_base + "heading_per_second_resultant", heading_per_second_resultant);

    std::vector<float> heading_per_second_time_seconds;
    const std::vector<std::string> heading_per_second_time_names = {
        "heading_per_second_time_seconds",
        "heading_per_second_seconds"
    };
    for (const auto& name : heading_per_second_time_names) {
        if (readFloatArray(store, track_base + name, heading_per_second_time_seconds) &&
            !heading_per_second_time_seconds.empty()) {
            break;
        }
    }

    if (!has_smoothed && !has_instant) {
        return false;
    }

    size_t sample_count = time_seconds.size();
    if (has_smoothed) {
        sample_count = std::min(sample_count, smoothed_mm.size());
    }
    if (has_instant) {
        sample_count = std::min(sample_count, instant_mm.size());
    }
    if (!heading_degrees.empty()) {
        sample_count = std::min(sample_count, heading_degrees.size());
    }
    if (!smoothed_heading_degrees.empty()) {
        sample_count = std::min(sample_count, smoothed_heading_degrees.size());
    }
    if (!keypoint_success.empty()) {
        sample_count = std::min(sample_count, keypoint_success.size());
    }
    if (!frame_indices.empty()) {
        sample_count = std::min(sample_count, frame_indices.size());
    }
    if (sample_count == 0) {
        return false;
    }

    auto trim_to = [&](auto& vec) {
        if (!vec.empty() && vec.size() > sample_count) {
            vec.resize(sample_count);
        }
    };
    trim_to(time_seconds);
    trim_to(smoothed_mm);
    trim_to(instant_mm);
    trim_to(heading_degrees);
    trim_to(smoothed_heading_degrees);
    trim_to(keypoint_success);
    trim_to(frame_indices);
    trim_to(detection_indices);

    std::vector<float> distance_series;
    if (run_camera_frame_ids && run_camera_lookup && run_distance_to_target_mm &&
        !run_camera_frame_ids->empty() && !run_distance_to_target_mm->empty()) {
        const auto& camera_ids = *run_camera_frame_ids;
        const auto& distance_mm = *run_distance_to_target_mm;
        const std::vector<uint8_t>* has_offline = nullptr;
        if (run_has_offline_flags &&
            run_has_offline_flags->size() == distance_mm.size()) {
            has_offline = run_has_offline_flags;
        }

        distance_series.assign(time_seconds.size(), std::numeric_limits<float>::quiet_NaN());
        bool any_valid = false;

        for (size_t i = 0; i < frame_indices.size() && i < distance_series.size(); ++i) {
            int32_t frame = frame_indices[i];
            auto lookup_it = run_camera_lookup->find(static_cast<int64_t>(frame));
            if (lookup_it == run_camera_lookup->end()) {
                continue;
            }
            size_t run_idx = lookup_it->second;
            if (run_idx >= distance_mm.size()) {
                continue;
            }
            if (has_offline && (run_idx >= has_offline->size() || (*has_offline)[run_idx] == 0)) {
                continue;
            }
            float value = distance_mm[run_idx];
            if (!std::isfinite(static_cast<double>(value))) {
                continue;
            }
            distance_series[i] = value;
            any_valid = true;
        }

        if (!any_valid) {
            distance_series.clear();
        }
    }

    ZarrDetectionData::MovementSeries series;
    series.category = category;
    series.run_name = run_name;
    series.track_id = track_id;
    series.detection_variant = detection_variant;
    series.source_detect_run = source_detect_run;
    series.fps = run_fps;
    series.smoothing_seconds = smoothing_seconds;
    series.video_width = video_width;
    series.video_height = video_height;
    series.from_speed_runs = from_speed_runs;
    series.time_seconds = std::move(time_seconds);
    series.smoothed_speed_mm = std::move(smoothed_mm);
    series.instant_speed_mm = std::move(instant_mm);
    series.heading_degrees = std::move(heading_degrees);
    series.smoothed_heading_degrees = std::move(smoothed_heading_degrees);
    series.keypoint_success = std::move(keypoint_success);
    series.frame_indices = std::move(frame_indices);
    series.detection_indices = std::move(detection_indices);
    if (!distance_series.empty()) {
        series.distance_to_target_mm = std::move(distance_series);
    }
    if (!heading_per_second_degrees.empty()) {
        series.heading_per_second_degrees = std::move(heading_per_second_degrees);
    }
    if (!heading_per_second_resultant.empty()) {
        series.heading_per_second_resultant = std::move(heading_per_second_resultant);
    }
    if (!heading_per_second_time_seconds.empty()) {
        series.heading_per_second_time_seconds = std::move(heading_per_second_time_seconds);
    }

    data_.movement_series.push_back(std::move(series));
    std::cout << "  " << log_tag << " Loaded run '" << run_name << "' track '" << track_id
              << "' (" << category << ", samples " << sample_count << ")"
              << std::endl;
    return true;
}

bool ZarrDetectionLoader::loadMovementCropRun(const ts::kvstore::KvStore& store,
                                              const std::string& crop_run_name) {
    std::string normalized = NormalizeCropRunName(crop_run_name);
    if (normalized.empty()) {
        return false;
    }
    if (data_.crop_data.loaded && data_.crop_data.run_name == normalized) {
        return true;
    }

    data_.crop_data = {};
    data_.crop_data.run_name = normalized;

    const std::string crop_base = "crop_runs/" + normalized + "/";
    std::cout << "  [CropRun] Resolving movement crop run at '" << crop_base << "'" << std::endl;

    std::vector<int32_t> crop_frame_indices;
    readInt32Array(store, crop_base + "frame_indices", crop_frame_indices);

    auto assignFrameIndices = [&](size_t roi_count) {
        if (crop_frame_indices.size() < roi_count) {
            crop_frame_indices.resize(roi_count, -1);
        }
        data_.crop_data.frame_indices = std::move(crop_frame_indices);
    };

    auto load_from_array = [&](auto& store_handle, int rank) -> bool {
        using StoreType = std::decay_t<decltype(store_handle)>;
        if (!store_handle.ok()) {
            return false;
        }
        auto array_result = ts::Read(store_handle.value()).result();
        if (!array_result.ok()) {
            return false;
        }
        auto array = array_result.value();
        if (array.rank() != rank) {
            return false;
        }
        size_t roi_count = static_cast<size_t>(array.shape()[0]);
        size_t height = static_cast<size_t>(array.shape()[1]);
        size_t width = static_cast<size_t>(array.shape()[2]);
        size_t channels = (rank == 4) ? static_cast<size_t>(array.shape()[3]) : 1;
        if (roi_count == 0 || height == 0 || width == 0 || channels == 0) {
            return false;
        }
        size_t total = roi_count * height * width * channels;
        data_.crop_data.images.resize(total);
        const uint8_t* src = static_cast<const uint8_t*>(array.data());
        std::copy(src, src + total, data_.crop_data.images.begin());
        data_.crop_data.roi_count = roi_count;
        data_.crop_data.height = height;
        data_.crop_data.width = width;
        data_.crop_data.channels = channels;
        assignFrameIndices(roi_count);
        data_.crop_data.loaded = true;
        return true;
    };

    auto store4 = openArrayAny<uint8_t, 4>(store, crop_base + "roi_images", context_);
    if (!load_from_array(store4, 4)) {
        auto store3 = openArrayAny<uint8_t, 3>(store, crop_base + "roi_images", context_);
        load_from_array(store3, 3);
    }

    if (!data_.crop_data.loaded) {
        data_.crop_data = {};
        return false;
    }

    if (kChaserDebugLoggingEnabled) {
        std::cout << "  [CropRun] Loaded '" << normalized << "' (roi_count="
                  << data_.crop_data.roi_count << ", size="
                  << data_.crop_data.height << "x" << data_.crop_data.width
                  << ", channels=" << data_.crop_data.channels << ")" << std::endl;
    }
    return true;
}

void ZarrDetectionLoader::finalizeMovementSelection() {
    if (data_.movement_series.empty()) {
        data_.movement_selected_index = std::numeric_limits<size_t>::max();
        data_.has_movement_data = false;
        return;
    }

    auto scoreFor = [](const std::string& value) -> int {
        std::string lower;
        lower.resize(value.size());
        std::transform(value.begin(), value.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower == "refined") return 5;
        if (lower == "offline") return 4;
        if (lower == "online_refined") return 3;
        if (lower == "online") return 2;
        if (lower == "speed_run") return 1;
        return 0;
    };

    size_t best_index = 0;
    int best_score = std::numeric_limits<int>::min();
    for (size_t i = 0; i < data_.movement_series.size(); ++i) {
        const auto& series = data_.movement_series[i];
        int score = std::max(scoreFor(series.category), scoreFor(series.detection_variant));
        if (score > best_score) {
            best_score = score;
            best_index = i;
        }
    }

    data_.movement_selected_index = best_index;
    data_.has_movement_data = true;
}

size_t ZarrDetectionLoader::getMovementSeriesCount() const {
    return data_.movement_series.size();
}

const ZarrDetectionData::MovementSeries*
ZarrDetectionLoader::getMovementSeries(size_t index) const {
    if (index < data_.movement_series.size()) {
        return &data_.movement_series[index];
    }
    return nullptr;
}

size_t ZarrDetectionLoader::getSelectedMovementSeriesIndex() const {
    return data_.movement_selected_index;
}

const ZarrDetectionData::MovementSeries*
ZarrDetectionLoader::getSelectedMovementSeries() const {
    size_t index = data_.movement_selected_index;
    if (index < data_.movement_series.size()) {
        return &data_.movement_series[index];
    }
    return nullptr;
}

bool ZarrDetectionLoader::selectMovementSeries(size_t index) {
    if (index >= data_.movement_series.size()) {
        return false;
    }
    data_.movement_selected_index = index;
    data_.has_movement_data = true;
    return true;
}
