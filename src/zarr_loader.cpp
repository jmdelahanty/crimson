#include "zarr_loader_internal.h"
#include <iostream>

using json = nlohmann::json;

ZarrDetectionLoader::ZarrDetectionLoader() {
    // Initialize TensorStore context with default settings
    context_ = ts::Context::Default();
}

ZarrDetectionLoader::~ZarrDetectionLoader() {
    // TensorStore handles cleanup automatically
}

bool ZarrDetectionLoader::loadZarrFile(const std::string& filepath,
                                       std::string& error_message) {
    try {
        // Clear any previous data
        data_ = ZarrDetectionData();
        active_dataset_ = DetectionDataset::RawDetect;
        
        std::cout << "Opening Zarr store: " << filepath << std::endl;

        // Validate filepath exists
        if (!std::filesystem::exists(filepath)) {
            error_message = "Zarr file/directory does not exist: " + filepath;
            return false;
        }

        root_path_ = filepath;

        // Open the kvstore
        const std::string kvstore_path = normalizeKvstoreFileRootPath(filepath);
        auto spec_result = ts::kvstore::Spec::FromJson({
            {"driver", "file"},
            {"path", kvstore_path}
        });
        
        if (!spec_result.ok()) {
            error_message = "Failed to create kvstore spec: " + spec_result.status().ToString();
            return false;
        }

        auto store_future = ts::kvstore::Open(spec_result.value(), context_);
        auto store_result = store_future.result();
        
        if (!store_result.ok()) {
            error_message = "Failed to open kvstore: " + store_result.status().ToString();
            return false;
        }
        auto store = store_result.value();

        // Debug probes to help diagnose layout selection.
        {
            auto probe = ts::kvstore::Read(store, "detect_runs/zarr.json").result();
            if (probe.ok()) {
                const bool kv_has_value = probe.value().has_value();
                std::string payload;
                bool have_payload = false;

                if (kv_has_value) {
                    std::cout << "  probe detect_runs/zarr.json: FOUND" << std::endl;
                    absl::CopyCordToString(probe.value().value, &payload);
                    have_payload = true;
                } else {
                    std::filesystem::path fs_path =
                        std::filesystem::path(filepath) / "detect_runs" / "zarr.json";
                    if (std::filesystem::exists(fs_path)) {
                        std::cout << "  probe detect_runs/zarr.json: FOUND via filesystem fallback" << std::endl;
                        std::ifstream file(fs_path);
                        if (file) {
                            payload.assign((std::istreambuf_iterator<char>(file)),
                                           std::istreambuf_iterator<char>());
                            have_payload = !payload.empty();
                        }
                        if (!have_payload) {
                            std::cout << "    fallback read: empty payload" << std::endl;
                        }
                    } else {
                        std::cout << "  probe detect_runs/zarr.json: MISSING" << std::endl;
                    }
                }

                if (have_payload) {
                    try {
                        auto json_meta = json::parse(payload);
                        std::string latest_run = "<unset>";
                        if (json_meta.contains("attributes") &&
                            json_meta["attributes"].is_object() &&
                            json_meta["attributes"].contains("latest") &&
                            json_meta["attributes"]["latest"].is_string()) {
                            latest_run = json_meta["attributes"]["latest"].get<std::string>();
                        }
                        std::cout << "    latest=" << latest_run << std::endl;
                        if (latest_run != "<unset>") {
                            std::string base = "detect_runs/" + latest_run + "/";
                            auto fi = ts::kvstore::Read(store, base + "frame_indices/zarr.json").result();
                            auto boxes = ts::kvstore::Read(store, base + "bbox_norm_coords/zarr.json").result();
                            std::cout << "    " << base << "frame_indices/zarr.json: "
                                      << (fi.ok() && fi.value().has_value() ? "FOUND" : "MISSING") << std::endl;
                            std::cout << "    " << base << "bbox_norm_coords/zarr.json: "
                                      << (boxes.ok() && boxes.value().has_value() ? "FOUND" : "MISSING") << std::endl;
                        }
                    } catch (const std::exception& e) {
                        std::cout << "    parse error: " << e.what() << std::endl;
                    }
                }
            } else {
                std::cout << "  probe detect_runs/zarr.json: ERROR "
                          << probe.status().ToString() << std::endl;
            }
        }

        bool loaded_palette_layout = false;

        try {
            loaded_palette_layout = loadDetectionRuns(store);
        } catch (const std::exception& palette_error) {
            error_message = std::string("Failed to load detect_runs layout: ") +
                            palette_error.what();
            return false;
        }

        if (loaded_palette_layout) {
            if (!loadRefinedDetectionsAsPrimary(store)) {
                std::cout << "  Refined detect runs not found or could not be loaded; "
                          << "using base detect_runs data" << std::endl;
            }

            if (loadKeypointHeadingData(store)) {
                std::cout << "  Loaded keypoint headings from '"
                          << data_.keypoints_run_name << "'" << std::endl;
            } else {
                std::cout << "  No keypoint heading data available" << std::endl;
            }
        } else {
            std::cout << "  detect_runs layout not found; opening in metadata/stimulus-only mode"
                      << std::endl;
        }

        // Populate metadata; continue even if it fails (broken archives may lack it)
        if (!loadMetadata(store)) {
            std::cout << "  Warning: Could not read raw_video metadata" << std::endl;
        }
        
        // Validate we have essential data
        if (data_.total_frames == 0) {
            error_message = "Zarr file has 0 frames";
            return false;
        }
        
        // Try to load interpolation runs from the correct, known location
        if (loadInterpolationRuns(store)) {
            std::cout << "  Successfully loaded interpolation data" << std::endl;
            
            // Validate interpolation data matches main data dimensions
            if (data_.has_interpolation) {
                if (!data_.latest_interpolation.uses_palette_layout &&
                    data_.latest_interpolation.bboxes_store.valid()) {
                    auto interp_domain = data_.latest_interpolation.bboxes_store.domain();
                    if (interp_domain.rank() > 0 &&
                        interp_domain.shape()[0] != data_.total_frames) {
                        std::cerr << "  Warning: Interpolation frame count mismatch. "
                                  << "Expected " << data_.total_frames 
                                  << " but got " << interp_domain.shape()[0] << std::endl;
                        data_.has_interpolation = false;
                    }
                } else if (data_.latest_interpolation.uses_palette_layout &&
                           !data_.latest_interpolation.frame_offsets.empty()) {
                    size_t palette_frames =
                        data_.latest_interpolation.frame_offsets.size() > 0
                            ? data_.latest_interpolation.frame_offsets.size() - 1
                            : 0;
                    if (palette_frames != data_.total_frames) {
                        std::cerr << "  Warning: Interpolation frame count mismatch. "
                                  << "Expected " << data_.total_frames
                                  << " but got " << palette_frames << std::endl;
                        data_.has_interpolation = false;
                    }
                }
            }
            computeActiveDatasetInterpolationFlags();
        } else {
            std::cout << "  No interpolation data available in 'interpolation_runs'" << std::endl;
        }

        if (loadMovementData(store)) {
            size_t dataset_count = getMovementSeriesCount();
            size_t selected_index = getSelectedMovementSeriesIndex();
            const auto* series = getMovementSeries(selected_index);
            if (series) {
                std::cout << "  Loaded " << dataset_count << " movement dataset"
                          << (dataset_count == 1 ? "" : "s") << "; default '"
                          << series->category << "/" << series->run_name
                          << "' (track " << series->track_id << ")" << std::endl;
            } else {
                std::cout << "  Loaded movement analysis data" << std::endl;
            }
        } else {
            std::cout << "  No movement analysis data available" << std::endl;
        }

        // Standalone crop loading fallback: if movement loading didn't
        // populate crop data, try to discover and load a crop run directly.
        if (!data_.crop_data.loaded) {
            std::string crop_candidate;
            if (!data_.keypoints_source_crop_run.empty()) {
                crop_candidate = NormalizeCropRunName(data_.keypoints_source_crop_run);
            }
            if (crop_candidate.empty()) {
                if (auto crop_group_attrs = readAttrsAny(store, "crop_runs")) {
                    crop_candidate = NormalizeCropRunName(
                        extractLatestRunName(*crop_group_attrs));
                }
            }
            if (crop_candidate.empty() && !root_path_.empty()) {
                auto fs_runs = collect_runs_fs(root_path_, "crop_runs",
                                               {"roi_images"});
                if (!fs_runs.empty()) {
                    crop_candidate = NormalizeCropRunName(fs_runs.front());
                }
            }
            if (!crop_candidate.empty()) {
                if (loadMovementCropRun(store, crop_candidate)) {
                    std::cout << "  Loaded crop run '" << crop_candidate
                              << "' (" << data_.crop_data.roi_count << " ROIs, "
                              << data_.crop_data.height << "x"
                              << data_.crop_data.width << ")" << std::endl;
                }
            }
        }

        std::cout << "Successfully loaded zarr file: " << filepath << std::endl;
        std::cout << "  Total frames: " << data_.total_frames << std::endl;
        std::cout << "  Max detections per frame: " << data_.max_detections << std::endl;
        std::cout << "  FPS: " << data_.fps << std::endl;
        std::string layout_name = "Metadata/stimulus-only (no detect_runs)";
        if (data_.layout == ZarrLayoutType::kPaletteRuns) {
            layout_name = "Palette detection_runs";
        } else if (data_.layout == ZarrLayoutType::kLegacyGrid) {
            layout_name = "Legacy dense arrays";
        }
        std::cout << "  Layout: " << layout_name << std::endl;
        if (!data_.detect_run_name.empty()) {
            std::cout << "  Detect run: " << data_.detect_run_name << std::endl;
        }
        if (data_.image_width > 0 && data_.image_height > 0) {
            std::cout << "  Resolution: " << data_.image_width << "x"
                      << data_.image_height << std::endl;
        }
        std::cout << "  Has scores: " << (data_.has_scores ? "Yes" : "No") << std::endl;
        std::cout << "  Has class IDs: " << (data_.has_class_ids ? "Yes" : "No") << std::endl;
        std::cout << "  Has interpolation: " << (data_.has_interpolation ? "Yes" : "No") << std::endl;
        
        if (data_.has_interpolation) {
            std::cout << "  Interpolation method: " << data_.latest_interpolation.method << std::endl;
            std::cout << "  Interpolation created: " << data_.latest_interpolation.created_at << std::endl;
        }
        
        return true;
        
    } catch (const std::exception& e) {
        error_message = std::string("Exception loading zarr: ") + e.what();
        // Clear any partially loaded data
        data_ = ZarrDetectionData();
        active_dataset_ = DetectionDataset::RawDetect;
        return false;
    }
}

bool ZarrDetectionLoader::readInt32Array(const ts::kvstore::KvStore& store,
                                        const std::string& path,
                                        std::vector<int32_t>& out) {
    try {
        std::vector<std::string> failure_messages;
        auto attempt = [&](auto type_token, const char* label) -> bool {
            using Source = decltype(type_token);
            auto open_result = openArrayAny<Source, 1>(store, path, context_);
            if (!open_result.ok()) {
                std::ostringstream oss;
                oss << "open as " << label << " failed: " << open_result.status();
                failure_messages.push_back(oss.str());
                return false;
            }

            auto array_result = ts::Read(open_result.value()).result();
            if (!array_result.ok()) {
                std::ostringstream oss;
                oss << "read as " << label << " failed: " << array_result.status();
                failure_messages.push_back(oss.str());
                return false;
            }

            auto array = array_result.value();
            if (array.rank() != 1) {
                std::ostringstream oss;
                oss << "rank mismatch for " << label << " (expected 1, got " << array.rank() << ")";
                failure_messages.push_back(oss.str());
                return false;
            }

            size_t length = static_cast<size_t>(array.shape()[0]);
            out.resize(length);
            const auto* data = static_cast<const Source*>(array.data());
            bool overflow = false;
            for (size_t i = 0; i < length; ++i) {
                out[i] = convertToInt32WithClamp<Source>(data[i], overflow);
            }
        if (kChaserDebugLoggingEnabled) {
            std::cout << "  [ReadInt32Array] '" << path << "' read as " << label
                      << " (" << length << " rows"
                      << (overflow ? ", overflow clamp applied" : "") << ")"
                      << std::endl;
        }
        return true;
        };

        if (attempt(int32_t{}, "int32_t") ||
            attempt(uint32_t{}, "uint32_t") ||
            attempt(int64_t{}, "int64_t") ||
            attempt(uint64_t{}, "uint64_t") ||
            attempt(int16_t{}, "int16_t") ||
            attempt(uint16_t{}, "uint16_t") ||
            attempt(int8_t{}, "int8_t") ||
            attempt(uint8_t{}, "uint8_t")) {
            return true;
        }

        if (kChaserDebugLoggingEnabled) {
            std::cout << "  [ReadInt32Array] '" << path << "' failed after dtype attempts:"
                      << std::endl;
            for (const auto& msg : failure_messages) {
                std::cout << "    - " << msg << std::endl;
            }
        }
        return false;
    } catch (const std::exception& e) {
        std::cerr << "Error reading int32 array at " << path << ": " << e.what() << std::endl;
        return false;
    }
}

bool ZarrDetectionLoader::readInt64Array(const ts::kvstore::KvStore& store,
                                        const std::string& path,
                                        std::vector<int64_t>& out) {
    try {
        std::vector<std::string> failure_messages;
        auto attempt = [&](auto type_token, const char* label) -> bool {
            using Source = decltype(type_token);
            auto open_result = openArrayAny<Source, 1>(store, path, context_);
            if (!open_result.ok()) {
                std::ostringstream oss;
                oss << "open as " << label << " failed: " << open_result.status();
                failure_messages.push_back(oss.str());
                return false;
            }

            auto array_result = ts::Read(open_result.value()).result();
            if (!array_result.ok()) {
                std::ostringstream oss;
                oss << "read as " << label << " failed: " << array_result.status();
                failure_messages.push_back(oss.str());
                return false;
            }

            auto array = array_result.value();
            if (array.rank() != 1) {
                std::ostringstream oss;
                oss << "rank mismatch for " << label << " (expected 1, got " << array.rank() << ")";
                failure_messages.push_back(oss.str());
                return false;
            }

            size_t length = static_cast<size_t>(array.shape()[0]);
            out.resize(length);
            const auto* data = static_cast<const Source*>(array.data());
            bool overflow = false;
            for (size_t i = 0; i < length; ++i) {
                out[i] = convertToInt64WithClamp<Source>(data[i], overflow);
            }
        if (kChaserDebugLoggingEnabled) {
            std::cout << "  [ReadInt64Array] '" << path << "' read as " << label
                      << " (" << length << " rows"
                      << (overflow ? ", overflow clamp applied" : "") << ")"
                      << std::endl;
        }
            return true;
        };

        if (attempt(int64_t{}, "int64_t") ||
            attempt(uint64_t{}, "uint64_t") ||
            attempt(int32_t{}, "int32_t") ||
            attempt(uint32_t{}, "uint32_t") ||
            attempt(int16_t{}, "int16_t") ||
            attempt(uint16_t{}, "uint16_t") ||
            attempt(int8_t{}, "int8_t") ||
            attempt(uint8_t{}, "uint8_t")) {
            return true;
        }

        if (kChaserDebugLoggingEnabled) {
            std::cout << "  [ReadInt64Array] '" << path << "' failed after dtype attempts:"
                      << std::endl;
            for (const auto& msg : failure_messages) {
                std::cout << "    - " << msg << std::endl;
            }
        }
        return false;
    } catch (const std::exception& e) {
        std::cerr << "Error reading int64 array at " << path << ": " << e.what() << std::endl;
        return false;
    }
}

bool ZarrDetectionLoader::readFloatArray(const ts::kvstore::KvStore& store,
                                        const std::string& path,
                                        std::vector<float>& out) {
    try {
        auto open_result = openArrayAny<float, 1>(store, path, context_);

        if (open_result.ok()) {
            auto array_result = ts::Read(open_result.value()).result();
            if (!array_result.ok()) {
                return false;
            }
            auto array = array_result.value();
            if (array.rank() != 1) {
                return false;
            }
            size_t length = static_cast<size_t>(array.shape()[0]);
            out.resize(length);
            const auto* data = static_cast<const float*>(array.data());
            std::copy(data, data + length, out.begin());
            return true;
        }

        // Fallback for float64 datasets
        auto open_double = openArrayAny<double, 1>(store, path, context_);

        if (!open_double.ok()) {
            return false;
        }

        auto array_double = ts::Read(open_double.value()).result();
        if (!array_double.ok()) {
            return false;
        }

        auto array = array_double.value();
        if (array.rank() != 1) {
            return false;
        }

        size_t length = static_cast<size_t>(array.shape()[0]);
        out.resize(length);
        const auto* data = static_cast<const double*>(array.data());
        for (size_t i = 0; i < length; ++i) {
            out[i] = static_cast<float>(data[i]);
        }
        return true;

    } catch (const std::exception& e) {
        std::cerr << "Error reading float array at " << path << ": " << e.what() << std::endl;
        return false;
    }
}

bool ZarrDetectionLoader::readFloatMatrix(const ts::kvstore::KvStore& store,
                                         const std::string& path,
                                         std::vector<std::array<float, 4>>& out) {
    try {
        auto open_result = openArrayAny<float, 2>(store, path, context_);

        if (open_result.ok()) {
            auto array_result = ts::Read(open_result.value()).result();
            if (!array_result.ok()) {
                return false;
            }
            auto array = array_result.value();
            if (array.rank() != 2 || array.shape()[1] != 4) {
                return false;
            }

            size_t rows = static_cast<size_t>(array.shape()[0]);
            out.resize(rows);
            const auto* data = static_cast<const float*>(array.data());
            for (size_t row = 0; row < rows; ++row) {
                out[row][0] = data[row * 4 + 0];
                out[row][1] = data[row * 4 + 1];
                out[row][2] = data[row * 4 + 2];
                out[row][3] = data[row * 4 + 3];
            }
            return true;
        }

        // Fallback for float64 datasets
        auto open_double = openArrayAny<double, 2>(store, path, context_);

        if (!open_double.ok()) {
            return false;
        }

        auto array_result = ts::Read(open_double.value()).result();
        if (!array_result.ok()) {
            return false;
        }

        auto array = array_result.value();
        if (array.rank() != 2 || array.shape()[1] != 4) {
            return false;
        }

        size_t rows = static_cast<size_t>(array.shape()[0]);
        out.resize(rows);
        const auto* data = static_cast<const double*>(array.data());
        for (size_t row = 0; row < rows; ++row) {
            out[row][0] = static_cast<float>(data[row * 4 + 0]);
            out[row][1] = static_cast<float>(data[row * 4 + 1]);
            out[row][2] = static_cast<float>(data[row * 4 + 2]);
            out[row][3] = static_cast<float>(data[row * 4 + 3]);
        }
        return true;

    } catch (const std::exception& e) {
        std::cerr << "Error reading float matrix at " << path << ": " << e.what() << std::endl;
        return false;
    }
}

bool ZarrDetectionLoader::readBoolArray(const ts::kvstore::KvStore& store,
                                       const std::string& path,
                                       std::vector<uint8_t>& out) {
    try {
        auto open_result = openArrayAny<bool, 1>(store, path, context_);

        if (!open_result.ok()) {
            return false;
        }

        auto array_result = ts::Read(open_result.value()).result();
        if (!array_result.ok()) {
            return false;
        }

        auto array = array_result.value();
        if (array.rank() != 1) {
            return false;
        }

        size_t length = static_cast<size_t>(array.shape()[0]);
        out.resize(length);
        const auto* data = static_cast<const bool*>(array.data());
        for (size_t i = 0; i < length; ++i) {
            out[i] = data[i] ? 1 : 0;
        }
        return true;

    } catch (const std::exception& e) {
        std::cerr << "Error reading bool array at " << path << ": " << e.what() << std::endl;
        return false;
    }
}

bool ZarrDetectionLoader::readStringArray(const ts::kvstore::KvStore& store,
                                         const std::string& path,
                                         std::vector<std::string>& out) {
    try {
        static std::unordered_set<std::string> unsupported_string_dtype_paths;
        if (auto node_meta = readNodeMetaV3(store, path);
            node_meta.has_value() && usesZarrV3StringDataType(*node_meta)) {
            if (unsupported_string_dtype_paths.insert(path).second) {
                std::cout
                    << "  [ReadStringArray] '" << path
                    << "' uses Zarr v3 string dtype unsupported by current TensorStore build;"
                    << " skipping decode and using fallback provenance."
                    << std::endl;
            }
            return false;
        }

        std::vector<std::string> failure_messages;

        auto read_as_strings = [&]() -> bool {
            auto open_result = openArrayAny<std::string, 1>(store, path, context_);
            if (!open_result.ok()) {
                std::ostringstream oss;
                oss << "open as std::string failed: " << open_result.status();
                failure_messages.push_back(oss.str());
                return false;
            }

            auto array_result = ts::Read(open_result.value()).result();
            if (!array_result.ok()) {
                std::ostringstream oss;
                oss << "read as std::string failed: " << array_result.status();
                failure_messages.push_back(oss.str());
                return false;
            }

            auto array = array_result.value();
            if (array.rank() != 1) {
                std::ostringstream oss;
                oss << "rank mismatch for std::string (expected 1, got " << array.rank() << ")";
                failure_messages.push_back(oss.str());
                return false;
            }

            size_t length = static_cast<size_t>(array.shape()[0]);
            out.resize(length);
            for (size_t i = 0; i < length; ++i) {
                out[i] = array(static_cast<ts::Index>(i));
            }
            std::cout << "  [ReadStringArray] '" << path << "' read as std::string ("
                      << length << " rows)" << std::endl;
            return true;
        };

        auto read_as_bytes2d = [&](auto type_token, const char* label) -> bool {
            using Element = decltype(type_token);
            auto open_result = openArrayAny<Element, 2>(store, path, context_);
            if (!open_result.ok()) {
                std::ostringstream oss;
                oss << "open as " << label << " failed: " << open_result.status();
                failure_messages.push_back(oss.str());
                return false;
            }

            auto array_result = ts::Read(open_result.value()).result();
            if (!array_result.ok()) {
                std::ostringstream oss;
                oss << "read as " << label << " failed: " << array_result.status();
                failure_messages.push_back(oss.str());
                return false;
            }

            auto array = array_result.value();
            if (array.rank() != 2) {
                std::ostringstream oss;
                oss << "rank mismatch for " << label << " (expected 2, got " << array.rank() << ")";
                failure_messages.push_back(oss.str());
                return false;
            }

            size_t rows = static_cast<size_t>(array.shape()[0]);
            size_t width = static_cast<size_t>(array.shape()[1]);
            out.resize(rows);
            const auto* data = static_cast<const Element*>(array.data());
            for (size_t row = 0; row < rows; ++row) {
                const auto* row_ptr = data + row * width;
                size_t length = 0;
                while (length < width && row_ptr[length] != static_cast<Element>(0)) {
                    ++length;
                }
                out[row] = std::string(reinterpret_cast<const char*>(row_ptr),
                                       reinterpret_cast<const char*>(row_ptr + length));
            }
            std::cout << "  [ReadStringArray] '" << path << "' read as " << label
                      << " (" << rows << " rows, width " << width << ")" << std::endl;
            return true;
        };

        if (read_as_strings() ||
            read_as_bytes2d(uint8_t{}, "uint8_t[rows, width]") ||
            read_as_bytes2d(char{}, "char[rows, width]")) {
            return true;
        }

        std::cout << "  [ReadStringArray] '" << path << "' failed after dtype attempts:"
                  << std::endl;
        for (const auto& msg : failure_messages) {
            std::cout << "    - " << msg << std::endl;
        }
        return false;

    } catch (const std::exception& e) {
        std::cerr << "Error reading string array at " << path << ": " << e.what() << std::endl;
        return false;
    }
}

bool ZarrDetectionLoader::loadPaletteInterpolationRun(const ts::kvstore::KvStore& store,
                                                       const std::string& run_name,
                                                       const std::string& subgroup) {
    try {
        data_.latest_interpolation = InterpolationRunData();
        auto& interp = data_.latest_interpolation;
        interp.run_name = run_name;
        interp.method = subgroup;
        interp.uses_palette_layout = true;
        interp.has_flat_detections = true;

        // Try to read run-level metadata
        std::string run_prefix = "refined_runs/" + run_name;
        if (auto run_attrs = readAttrsAny(store, run_prefix)) {
            if (run_attrs->contains("created_at") && (*run_attrs)["created_at"].is_string()) {
                interp.created_at = (*run_attrs)["created_at"].get<std::string>();
            }
            if (run_attrs->contains("completed_at_utc") && (*run_attrs)["completed_at_utc"].is_string()) {
                interp.created_at = (*run_attrs)["completed_at_utc"].get<std::string>();
            }
            if (run_attrs->contains("method") && (*run_attrs)["method"].is_string()) {
                interp.method = (*run_attrs)["method"].get<std::string>();
            }
        }

        std::string base_path = run_prefix + "/" + subgroup + "/";
        if (auto subgroup_attrs = readAttrsAny(store, base_path)) {
            if (subgroup_attrs->contains("created_at") && (*subgroup_attrs)["created_at"].is_string()) {
                interp.created_at = (*subgroup_attrs)["created_at"].get<std::string>();
            }
            if (subgroup_attrs->contains("method") && (*subgroup_attrs)["method"].is_string()) {
                interp.method = (*subgroup_attrs)["method"].get<std::string>();
            }
        }

        if (!readInt32Array(store, base_path + "frame_indices", interp.frame_indices)) {
            std::cerr << "  Failed to read refined frame_indices from '" << subgroup << "'" << std::endl;
            return false;
        }

        if (!readFloatMatrix(store, base_path + "bbox_norm_coords", interp.bbox_norm_coords)) {
            std::cerr << "  Failed to read refined bbox_norm_coords from '" << subgroup << "'" << std::endl;
            return false;
        }

        if (interp.frame_indices.size() != interp.bbox_norm_coords.size()) {
            std::cerr << "  Refined run mismatch between frame_indices (" << interp.frame_indices.size()
                      << ") and bbox_norm_coords (" << interp.bbox_norm_coords.size() << ")" << std::endl;
            return false;
        }

        std::vector<float> scores;
        if (readFloatArray(store, base_path + "scores", scores)) {
            if (scores.size() == interp.frame_indices.size()) {
                interp.flat_scores = std::move(scores);
            } else {
                std::cerr << "  Warning: refined scores length mismatch; ignoring scores" << std::endl;
            }
        }

        std::vector<int32_t> class_ids;
        if (readInt32Array(store, base_path + "class_ids", class_ids)) {
            if (class_ids.size() == interp.frame_indices.size()) {
                interp.flat_class_ids = std::move(class_ids);
            } else {
                std::cerr << "  Warning: refined class_ids length mismatch; ignoring class IDs" << std::endl;
            }
        }

        std::vector<int32_t> detection_source_raw;
        if (readInt32Array(store, base_path + "detection_source", detection_source_raw)) {
            if (detection_source_raw.size() == interp.frame_indices.size()) {
                interp.detection_source.resize(detection_source_raw.size());
                for (size_t i = 0; i < detection_source_raw.size(); ++i) {
                    interp.detection_source[i] = detection_source_raw[i] != 0 ? 1 : 0;
                }
            } else {
                std::cerr << "  Warning: detection_source length mismatch; ignoring" << std::endl;
            }
        }

        std::vector<uint8_t> mask_raw;
        readBoolArray(store, base_path + "interpolation_mask", mask_raw);

        std::vector<int32_t> n_detections_disk;
        readInt32Array(store, base_path + "n_detections", n_detections_disk);

        int32_t max_frame_index = -1;
        for (const auto frame_index : interp.frame_indices) {
            if (frame_index > max_frame_index) {
                max_frame_index = frame_index;
            }
        }

        size_t resolved_frames = data_.total_frames;
        if (!n_detections_disk.empty()) {
            resolved_frames = std::max(resolved_frames, static_cast<size_t>(n_detections_disk.size()));
        }
        if (max_frame_index >= 0) {
            resolved_frames = std::max(resolved_frames, static_cast<size_t>(max_frame_index) + 1);
        }

        if (resolved_frames == 0) {
            std::cerr << "  Could not determine frame count for refined detections" << std::endl;
            return false;
        }

        interp.frame_offsets.assign(resolved_frames + 1, 0);
        for (const auto frame_index : interp.frame_indices) {
            if (frame_index < 0) {
                continue;
            }
            size_t frame = static_cast<size_t>(frame_index);
            if (frame + 1 >= interp.frame_offsets.size()) {
                interp.frame_offsets.resize(frame + 2, 0);
            }
            interp.frame_offsets[frame + 1]++;
        }
        for (size_t i = 0; i + 1 < interp.frame_offsets.size(); ++i) {
            interp.frame_offsets[i + 1] += interp.frame_offsets[i];
        }

        size_t total_detections = interp.frame_indices.size();
        std::vector<std::array<float, 4>> sorted_boxes(total_detections);
        std::vector<float> sorted_scores(!interp.flat_scores.empty() ? total_detections : 0);
        std::vector<int32_t> sorted_class_ids(!interp.flat_class_ids.empty() ? total_detections : 0);
        std::vector<uint8_t> sorted_detection_source(!interp.detection_source.empty() ? total_detections : 0);
        std::vector<uint8_t> sorted_mask_detection(
            (!mask_raw.empty() && mask_raw.size() == total_detections) ? total_detections : 0);
        std::vector<int32_t> sorted_frame_indices(total_detections, 0);

        std::vector<size_t> write_cursor(interp.frame_offsets.size() > 0
            ? interp.frame_offsets.size() - 1
            : 0, 0);
        for (size_t frame = 0; frame < write_cursor.size(); ++frame) {
            write_cursor[frame] = interp.frame_offsets[frame];
        }

        for (size_t det = 0; det < total_detections; ++det) {
            int32_t frame_index = interp.frame_indices[det];
            if (frame_index < 0) {
                continue;
            }
            size_t frame = static_cast<size_t>(frame_index);
            if (frame >= write_cursor.size()) {
                continue;
            }
            size_t dest = write_cursor[frame]++;
            if (dest >= sorted_boxes.size()) {
                continue;
            }
            sorted_boxes[dest] = interp.bbox_norm_coords[det];
            sorted_frame_indices[dest] = frame_index;

            if (!interp.flat_scores.empty()) {
                sorted_scores[dest] = interp.flat_scores[det];
            }
            if (!interp.flat_class_ids.empty()) {
                sorted_class_ids[dest] = interp.flat_class_ids[det];
            }
            if (!interp.detection_source.empty()) {
                sorted_detection_source[dest] = interp.detection_source[det];
            }
            if (!sorted_mask_detection.empty()) {
                sorted_mask_detection[dest] = mask_raw[det] ? 1 : 0;
            }
        }

        interp.bbox_norm_coords = std::move(sorted_boxes);
        interp.frame_indices = std::move(sorted_frame_indices);
        if (!interp.flat_scores.empty()) {
            interp.flat_scores = std::move(sorted_scores);
        }
        if (!interp.flat_class_ids.empty()) {
            interp.flat_class_ids = std::move(sorted_class_ids);
        }
        if (!interp.detection_source.empty()) {
            interp.detection_source = std::move(sorted_detection_source);
        }

        interp.frame_mask.assign(
            interp.frame_offsets.size() > 0 ? interp.frame_offsets.size() - 1 : 0,
            0
        );

        if (!interp.detection_source.empty()) {
            for (size_t frame = 0; frame + 1 < interp.frame_offsets.size(); ++frame) {
                size_t start = interp.frame_offsets[frame];
                size_t end = interp.frame_offsets[frame + 1];
                for (size_t idx = start; idx < end && idx < interp.detection_source.size(); ++idx) {
                    if (interp.detection_source[idx] != 0) {
                        interp.frame_mask[frame] = 1;
                        break;
                    }
                }
            }
        } else if (!sorted_mask_detection.empty()) {
            for (size_t frame = 0; frame + 1 < interp.frame_offsets.size(); ++frame) {
                size_t start = interp.frame_offsets[frame];
                size_t end = interp.frame_offsets[frame + 1];
                for (size_t idx = start; idx < end && idx < sorted_mask_detection.size(); ++idx) {
                    if (sorted_mask_detection[idx] != 0) {
                        interp.frame_mask[frame] = 1;
                        break;
                    }
                }
            }
        } else if (!mask_raw.empty() && mask_raw.size() == interp.frame_mask.size()) {
            interp.frame_mask.assign(
                mask_raw.begin(),
                mask_raw.begin() + interp.frame_mask.size()
            );
        }

        interp.is_loaded = true;
        data_.has_interpolation = true;

        std::cout << "  Loaded refined run '" << run_name
                  << "' subgroup '" << subgroup << "' with "
                  << total_detections << " detections" << std::endl;

        return true;

    } catch (const std::exception& e) {
        std::cerr << "  Error loading refined interpolation run: " << e.what() << std::endl;
        return false;
    }
}

bool ZarrDetectionLoader::loadDetectionRuns(const ts::kvstore::KvStore& store) {
    const std::string kPaletteGroup = "detect_runs";

    if (loadDetectionRunFromGroup(store, kPaletteGroup)) {
        data_.layout = ZarrLayoutType::kPaletteRuns;
        data_.coordinates_normalized = true;
        return true;
    }
    return false;
}

bool ZarrDetectionLoader::loadDetectionRunFromGroup(
    const ts::kvstore::KvStore& store,
    const std::string& group_path) {

    std::string latest_run;
    if (auto group_attrs = readAttrsAny(store, group_path)) {
        latest_run = extractLatestRunName(*group_attrs);
    }

    if (latest_run.empty()) {
        if (root_path_.empty()) {
            return false;
        }
        auto candidates = collect_runs_fs(
            root_path_,
            group_path,
            {"frame_indices", "bbox_norm_coords"});
        candidates.erase(
            std::remove_if(
                candidates.begin(),
                candidates.end(),
                [](const std::string& name) {
                    return name.rfind("detect_", 0) != 0 &&
                           name.rfind("detection_", 0) != 0;
                }),
            candidates.end());
        if (candidates.empty()) {
            return false;
        }
        latest_run = candidates.back();
    }

    std::string base_path = group_path + "/" + latest_run + "/";
    std::cout << "  Loading detection run '" << latest_run
              << "' from " << group_path << std::endl;

    size_t resolved_frames = data_.total_frames;
    std::vector<int32_t> frame_indices;
    std::vector<std::array<float, 4>> boxes;
    std::vector<float> scores;
    std::vector<int32_t> class_ids;
    std::vector<int32_t> n_detections;
    std::vector<size_t> frame_offsets;
    std::vector<uint8_t> detection_source;
    bool has_scores = false;
    bool has_class_ids = false;

    if (!loadFlattenedRun(store,
                          base_path,
                          &frame_indices,
                          boxes,
                          scores,
                          class_ids,
                          n_detections,
                          frame_offsets,
                          nullptr,
                          nullptr,
                          has_scores,
                          has_class_ids,
                          resolved_frames)) {
        return false;
    }

    data_.frame_indices = std::move(frame_indices);
    data_.bbox_norm_coords = std::move(boxes);
    data_.flat_scores = std::move(scores);
    data_.flat_class_ids = std::move(class_ids);
    data_.frame_offsets = std::move(frame_offsets);
    data_.n_detections = std::move(n_detections);
    data_.has_scores = has_scores;
    data_.has_class_ids = has_class_ids;
    data_.coordinates_normalized = true;
    data_.detection_source_flags.clear();
    data_.detection_reason_flags.clear();

    if (data_.n_detections.empty()) {
        computeDetectionsFromOffsets(data_.frame_offsets, data_.n_detections);
    }

    data_.total_frames = data_.n_detections.size();
    if (data_.total_frames == 0 && data_.frame_offsets.size() > 1) {
        data_.total_frames = data_.frame_offsets.size() - 1;
    }

    data_.max_detections = 0;
    if (!data_.frame_offsets.empty()) {
        for (size_t frame = 0; frame + 1 < data_.frame_offsets.size(); ++frame) {
            size_t count = data_.frame_offsets[frame + 1] - data_.frame_offsets[frame];
            data_.max_detections = std::max(data_.max_detections, count);
        }
    }
    for (const auto count : data_.n_detections) {
        if (count >= 0) {
            data_.max_detections = std::max(
                data_.max_detections,
                static_cast<size_t>(count)
            );
        }
    }

    data_.detect_run_name = latest_run;

    if (auto attrs = readAttrsAny(store, base_path)) {
        populateDetectionMetadata(*attrs, data_);
    }

    InterpolationRunData raw_stage;
    raw_stage.run_name = latest_run;
    raw_stage.stage_label.clear();
    raw_stage.method = data_.detect_run_method.empty() ? "detect_runs" : data_.detect_run_method;
    raw_stage.created_at = data_.detect_run_created_at;
    raw_stage.source_detection_run = data_.detect_run_source;
    raw_stage.provenance_json = data_.detect_run_provenance_json;
    raw_stage.uses_palette_layout = true;
    raw_stage.has_flat_detections = true;
    raw_stage.frame_indices = data_.frame_indices;
    raw_stage.bbox_norm_coords = data_.bbox_norm_coords;
    raw_stage.flat_scores = data_.flat_scores;
    raw_stage.flat_class_ids = data_.flat_class_ids;
    raw_stage.frame_offsets = data_.frame_offsets;
    raw_stage.n_detections = data_.n_detections;
    raw_stage.has_scores = data_.has_scores;
    raw_stage.has_class_ids = data_.has_class_ids;
    cacheDetectionStage(std::move(raw_stage), DetectionDataset::RawDetect);
    computeActiveDatasetInterpolationFlags();
    active_dataset_ = DetectionDataset::RawDetect;

    std::cout << "  Detection run '" << data_.detect_run_name << "' loaded with "
              << data_.bbox_norm_coords.size() << " detections over "
              << data_.total_frames << " frames" << std::endl;
    return true;
}

bool ZarrDetectionLoader::loadFlattenedRun(
    const ts::kvstore::KvStore& store,
    const std::string& base_path,
    std::vector<int32_t>* frame_indices_out,
    std::vector<std::array<float, 4>>& boxes_out,
    std::vector<float>& scores_out,
    std::vector<int32_t>& class_ids_out,
    std::vector<int32_t>& n_detections_out,
    std::vector<size_t>& frame_offsets_out,
    std::vector<uint8_t>* detection_source_out,
    std::vector<std::string>* detection_reason_out,
    bool& has_scores_out,
    bool& has_class_ids_out,
    size_t& resolved_frames_out) {

    has_scores_out = false;
    has_class_ids_out = false;

    std::vector<int32_t> frame_indices_local;
    bool has_frame_indices = readInt32Array(store, base_path + "frame_indices", frame_indices_local);

    std::vector<std::array<float, 4>> boxes_local;
    if (!readFloatMatrix(store, base_path + "bbox_norm_coords", boxes_local)) {
        std::vector<std::array<float, 4>> bbox_img_xyxy;
        if (readFloatMatrix(store, base_path + "bbox_img_xyxy", bbox_img_xyxy)) {
            if (data_.image_width <= 0 || data_.image_height <= 0) {
                std::cerr << "  bbox_img_xyxy present but image dimensions unavailable under "
                          << base_path << std::endl;
                return false;
            }
            boxes_local.reserve(bbox_img_xyxy.size());
            for (const auto& box : bbox_img_xyxy) {
                const float x1 = std::clamp(box[0], 0.0f, static_cast<float>(data_.image_width));
                const float y1 = std::clamp(box[1], 0.0f, static_cast<float>(data_.image_height));
                const float x2 = std::clamp(box[2], 0.0f, static_cast<float>(data_.image_width));
                const float y2 = std::clamp(box[3], 0.0f, static_cast<float>(data_.image_height));
                const float w = std::max(0.0f, x2 - x1);
                const float h = std::max(0.0f, y2 - y1);
                const float cx = (x1 + 0.5f * w) / static_cast<float>(data_.image_width);
                const float cy = (y1 + 0.5f * h) / static_cast<float>(data_.image_height);
                boxes_local.push_back(
                    {std::clamp(cx, 0.0f, 1.0f),
                     std::clamp(cy, 0.0f, 1.0f),
                     std::clamp(w / static_cast<float>(data_.image_width), 0.0f, 1.0f),
                     std::clamp(h / static_cast<float>(data_.image_height), 0.0f, 1.0f)});
            }
        } else if (!readFloatMatrix(store, base_path + "bboxes", boxes_local)) {
            std::cerr << "  No bounding box array found under " << base_path << std::endl;
            return false;
        }
    }

    size_t total_detections = boxes_local.size();
    if (total_detections == 0) {
        std::cerr << "  No detections available under " << base_path << std::endl;
    }

    std::vector<int32_t> n_detections_disk;
    if (!readInt32Array(store, base_path + "n_detections", n_detections_disk)) {
        readInt32Array(store, base_path + "frame_counts", n_detections_disk);
    }

    if (!has_frame_indices && n_detections_disk.empty()) {
        std::cerr << "  Missing both frame_indices and n_detections for " << base_path << std::endl;
        return false;
    }

    if (has_frame_indices && frame_indices_local.size() != total_detections) {
        std::cerr << "  frame_indices length mismatch at " << base_path << std::endl;
        return false;
    }

    scores_out.clear();
    if ((readFloatArray(store, base_path + "confidence_scores", scores_out) ||
         readFloatArray(store, base_path + "scores", scores_out)) &&
        scores_out.size() == total_detections) {
        has_scores_out = true;
    } else {
        scores_out.clear();
        has_scores_out = false;
    }

    class_ids_out.clear();
    if (readInt32Array(store, base_path + "class_ids", class_ids_out) &&
        class_ids_out.size() == total_detections) {
        has_class_ids_out = true;
    } else {
        class_ids_out.clear();
        has_class_ids_out = false;
    }

    std::optional<json> instances_attrs = readAttrsAny(store, base_path);
    std::optional<json> run_attrs;
    if (!instances_attrs.has_value()) {
        std::string normalized_base = base_path;
        while (!normalized_base.empty() && normalized_base.back() == '/') {
            normalized_base.pop_back();
        }
        const size_t slash = normalized_base.find_last_of('/');
        if (slash != std::string::npos) {
            run_attrs = readAttrsAny(store, normalized_base.substr(0, slash + 1));
        }
    } else {
        std::string normalized_base = base_path;
        while (!normalized_base.empty() && normalized_base.back() == '/') {
            normalized_base.pop_back();
        }
        const size_t slash = normalized_base.find_last_of('/');
        if (slash != std::string::npos) {
            run_attrs = readAttrsAny(store, normalized_base.substr(0, slash + 1));
        }
    }

    std::unordered_map<int, std::string> source_kind_labels_by_code = {
        {0, "none"},
        {1, "raw_detect"},
        {2, "interpolated"},
        {3, "manual"},
    };
    auto load_source_kind_code_map = [&](const json& attrs) {
        if (!attrs.contains("source_kind_code_map") ||
            !attrs["source_kind_code_map"].is_object()) {
            return;
        }
        std::unordered_map<int, std::string> parsed;
        for (auto it = attrs["source_kind_code_map"].begin();
             it != attrs["source_kind_code_map"].end();
             ++it) {
            if (!it.value().is_number_integer()) {
                continue;
            }
            parsed[it.value().get<int>()] = it.key();
        }
        if (!parsed.empty()) {
            source_kind_labels_by_code = std::move(parsed);
        }
    };
    if (run_attrs.has_value()) {
        load_source_kind_code_map(*run_attrs);
    } else if (instances_attrs.has_value()) {
        load_source_kind_code_map(*instances_attrs);
    }

    std::vector<int8_t> source_kind_codes;
    bool has_source_kind_codes = false;
    if (auto source_kind_store =
            openArrayAny<int8_t, 1>(store, base_path + "source_kind_codes", context_);
        source_kind_store.ok()) {
        auto source_kind_read = ts::Read(source_kind_store.value()).result();
        if (source_kind_read.ok()) {
            auto source_kind_array = source_kind_read.value();
            const size_t count = static_cast<size_t>(source_kind_array.num_elements());
            source_kind_codes.assign(source_kind_array.data(),
                                     source_kind_array.data() + count);
            has_source_kind_codes = source_kind_codes.size() == total_detections;
        }
    }

    if (detection_reason_out) {
        detection_reason_out->clear();
        auto try_load_reason = [&](const std::string& field_name) -> bool {
            const std::string field_path = base_path + field_name;
            if (!arrayExists(store, field_path)) {
                return false;
            }
            std::vector<std::string> reason_raw;
            if (!readStringArray(store, field_path, reason_raw)) {
                return false;
            }
            if (reason_raw.size() != total_detections) {
                std::cout << "  [ReadStringArray] '" << field_path
                          << "' length mismatch (expected " << total_detections
                          << ", got " << reason_raw.size()
                          << "); ignoring field." << std::endl;
                return false;
            }
            *detection_reason_out = std::move(reason_raw);
            return true;
        };

        // Palette detect reason precedence: reason_bytes -> reason -> detection_source
        if (!try_load_reason("reason_bytes")) {
            if (!try_load_reason("reason")) {
                if (has_source_kind_codes && total_detections > 0) {
                    detection_reason_out->resize(total_detections);
                    for (size_t i = 0; i < total_detections; ++i) {
                        auto label_it = source_kind_labels_by_code.find(
                            static_cast<int>(source_kind_codes[i]));
                        (*detection_reason_out)[i] =
                            (label_it != source_kind_labels_by_code.end())
                                ? label_it->second
                                : "unknown";
                    }
                } else if (detection_source_out &&
                           detection_source_out->size() == total_detections &&
                           total_detections > 0) {
                    detection_reason_out->resize(total_detections);
                    for (size_t i = 0; i < total_detections; ++i) {
                        (*detection_reason_out)[i] =
                            ((*detection_source_out)[i] != 0) ? "interpolated" : "clean";
                    }
                }
            }
        }
    }

    if (detection_source_out) {
        std::vector<int32_t> detection_source_raw;
        if (readInt32Array(store, base_path + "detection_source", detection_source_raw) &&
            detection_source_raw.size() == total_detections) {
            detection_source_out->assign(detection_source_raw.size(), 0);
            for (size_t i = 0; i < detection_source_raw.size(); ++i) {
                (*detection_source_out)[i] = detection_source_raw[i] != 0 ? 1 : 0;
            }
        } else if (has_source_kind_codes) {
            detection_source_out->assign(total_detections, 0);
            int interpolated_code = 2;
            for (const auto& [code, label] : source_kind_labels_by_code) {
                if (label == "interpolated") {
                    interpolated_code = code;
                    break;
                }
            }
            for (size_t i = 0; i < total_detections; ++i) {
                (*detection_source_out)[i] =
                    (static_cast<int>(source_kind_codes[i]) == interpolated_code)
                        ? 1
                        : 0;
            }
        } else if (detection_reason_out &&
                   detection_reason_out->size() == total_detections) {
            detection_source_out->assign(total_detections, 0);
            for (size_t i = 0; i < total_detections; ++i) {
                const std::string lowered =
                    toLowerCopy((*detection_reason_out)[i]);
                (*detection_source_out)[i] =
                    (lowered == "interpolated" ||
                     lowered.find("interp") != std::string::npos)
                        ? 1
                        : 0;
            }
        } else {
            detection_source_out->clear();
        }
    }

    // Determine frame counts and offsets
    std::vector<size_t> frame_offsets_local;
    if (has_frame_indices) {
        int32_t max_frame_index = -1;
        for (const auto idx : frame_indices_local) {
            if (idx > max_frame_index) {
                max_frame_index = idx;
            }
        }

        resolved_frames_out = std::max(
            resolved_frames_out,
            max_frame_index >= 0 ? static_cast<size_t>(max_frame_index + 1) : size_t(0)
        );

        if (!n_detections_disk.empty()) {
            resolved_frames_out = std::max(
                resolved_frames_out,
                static_cast<size_t>(n_detections_disk.size())
            );
        }

        if (resolved_frames_out == 0) {
            std::cerr << "  Unable to resolve frame count for " << base_path << std::endl;
            return false;
        }

        frame_offsets_local.assign(resolved_frames_out + 1, 0);
        for (const auto frame_index : frame_indices_local) {
            if (frame_index < 0) {
                continue;
            }
            size_t frame = static_cast<size_t>(frame_index);
            if (frame + 1 >= frame_offsets_local.size()) {
                frame_offsets_local.resize(frame + 2, 0);
            }
            frame_offsets_local[frame + 1]++;
        }
        for (size_t i = 0; i + 1 < frame_offsets_local.size(); ++i) {
            frame_offsets_local[i + 1] += frame_offsets_local[i];
        }

        std::vector<std::array<float, 4>> sorted_boxes(total_detections);
        std::vector<float> sorted_scores(has_scores_out ? total_detections : 0);
        std::vector<int32_t> sorted_class_ids(has_class_ids_out ? total_detections : 0);
        std::vector<uint8_t> sorted_detection_source(
            (detection_source_out && detection_source_out->size() == total_detections)
                ? total_detections
                : 0,
            0);
        std::vector<std::string> sorted_detection_reason(
            (detection_reason_out && detection_reason_out->size() == total_detections)
                ? total_detections
                : 0);
        std::vector<size_t> write_cursor(frame_offsets_local.size() > 0
            ? frame_offsets_local.size() - 1
            : 0, 0);
        for (size_t frame = 0; frame < write_cursor.size(); ++frame) {
            write_cursor[frame] = frame_offsets_local[frame];
        }

        for (size_t det = 0; det < total_detections; ++det) {
            int32_t frame_index = frame_indices_local[det];
            if (frame_index < 0) {
                continue;
            }
            size_t frame = static_cast<size_t>(frame_index);
            if (frame >= write_cursor.size()) {
                continue;
            }
            size_t dest = write_cursor[frame]++;
            if (dest >= sorted_boxes.size()) {
                continue;
            }
            sorted_boxes[dest] = boxes_local[det];
            if (has_scores_out) {
                sorted_scores[dest] = scores_out[det];
            }
            if (has_class_ids_out) {
                sorted_class_ids[dest] = class_ids_out[det];
            }
            if (!sorted_detection_source.empty()) {
                sorted_detection_source[dest] = (*detection_source_out)[det];
            }
            if (!sorted_detection_reason.empty()) {
                sorted_detection_reason[dest] = (*detection_reason_out)[det];
            }
        }

        boxes_out = std::move(sorted_boxes);
        if (has_scores_out) {
            scores_out = std::move(sorted_scores);
        }
        if (has_class_ids_out) {
            class_ids_out = std::move(sorted_class_ids);
        }
        if (detection_source_out) {
            *detection_source_out = std::move(sorted_detection_source);
        }
        if (detection_reason_out) {
            *detection_reason_out = std::move(sorted_detection_reason);
        }

        if (frame_indices_out) {
            *frame_indices_out = std::move(frame_indices_local);
            std::vector<int32_t>& indices_ref = *frame_indices_out;
            for (size_t frame = 0; frame + 1 < frame_offsets_local.size(); ++frame) {
                size_t start = frame_offsets_local[frame];
                size_t end = frame_offsets_local[frame + 1];
                for (size_t idx = start; idx < end && idx < indices_ref.size(); ++idx) {
                    indices_ref[idx] = static_cast<int32_t>(frame);
                }
            }
        }
    } else {
        size_t frame_count = n_detections_disk.size();
        resolved_frames_out = std::max(resolved_frames_out, frame_count);

        frame_offsets_local.assign(frame_count + 1, 0);
        size_t running = 0;
        for (size_t frame = 0; frame < frame_count; ++frame) {
            int32_t count = std::max(n_detections_disk[frame], 0);
            running += static_cast<size_t>(count);
            frame_offsets_local[frame + 1] = running;
        }

        if (running != total_detections) {
            std::cerr << "  Warning: detection count mismatch under " << base_path
                      << " (expected " << running << ", have " << total_detections << ")"
                      << std::endl;
        }

        boxes_out = std::move(boxes_local);

        if (frame_indices_out) {
            frame_indices_out->clear();
            frame_indices_out->reserve(total_detections);
            for (size_t frame = 0; frame < frame_count; ++frame) {
                int32_t count = std::max(n_detections_disk[frame], 0);
                for (int32_t i = 0; i < count; ++i) {
                    frame_indices_out->push_back(static_cast<int32_t>(frame));
                }
            }
        }
    }

    if (n_detections_disk.empty()) {
        n_detections_out.assign(resolved_frames_out, 0);
        for (size_t frame = 0; frame + 1 < frame_offsets_local.size(); ++frame) {
            size_t start = frame_offsets_local[frame];
            size_t end = frame_offsets_local[frame + 1];
            size_t count = (end >= start) ? (end - start) : 0;
            n_detections_out[frame] = static_cast<int32_t>(count);
        }
    } else {
        n_detections_out = std::move(n_detections_disk);
    }

    frame_offsets_out = std::move(frame_offsets_local);

    return true;
}

bool ZarrDetectionLoader::loadRefinedDetectionsAsPrimary(const ts::kvstore::KvStore& store) {
    data_.has_refined_manual_dataset = false;
    data_.has_refined_filtered_dataset = false;
    data_.has_refined_interpolated_dataset = false;
    data_.has_refined_root_dataset = false;

    auto group_attrs = readAttrsAny(store, "refined_detect_runs");

    std::string latest_run;
    if (group_attrs.has_value()) {
        latest_run = extractLatestRunName(*group_attrs);
    }

    if (latest_run.empty()) {
        if (root_path_.empty()) {
            return false;
        }
        auto filtered_candidates = collect_runs_fs(
            root_path_,
            "refined_detect_runs",
            {"filtered/frame_indices", "filtered/bbox_norm_coords"});
        auto instances_candidates = collect_runs_fs(
            root_path_,
            "refined_detect_runs",
            {"instances/frame_indices", "instances/bbox_norm_coords"});
        auto refined_candidates = collect_runs_fs(
            root_path_,
            "refined_detect_runs",
            {"refined/frame_indices", "refined/bbox_norm_coords"});
        auto manual_candidates = collect_runs_fs(
            root_path_,
            "refined_detect_runs",
            {"manual/frame_indices", "manual/bbox_norm_coords"});
        auto root_candidates = collect_runs_fs(
            root_path_,
            "refined_detect_runs",
            {"frame_indices", "bbox_norm_coords"});

        std::vector<std::string> candidates;
        candidates.insert(candidates.end(),
                          instances_candidates.begin(), instances_candidates.end());
        candidates.insert(candidates.end(),
                          filtered_candidates.begin(), filtered_candidates.end());
        candidates.insert(candidates.end(),
                          refined_candidates.begin(), refined_candidates.end());
        candidates.insert(candidates.end(),
                          manual_candidates.begin(), manual_candidates.end());
        candidates.insert(candidates.end(),
                          root_candidates.begin(), root_candidates.end());

        candidates.erase(
            std::remove_if(
                candidates.begin(),
                candidates.end(),
                [](const std::string& name) {
                    return name.rfind("refined_detect_", 0) != 0;
                }),
            candidates.end());

        if (candidates.empty()) {
            return false;
        }
        std::sort(candidates.begin(), candidates.end());
        latest_run = candidates.back();
    }

    if (latest_run.empty()) {
        return false;
    }

    std::string run_base = "refined_detect_runs/" + latest_run + "/";
    std::optional<json> run_attrs = readAttrsAny(store, run_base);

    std::string manual_review_latest;
    if (run_attrs.has_value() &&
        run_attrs->contains("manual_review_latest") &&
        (*run_attrs)["manual_review_latest"].is_string()) {
        manual_review_latest = (*run_attrs)["manual_review_latest"].get<std::string>();
        const std::string run_prefix = run_base;
        if (manual_review_latest.rfind(run_prefix, 0) == 0) {
            manual_review_latest = manual_review_latest.substr(run_prefix.size());
        }
        while (!manual_review_latest.empty() && manual_review_latest.back() == '/') {
            manual_review_latest.pop_back();
        }
    }

    struct StageOption {
        std::string subdir;
        std::string label;
        DetectionDataset dataset_type;
    };

    std::vector<StageOption> stage_options;
    std::unordered_set<std::string> seen_stage_subdirs;
    auto add_stage = [&](std::string subdir,
                         std::string label,
                         DetectionDataset dataset_type) {
        if (seen_stage_subdirs.insert(subdir).second) {
            stage_options.push_back(StageOption{std::move(subdir),
                                                std::move(label),
                                                dataset_type});
        }
    };

    add_stage("instances/", "refined", DetectionDataset::RefinedRoot);
    add_stage("refined/", "refined", DetectionDataset::RefinedRoot);
    add_stage("", "root", DetectionDataset::RefinedRoot);
    if (!manual_review_latest.empty()) {
        add_stage(manual_review_latest + "/", manual_review_latest,
                  DetectionDataset::RefinedManual);
    }
    add_stage("manual/", "manual", DetectionDataset::RefinedManual);
    add_stage("interpolated/", "interpolated", DetectionDataset::RefinedInterpolated);
    add_stage("filtered/", "filtered", DetectionDataset::RefinedFiltered);

    InterpolationRunData stage_template;
    stage_template.run_name = latest_run;
    stage_template.uses_palette_layout = true;
    stage_template.has_flat_detections = true;
    if (run_attrs) {
        if ((*run_attrs).contains("method") && (*run_attrs)["method"].is_string()) {
            stage_template.method = (*run_attrs)["method"].get<std::string>();
        }
        if ((*run_attrs).contains("created_at") && (*run_attrs)["created_at"].is_string()) {
            stage_template.created_at = (*run_attrs)["created_at"].get<std::string>();
        } else if ((*run_attrs).contains("created_at_utc") && (*run_attrs)["created_at_utc"].is_string()) {
            stage_template.created_at = (*run_attrs)["created_at_utc"].get<std::string>();
        }
        if ((*run_attrs).contains("source_detection_run") && (*run_attrs)["source_detection_run"].is_string()) {
            stage_template.source_detection_run = (*run_attrs)["source_detection_run"].get<std::string>();
        }
        if ((*run_attrs).contains("provenance_json") && (*run_attrs)["provenance_json"].is_string()) {
            stage_template.provenance_json = (*run_attrs)["provenance_json"].get<std::string>();
        }
        if ((*run_attrs).contains("detect_review_status") &&
            (*run_attrs)["detect_review_status"].is_object()) {
            const auto& rs = (*run_attrs)["detect_review_status"];
            auto str_field = [&](const char* key) -> std::string {
                if (rs.contains(key) && rs[key].is_string()) return rs[key].get<std::string>();
                return "";
            };
            data_.review_state        = str_field("state");
            data_.review_method       = str_field("method");
            data_.review_intended_use = str_field("intended_use");
            data_.review_timestamp    = str_field("timestamp");
            data_.review_reviewer     = str_field("reviewer");
            data_.review_notes        = str_field("notes");
            data_.has_review_status   = !data_.review_state.empty();
        }
    }

    bool loaded_any = false;

    for (const auto& stage : stage_options) {
        std::string base_path = run_base + stage.subdir;
        size_t resolved_frames = data_.total_frames;
        std::vector<int32_t> frame_indices;
        std::vector<std::array<float, 4>> boxes;
        std::vector<float> scores;
        std::vector<int32_t> class_ids;
        std::vector<int32_t> n_detections;
        std::vector<size_t> frame_offsets;
        std::vector<uint8_t> detection_source;
        std::vector<std::string> detection_reason;
        bool has_scores = false;
        bool has_class_ids = false;

        if (!loadFlattenedRun(store,
                              base_path,
                              &frame_indices,
                              boxes,
                              scores,
                              class_ids,
                              n_detections,
                              frame_offsets,
                              &detection_source,
                              &detection_reason,
                              has_scores,
                              has_class_ids,
                              resolved_frames)) {
            continue;
        }

        InterpolationRunData stage_data = stage_template;
        stage_data.stage_label = stage.label;
        if (stage_data.method.empty()) {
            stage_data.method = stage.label;
        }
        stage_data.frame_indices = std::move(frame_indices);
        stage_data.bbox_norm_coords = std::move(boxes);
        stage_data.flat_scores = std::move(scores);
        stage_data.flat_class_ids = std::move(class_ids);
        stage_data.frame_offsets = std::move(frame_offsets);
        stage_data.n_detections = std::move(n_detections);
        stage_data.detection_source = std::move(detection_source);
        stage_data.detection_reason = std::move(detection_reason);
        stage_data.has_scores = has_scores;
        stage_data.has_class_ids = has_class_ids;
        cacheDetectionStage(std::move(stage_data), stage.dataset_type);
        loaded_any = true;

        const InterpolationRunData* stored = nullptr;
        switch (stage.dataset_type) {
            case DetectionDataset::RefinedManual:
                stored = &data_.refined_manual_dataset;
                break;
            case DetectionDataset::RefinedInterpolated:
                stored = &data_.refined_interpolated_dataset;
                break;
            case DetectionDataset::RefinedFiltered:
                stored = &data_.refined_filtered_dataset;
                break;
            case DetectionDataset::RefinedRoot:
                stored = &data_.refined_root_dataset;
                break;
            case DetectionDataset::RawDetect:
                stored = &data_.raw_detection_dataset;
                break;
        }
        std::cout << "  Refined detect run '" << latest_run << "' stage '"
                  << stage.label << "' loaded ("
                  << (stored ? stored->bbox_norm_coords.size() : 0)
                  << " detections)" << std::endl;
    }

    if (!loaded_any) {
        return false;
    }

    if (!manual_review_latest.empty() && !data_.has_refined_manual_dataset) {
        std::cout << "  Refined detect run '" << latest_run
                  << "' manual_review_latest='" << manual_review_latest
                  << "' not found; falling back to other refined groups" << std::endl;
    }

    if (data_.has_refined_root_dataset &&
        applyDetectionDataset(data_.refined_root_dataset,
                              DetectionDataset::RefinedRoot)) {
        std::cout << "  Using refined detect run '" << data_.detect_run_name
                  << "' as primary detections (refined)" << std::endl;
        return true;
    }

    if (data_.has_refined_manual_dataset &&
        applyDetectionDataset(data_.refined_manual_dataset,
                              DetectionDataset::RefinedManual)) {
        std::cout << "  Using refined detect run '" << data_.detect_run_name
                  << "' as primary detections (legacy manual fallback)" << std::endl;
        return true;
    }

    if (data_.has_refined_interpolated_dataset &&
        applyDetectionDataset(data_.refined_interpolated_dataset,
                              DetectionDataset::RefinedInterpolated)) {
        std::cout << "  Using refined detect run '" << data_.detect_run_name
                  << "' as primary detections (legacy interpolated fallback)" << std::endl;
        return true;
    }

    if (data_.has_refined_filtered_dataset &&
        applyDetectionDataset(data_.refined_filtered_dataset,
                              DetectionDataset::RefinedFiltered)) {
        std::cout << "  Using refined detect run '" << data_.detect_run_name
                  << "' as primary detections (legacy filtered fallback)" << std::endl;
        return true;
    }

    return false;
}

bool ZarrDetectionLoader::loadRefinedDetectRuns(const ts::kvstore::KvStore& store) {
    auto group_attrs = readAttrsAny(store, "refined_detect_runs");

    std::string latest_run;
    if (group_attrs.has_value()) {
        latest_run = extractLatestRunName(*group_attrs);
    }

    if (latest_run.empty()) {
        if (root_path_.empty()) {
            return false;
        }
        std::vector<std::string> candidates;
        auto append_candidates = [&](std::initializer_list<std::string> required_arrays) {
            auto found = collect_runs_fs(root_path_, "refined_detect_runs", required_arrays);
            candidates.insert(candidates.end(), found.begin(), found.end());
        };
        append_candidates({"instances/frame_indices", "instances/bbox_norm_coords"});
        append_candidates({"interpolated/frame_indices", "interpolated/bbox_norm_coords"});
        append_candidates({"filtered/frame_indices", "filtered/bbox_norm_coords"});
        append_candidates({"refined/frame_indices", "refined/bbox_norm_coords"});
        append_candidates({"frame_indices", "bbox_norm_coords"});
        candidates.erase(
            std::remove_if(
                candidates.begin(),
                candidates.end(),
                [](const std::string& name) {
                    return name.rfind("refined_detect_", 0) != 0;
                }),
            candidates.end());
        std::sort(candidates.begin(), candidates.end());
        if (candidates.empty()) {
            return false;
        }
        latest_run = candidates.back();
    }

    std::string run_base = "refined_detect_runs/" + latest_run + "/";
    std::cout << "  Loading refined detection run '" << latest_run << "'" << std::endl;

    InterpolationRunData refined;
    refined.run_name = latest_run;
    refined.uses_palette_layout = true;
    refined.has_flat_detections = false;

    if (auto run_attrs = readAttrsAny(store, run_base)) {
        populateInterpolationMetadata(*run_attrs, refined);
    }

    static const std::vector<std::string> kSubgroups = {
        "instances/",
        "interpolated/",
        "refined/",
        "filtered/",
        ""
    };

    for (const auto& subgroup : kSubgroups) {
        std::string base_path = run_base + subgroup;
        size_t resolved_frames = data_.total_frames;
        std::vector<int32_t> frame_indices;
        std::vector<std::array<float, 4>> boxes;
        std::vector<float> scores;
        std::vector<int32_t> class_ids;
        std::vector<int32_t> n_detections;
        std::vector<size_t> frame_offsets;
        std::vector<uint8_t> detection_source;
        std::vector<std::string> detection_reason;
        bool has_scores = false;
        bool has_class_ids = false;

        if (!loadFlattenedRun(store,
                              base_path,
                              &frame_indices,
                              boxes,
                              scores,
                              class_ids,
                              n_detections,
                              frame_offsets,
                              &detection_source,
                              &detection_reason,
                              has_scores,
                              has_class_ids,
                              resolved_frames)) {
            continue;
        }

        refined.has_flat_detections = true;
        refined.frame_indices = std::move(frame_indices);
        refined.bbox_norm_coords = std::move(boxes);
        refined.flat_scores = std::move(scores);
        refined.flat_class_ids = std::move(class_ids);
        refined.frame_offsets = std::move(frame_offsets);
        refined.detection_source = std::move(detection_source);
        refined.detection_reason = std::move(detection_reason);
        refined.has_stimulus_alignment = false;
        refined.has_flat_detections = true;
        refined.is_loaded = true;
        refined.uses_palette_layout = true;

        if (refined.method.empty()) {
            if (subgroup == "instances/") {
                refined.method = "refined";
            } else if (!subgroup.empty()) {
                refined.method = subgroup.substr(0, subgroup.size() - 1);
            } else if (refined.method.empty()) {
                refined.method = "refined_detect";
            }
        }

        size_t frame_count = refined.frame_offsets.size() > 0
            ? refined.frame_offsets.size() - 1
            : resolved_frames;
        refined.frame_mask.assign(frame_count, 0);

        if (!refined.detection_source.empty()) {
            for (size_t frame = 0; frame + 1 < refined.frame_offsets.size(); ++frame) {
                size_t start = refined.frame_offsets[frame];
                size_t end = refined.frame_offsets[frame + 1];
                for (size_t idx = start; idx < end && idx < refined.detection_source.size(); ++idx) {
                    if (refined.detection_source[idx] != 0) {
                        refined.frame_mask[frame] = 1;
                        break;
                    }
                }
            }
        }

        data_.latest_interpolation = std::move(refined);
        data_.has_interpolation = true;

        std::string subgroup_label = subgroup.empty()
            ? "root"
            : subgroup.substr(0, subgroup.size() - 1);
        if (subgroup_label == "instances") {
            subgroup_label = "refined";
        }

        std::cout << "  Refined detect run '" << latest_run
                  << "' (" << subgroup_label << ") loaded with "
                  << data_.latest_interpolation.bbox_norm_coords.size()
                  << " detections" << std::endl;
        return true;
    }

    std::cout << "  Refined detect run '" << latest_run
              << "' does not contain interpolated outputs" << std::endl;
    return false;
}
