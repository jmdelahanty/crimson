#include "zarr/palette_clipped_resolver.h"
#include "debug_flags.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <unordered_set>

#ifndef CRIMSON_ENABLE_PARQUET
#define CRIMSON_ENABLE_PARQUET 0
#endif

#if CRIMSON_ENABLE_PARQUET
#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>
#endif

using json = nlohmann::json;

namespace {

std::optional<json> readJsonFile(const std::filesystem::path& path,
                                 std::string& error_message) {
    std::ifstream in(path);
    if (!in) {
        error_message = "Could not open JSON file: " + path.string();
        return std::nullopt;
    }
    try {
        json payload;
        in >> payload;
        return payload;
    } catch (const std::exception& e) {
        error_message = "Could not parse JSON file " + path.string() + ": " + e.what();
        return std::nullopt;
    }
}

double elapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - start)
        .count();
}

std::optional<json> readZarrAttrs(const std::filesystem::path& group_path,
                                  std::string& error_message) {
    auto payload = readJsonFile(group_path / "zarr.json", error_message);
    if (!payload.has_value()) {
        return std::nullopt;
    }
    if (payload->contains("attributes") && (*payload)["attributes"].is_object()) {
        return (*payload)["attributes"];
    }
    if (payload->is_object()) {
        return *payload;
    }
    return json::object();
}

std::string jsonString(const json& object, const char* key) {
    if (object.contains(key) && object[key].is_string()) {
        return object[key].get<std::string>();
    }
    return {};
}

std::string jsonAnyToString(const json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<int64_t>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<uint64_t>());
    }
    return {};
}

std::filesystem::path expandAndResolvePath(const std::filesystem::path& path) {
    std::filesystem::path expanded = path;
    const std::string raw = path.string();
    if (!raw.empty() && raw[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home != nullptr) {
            if (raw.size() == 1) {
                expanded = home;
            } else if (raw[1] == '/') {
                expanded = std::filesystem::path(home) / raw.substr(2);
            }
        }
    }

    std::error_code ec;
    auto absolute = std::filesystem::absolute(expanded, ec);
    if (!ec) {
        expanded = absolute;
    }
    auto canonical = std::filesystem::weakly_canonical(expanded, ec);
    return ec ? expanded : canonical;
}

std::filesystem::path resolvePathRelativeTo(const std::filesystem::path& base,
                                            const std::string& raw_path) {
    if (raw_path.empty()) {
        return {};
    }
    std::filesystem::path path(raw_path);
    if (!path.is_absolute()) {
        path = base / path;
    }
    return expandAndResolvePath(path);
}

std::string runKey(const std::string& camera_serial, const std::string& clip_id) {
    return camera_serial + '\0' + clip_id;
}

std::string sourceString(const json& run, const char* key) {
    if (!run.contains("source") || !run["source"].is_object()) {
        return {};
    }
    return jsonString(run["source"], key);
}

std::optional<std::filesystem::path> resolveRecordingFrameIndexPath(
    const std::filesystem::path& analysis_zarr,
    const json& root_attrs,
    const json& collection_attrs,
    std::string& error_message) {
    std::string raw_path = jsonString(root_attrs, "recording_frame_index_path");
    if (!raw_path.empty()) {
        return resolvePathRelativeTo(analysis_zarr.parent_path(), raw_path);
    }

    raw_path = jsonString(collection_attrs, "recording_frame_index_path");
    if (!raw_path.empty()) {
        return resolvePathRelativeTo(analysis_zarr.parent_path(), raw_path);
    }

    std::filesystem::path manifest_path;
    const std::string root_manifest =
        jsonString(root_attrs, "recording_frame_index_manifest_path");
    if (!root_manifest.empty()) {
        manifest_path = resolvePathRelativeTo(analysis_zarr.parent_path(), root_manifest);
    }

    if (manifest_path.empty()) {
        const std::string plan_path_raw = jsonString(collection_attrs, "plan_path");
        if (!plan_path_raw.empty()) {
            const auto plan_path =
                resolvePathRelativeTo(analysis_zarr.parent_path(), plan_path_raw);
            auto plan = readJsonFile(plan_path, error_message);
            if (!plan.has_value()) {
                return std::nullopt;
            }
            const std::string recording_dir_raw = jsonString(*plan, "recording_dir");
            if (!recording_dir_raw.empty()) {
                const auto recording_dir =
                    resolvePathRelativeTo(plan_path.parent_path(), recording_dir_raw);
                manifest_path = recording_dir / "recording_frame_index_manifest.json";
            }
        }
    }

    if (manifest_path.empty()) {
        error_message =
            "No recording_frame_index_path or recording_frame_index_manifest_path found.";
        return std::nullopt;
    }

    auto manifest = readJsonFile(manifest_path, error_message);
    if (!manifest.has_value()) {
        return std::nullopt;
    }
    std::string manifest_frame_path =
        jsonString(*manifest, "recording_frame_index_path");
    if (manifest_frame_path.empty()) {
        manifest_frame_path = "recording_frame_index.parquet";
    }
    return resolvePathRelativeTo(manifest_path.parent_path(), manifest_frame_path);
}

#if CRIMSON_ENABLE_PARQUET

std::string scalarToString(const std::shared_ptr<arrow::Scalar>& scalar);

std::string binaryScalarToString(const arrow::BaseBinaryScalar& scalar) {
    return scalar.is_valid && scalar.value ? scalar.value->ToString() : std::string();
}

std::string scalarToString(const std::shared_ptr<arrow::Scalar>& scalar) {
    if (!scalar || !scalar->is_valid) {
        return {};
    }
    switch (scalar->type->id()) {
        case arrow::Type::STRING:
        case arrow::Type::LARGE_STRING:
        case arrow::Type::BINARY:
        case arrow::Type::LARGE_BINARY:
            return binaryScalarToString(
                static_cast<const arrow::BaseBinaryScalar&>(*scalar));
        case arrow::Type::INT8:
            return std::to_string(static_cast<const arrow::Int8Scalar&>(*scalar).value);
        case arrow::Type::INT16:
            return std::to_string(static_cast<const arrow::Int16Scalar&>(*scalar).value);
        case arrow::Type::INT32:
            return std::to_string(static_cast<const arrow::Int32Scalar&>(*scalar).value);
        case arrow::Type::INT64:
            return std::to_string(static_cast<const arrow::Int64Scalar&>(*scalar).value);
        case arrow::Type::UINT8:
            return std::to_string(static_cast<const arrow::UInt8Scalar&>(*scalar).value);
        case arrow::Type::UINT16:
            return std::to_string(static_cast<const arrow::UInt16Scalar&>(*scalar).value);
        case arrow::Type::UINT32:
            return std::to_string(static_cast<const arrow::UInt32Scalar&>(*scalar).value);
        case arrow::Type::UINT64:
            return std::to_string(static_cast<const arrow::UInt64Scalar&>(*scalar).value);
        case arrow::Type::DICTIONARY: {
            const auto& dict_scalar =
                static_cast<const arrow::DictionaryScalar&>(*scalar);
            auto encoded_result = dict_scalar.GetEncodedValue();
            if (encoded_result.ok()) {
                return scalarToString(encoded_result.ValueOrDie());
            }
            return {};
        }
        default:
            return scalar->ToString();
    }
}

std::optional<int64_t> scalarToInt64(const std::shared_ptr<arrow::Scalar>& scalar) {
    if (!scalar || !scalar->is_valid) {
        return std::nullopt;
    }
    switch (scalar->type->id()) {
        case arrow::Type::INT8:
            return static_cast<const arrow::Int8Scalar&>(*scalar).value;
        case arrow::Type::INT16:
            return static_cast<const arrow::Int16Scalar&>(*scalar).value;
        case arrow::Type::INT32:
            return static_cast<const arrow::Int32Scalar&>(*scalar).value;
        case arrow::Type::INT64:
            return static_cast<const arrow::Int64Scalar&>(*scalar).value;
        case arrow::Type::UINT8:
            return static_cast<const arrow::UInt8Scalar&>(*scalar).value;
        case arrow::Type::UINT16:
            return static_cast<const arrow::UInt16Scalar&>(*scalar).value;
        case arrow::Type::UINT32:
            return static_cast<const arrow::UInt32Scalar&>(*scalar).value;
        case arrow::Type::UINT64: {
            const auto value = static_cast<const arrow::UInt64Scalar&>(*scalar).value;
            if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                return std::nullopt;
            }
            return static_cast<int64_t>(value);
        }
        default:
            return std::nullopt;
    }
}

bool readRequiredScalar(const std::shared_ptr<arrow::ChunkedArray>& column,
                        int64_t row,
                        std::shared_ptr<arrow::Scalar>& scalar,
                        std::string& error_message,
                        const char* column_name) {
    if (!column) {
        error_message = std::string("Missing required Parquet column: ") + column_name;
        return false;
    }
    auto scalar_result = column->GetScalar(row);
    if (!scalar_result.ok()) {
        error_message = std::string("Could not read Parquet column '") +
                        column_name + "' row " + std::to_string(row) + ": " +
                        scalar_result.status().ToString();
        return false;
    }
    scalar = scalar_result.ValueOrDie();
    return true;
}

#endif

}  // namespace

void PaletteClippedResolver::clear() {
    loaded_ = false;
    analysis_zarr_.clear();
    recording_frame_index_path_.clear();
    collection_id_.clear();
    primary_camera_serial_.clear();
    selected_runs_.clear();
    rows_.clear();
    parent_to_row_by_camera_.clear();
    mapped_frame_count_ = 0;
    total_parent_frames_ = 0;
    unselected_frame_pair_count_ = 0;
}

const PaletteClippedResolver::SelectedRun*
PaletteClippedResolver::selectedRun(size_t index) const {
    if (index >= selected_runs_.size()) {
        return nullptr;
    }
    return &selected_runs_[index];
}

const PaletteClippedResolver::FrameRunRow*
PaletteClippedResolver::rowForParentFrame(
    int64_t parent_frame_index,
    const std::string& camera_serial) const {
    if (parent_frame_index < 0) {
        return nullptr;
    }
    const std::string& camera =
        camera_serial.empty() ? primary_camera_serial_ : camera_serial;
    auto it = parent_to_row_by_camera_.find(camera);
    if (it == parent_to_row_by_camera_.end()) {
        return nullptr;
    }
    const auto& index = it->second;
    const size_t parent = static_cast<size_t>(parent_frame_index);
    if (parent >= index.size() || index[parent] < 0) {
        return nullptr;
    }
    const size_t row_index = static_cast<size_t>(index[parent]);
    return row_index < rows_.size() ? &rows_[row_index] : nullptr;
}

bool PaletteClippedResolver::load(const std::filesystem::path& analysis_zarr,
                                  const std::string& explicit_collection_id,
                                  std::string& error_message) {
    clear();
    error_message.clear();
    const bool clipped_startup_trace =
        crimson_clipped_startup_trace_enabled();
    const auto load_start = std::chrono::steady_clock::now();
    double root_attrs_ms = 0.0;
    double refined_attrs_ms = 0.0;
    double collection_attrs_ms = 0.0;
    double selected_runs_ms = 0.0;
    double frame_index_path_ms = 0.0;
    double parquet_open_ms = 0.0;
    double parquet_schema_ms = 0.0;
    double parquet_read_table_ms = 0.0;
    double map_rows_ms = 0.0;

    analysis_zarr_ = expandAndResolvePath(analysis_zarr);

    auto phase_start = std::chrono::steady_clock::now();
    auto root_attrs = readZarrAttrs(analysis_zarr_, error_message);
    root_attrs_ms = elapsedMs(phase_start);
    if (!root_attrs.has_value()) {
        return false;
    }

    std::string collection_id = explicit_collection_id;
    if (collection_id.empty()) {
        phase_start = std::chrono::steady_clock::now();
        auto refined_attrs =
            readZarrAttrs(analysis_zarr_ / "refined_detect_runs", error_message);
        refined_attrs_ms = elapsedMs(phase_start);
        if (!refined_attrs.has_value()) {
            error_message.clear();
            return false;
        }
        collection_id = jsonString(*refined_attrs, "latest_collection");
    }
    if (collection_id.empty()) {
        error_message.clear();
        return false;
    }

#if !CRIMSON_ENABLE_PARQUET
    (void)root_attrs;
    error_message =
        "Clipped finalized collection '" + collection_id +
        "' requires Arrow/Parquet support; rebuild with CRIMSON_ENABLE_PARQUET=ON.";
    return false;
#else
    const auto collection_path =
        analysis_zarr_ / "experiment_index" / "finalized_runs" / collection_id;
    phase_start = std::chrono::steady_clock::now();
    auto collection_attrs = readZarrAttrs(collection_path, error_message);
    collection_attrs_ms = elapsedMs(phase_start);
    if (!collection_attrs.has_value()) {
        return false;
    }
    if (!collection_attrs->contains("selected_runs") ||
        !(*collection_attrs)["selected_runs"].is_array()) {
        error_message = "Finalized collection has no selected_runs array: " +
                        collection_path.string();
        return false;
    }

    std::unordered_map<std::string, size_t> selected_lookup;
    phase_start = std::chrono::steady_clock::now();
    for (const auto& run : (*collection_attrs)["selected_runs"]) {
        if (!run.is_object()) {
            continue;
        }
        SelectedRun selected;
        selected.work_unit_id = jsonString(run, "work_unit_id");
        if (run.contains("camera_serial")) {
            selected.camera_serial = jsonAnyToString(run["camera_serial"]);
        }
        selected.clip_id = jsonString(run, "clip_id");
        selected.detect_run = jsonString(run, "detect_run");
        selected.refined_detect_run = jsonString(run, "refined_detect_run");
        selected.detect_group_path = jsonString(run, "detect_group_path");
        selected.refined_group_path = jsonString(run, "refined_group_path");
        selected.video_path = sourceString(run, "video_path");
        selected.metadata_path = sourceString(run, "metadata_path");
        selected.keyframe_path = sourceString(run, "keyframe_path");

        if (selected.camera_serial.empty() || selected.clip_id.empty() ||
            selected.refined_group_path.empty()) {
            continue;
        }
        const std::string key = runKey(selected.camera_serial, selected.clip_id);
        if (selected_lookup.find(key) != selected_lookup.end()) {
            error_message =
                "Duplicate selected run for camera/clip pair in finalized collection: " +
                selected.camera_serial + "/" + selected.clip_id;
            return false;
        }
        selected_lookup[key] = selected_runs_.size();
        selected_runs_.push_back(std::move(selected));
    }
    selected_runs_ms = elapsedMs(phase_start);

    if (selected_runs_.empty()) {
        error_message = "Finalized collection selected_runs resolved to zero usable runs.";
        return false;
    }

    phase_start = std::chrono::steady_clock::now();
    auto frame_index_path =
        resolveRecordingFrameIndexPath(analysis_zarr_, *root_attrs,
                                       *collection_attrs, error_message);
    frame_index_path_ms = elapsedMs(phase_start);
    if (!frame_index_path.has_value()) {
        return false;
    }
    recording_frame_index_path_ = *frame_index_path;

    phase_start = std::chrono::steady_clock::now();
    auto input_result = arrow::io::ReadableFile::Open(
        recording_frame_index_path_.string(), arrow::default_memory_pool());
    if (!input_result.ok()) {
        error_message = "Could not open recording_frame_index parquet: " +
                        input_result.status().ToString();
        return false;
    }
    std::shared_ptr<arrow::io::ReadableFile> input = input_result.ValueOrDie();

    auto reader_result =
        parquet::arrow::OpenFile(input, arrow::default_memory_pool());
    parquet_open_ms = elapsedMs(phase_start);
    if (!reader_result.ok()) {
        error_message =
            "Could not create Parquet reader: " +
            reader_result.status().ToString();
        return false;
    }
    std::unique_ptr<parquet::arrow::FileReader> reader =
        std::move(reader_result).ValueOrDie();

    phase_start = std::chrono::steady_clock::now();
    std::shared_ptr<arrow::Schema> schema;
    auto schema_status = reader->GetSchema(&schema);
    parquet_schema_ms = elapsedMs(phase_start);
    if (!schema_status.ok() || !schema) {
        error_message =
            "Could not read recording_frame_index schema: " +
            schema_status.ToString();
        return false;
    }

    std::vector<int> column_indices;
    auto require_column = [&](const char* name) -> bool {
        const int index = schema->GetFieldIndex(name);
        if (index < 0) {
            error_message =
                "recording_frame_index.parquet is missing required column: " +
                std::string(name);
            return false;
        }
        column_indices.push_back(index);
        return true;
    };
    if (!require_column("camera_serial") ||
        !require_column("clip_id") ||
        !require_column("recording_frame_id") ||
        !require_column("clip_local_frame_index")) {
        return false;
    }
    const int parent_column_index = schema->GetFieldIndex("parent_frame_index");
    if (parent_column_index >= 0) {
        column_indices.push_back(parent_column_index);
    }

    std::shared_ptr<arrow::Table> table;
    phase_start = std::chrono::steady_clock::now();
    auto table_status = reader->ReadTable(column_indices, &table);
    parquet_read_table_ms = elapsedMs(phase_start);
    if (!table_status.ok()) {
        error_message =
            "Could not read recording_frame_index table: " + table_status.ToString();
        return false;
    }

    auto camera_col = table->GetColumnByName("camera_serial");
    auto clip_col = table->GetColumnByName("clip_id");
    auto recording_frame_col = table->GetColumnByName("recording_frame_id");
    auto clip_local_col = table->GetColumnByName("clip_local_frame_index");
    auto parent_col = table->GetColumnByName("parent_frame_index");
    if (!camera_col || !clip_col || !recording_frame_col || !clip_local_col) {
        error_message =
            "recording_frame_index.parquet is missing one of: camera_serial, "
            "clip_id, recording_frame_id, clip_local_frame_index.";
        return false;
    }

    std::unordered_set<std::string> missing_pairs;
    const int64_t row_count = table->num_rows();
    rows_.reserve(static_cast<size_t>(row_count));

    phase_start = std::chrono::steady_clock::now();
    for (int64_t parquet_row = 0; parquet_row < row_count; ++parquet_row) {
        std::shared_ptr<arrow::Scalar> scalar;
        if (!readRequiredScalar(camera_col, parquet_row, scalar, error_message,
                                "camera_serial")) {
            return false;
        }
        const std::string camera_serial = scalarToString(scalar);

        if (!readRequiredScalar(clip_col, parquet_row, scalar, error_message,
                                "clip_id")) {
            return false;
        }
        const std::string clip_id = scalarToString(scalar);

        const auto lookup_it = selected_lookup.find(runKey(camera_serial, clip_id));
        if (lookup_it == selected_lookup.end()) {
            missing_pairs.insert(runKey(camera_serial, clip_id));
            continue;
        }

        if (!readRequiredScalar(recording_frame_col, parquet_row, scalar,
                                error_message, "recording_frame_id")) {
            return false;
        }
        const auto recording_frame_id = scalarToInt64(scalar);
        if (!recording_frame_id.has_value()) {
            error_message = "recording_frame_id is not an integer at row " +
                            std::to_string(parquet_row);
            return false;
        }

        if (!readRequiredScalar(clip_local_col, parquet_row, scalar, error_message,
                                "clip_local_frame_index")) {
            return false;
        }
        const auto clip_local_frame_index = scalarToInt64(scalar);
        if (!clip_local_frame_index.has_value()) {
            error_message = "clip_local_frame_index is not an integer at row " +
                            std::to_string(parquet_row);
            return false;
        }

        int64_t parent_frame_index = *recording_frame_id - 1;
        if (parent_col) {
            auto parent_scalar = parent_col->GetScalar(parquet_row);
            if (!parent_scalar.ok()) {
                error_message = "Could not read parent_frame_index at row " +
                                std::to_string(parquet_row) + ": " +
                                parent_scalar.status().ToString();
                return false;
            }
            auto parent_value = scalarToInt64(parent_scalar.ValueOrDie());
            if (parent_value.has_value()) {
                parent_frame_index = *parent_value;
            }
        }
        if (parent_frame_index < 0 || *clip_local_frame_index < 0) {
            continue;
        }

        FrameRunRow mapped;
        mapped.camera_serial = camera_serial;
        mapped.clip_id = clip_id;
        mapped.recording_frame_id = *recording_frame_id;
        mapped.parent_frame_index = parent_frame_index;
        mapped.clip_local_frame_index = *clip_local_frame_index;
        mapped.selected_run_index = lookup_it->second;

        const size_t row_index = rows_.size();
        rows_.push_back(std::move(mapped));

        auto& parent_index = parent_to_row_by_camera_[camera_serial];
        const size_t parent = static_cast<size_t>(parent_frame_index);
        if (parent >= parent_index.size()) {
            parent_index.resize(parent + 1, -1);
        }
        parent_index[parent] = static_cast<int64_t>(row_index);

        auto& selected = selected_runs_[lookup_it->second];
        const size_t clip_local = static_cast<size_t>(*clip_local_frame_index);
        if (clip_local >= selected.parent_frame_by_clip_local.size()) {
            selected.parent_frame_by_clip_local.resize(clip_local + 1, -1);
        }
        selected.parent_frame_by_clip_local[clip_local] = parent_frame_index;
    }
    map_rows_ms = elapsedMs(phase_start);

    if (rows_.empty()) {
        error_message =
            "Finalized collection did not map any recording_frame_index rows.";
        return false;
    }

    primary_camera_serial_ = selected_runs_.front().camera_serial;
    mapped_frame_count_ = rows_.size();
    unselected_frame_pair_count_ = missing_pairs.size();
    collection_id_ = collection_id;
    total_parent_frames_ = 0;
    for (const auto& entry : parent_to_row_by_camera_) {
        total_parent_frames_ = std::max(total_parent_frames_, entry.second.size());
    }
    loaded_ = true;
    if (clipped_startup_trace) {
        std::cout << "[ClippedStartup] resolver_detail total_ms="
                  << elapsedMs(load_start)
                  << " collection=" << collection_id_
                  << " row_count=" << row_count
                  << " selected_runs=" << selected_runs_.size()
                  << " mapped_frames=" << mapped_frame_count_
                  << " root_attrs_ms=" << root_attrs_ms
                  << " refined_attrs_ms=" << refined_attrs_ms
                  << " collection_attrs_ms=" << collection_attrs_ms
                  << " selected_runs_ms=" << selected_runs_ms
                  << " frame_index_path_ms=" << frame_index_path_ms
                  << " parquet_open_ms=" << parquet_open_ms
                  << " parquet_schema_ms=" << parquet_schema_ms
                  << " parquet_read_table_ms=" << parquet_read_table_ms
                  << " map_rows_ms=" << map_rows_ms
                  << std::endl;
    }
    return true;
#endif
}
