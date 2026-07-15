#pragma once

#include "zarr_loader.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <absl/strings/cord.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <utility>
#include <limits>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <tensorstore/cast.h>
#include <tensorstore/driver/zarr/dtype.h>
#include <tensorstore/kvstore/key_range.h>
#include <tensorstore/kvstore/operations.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr bool kChaserDebugLoggingEnabled =
#if defined(CRIMSON_CHASER_DEBUG_LOGS)
    true;
#else
    false;
#endif
;

constexpr bool kAttrProbeMissLoggingEnabled =
#if defined(CRIMSON_ATTR_PROBE_DEBUG_LOGS)
    true;
#else
    false;
#endif
;

// ---------------------------------------------------------------------------
// Structs
// ---------------------------------------------------------------------------

struct ZarrDiscoveryCandidate {
    std::filesystem::path path;
    std::string name;
    bool has_detect_runs = false;
    bool is_analysis_archive = false;
    bool name_suggests_analysis = false;
    bool name_suggests_training = false;
    int rank = 0;
};

struct ZarrDiscoveryResult {
    std::optional<std::filesystem::path> selected_archive;
    std::string error_message;
};

enum class ReasonAuthority {
    None,
    ReasonBytes,
    LegacyReason,
    DetectionSource,
};

struct ReasonBytesDecodeStats {
    size_t rows_without_null_terminator = 0;
    size_t rows_with_malformed_utf8 = 0;
};

struct ReasonColumnResolution {
    std::vector<std::string> labels;
    ReasonAuthority authority = ReasonAuthority::None;
    size_t conflicting_legacy_rows = 0;
};

#pragma pack(push, 1)
struct StimulusEventRowV3 {
    int64_t timestamp_ns_epoch = 0;
    int64_t timestamp_ns_session = 0;
    int32_t event_type_id = 0;
    int32_t current_step_index = 0;
    uint64_t stimulus_frame_num = 0;
    uint64_t camera_frame_id = 0;
    char name_or_context[256];
    int32_t stimulus_mode_id = 0;
    char details_json[1024];
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Function declarations
// ---------------------------------------------------------------------------

std::string appendPath(const std::string& base, const std::string& suffix);

std::optional<nlohmann::json> readAttrsAny(const ts::kvstore::KvStore& store,
                                           const std::string& path);

std::optional<nlohmann::json> readNodeMetaV3(const ts::kvstore::KvStore& store,
                                             const std::string& path);

std::string normalizeKvstoreFileRootPath(std::filesystem::path path);

std::string toLowerCopy(std::string value);

bool hasZarrArchiveExtension(const std::filesystem::path& path);

bool usesZarrV3StringDataType(const nlohmann::json& node_meta);

std::optional<nlohmann::json> readAttrsFromZarrJsonFile(const std::filesystem::path& zarr_json_path);

bool isAnalysisArchiveByAttrs(const std::filesystem::path& archive_root);

int scoreDiscoveryCandidate(const ZarrDiscoveryCandidate& candidate);

void appendDiscoveryCandidatesInDirectory(
    const std::filesystem::path& directory_path,
    std::vector<ZarrDiscoveryCandidate>& candidates);

ZarrDiscoveryResult discoverZarrArchiveInDirectory(const std::string& directory);

std::optional<std::filesystem::path> resolveExplicitZarrArchivePath(
    const std::string& candidate_path);

std::string NormalizeCropRunName(const std::string& name);

std::string ExtractCropRunFromObject(const nlohmann::json& obj);

std::string ResolveCropRunFromKeypointRun(const ts::kvstore::KvStore& store,
                                          const std::string& run_name);

std::string currentUtcIsoTimestamp();

nlohmann::json makeEmptyGroupMetadataV3();

nlohmann::json normalizeGroupMetadataV3(nlohmann::json node_meta);

bool writeNodeMetaV3(const ts::kvstore::KvStore& store,
                     const std::string& path,
                     const nlohmann::json& node_meta,
                     std::string* error_message);

nlohmann::json makeNumericArrayMetadata(const std::vector<ts::Index>& shape,
                                        const std::vector<ts::Index>& chunk_shape,
                                        const std::string& data_type,
                                        const nlohmann::json& fill_value,
                                        bool bytes_endian_little);

bool arrayExists(const ts::kvstore::KvStore& store, const std::string& path);

std::string sanitizeUtf8ReplacingInvalid(std::string_view value,
                                         bool* malformed = nullptr);

bool readReasonBytesArray(const ts::kvstore::KvStore& store,
                          const std::string& path,
                          const ts::Context& context,
                          std::vector<std::string>& out,
                          ReasonBytesDecodeStats* stats = nullptr,
                          std::string* error_message = nullptr);

ReasonColumnResolution resolveReasonColumns(
    size_t expected_rows,
    const std::optional<std::vector<std::string>>& reason_bytes,
    const std::optional<std::vector<std::string>>& legacy_reason,
    const std::vector<uint8_t>* detection_source);

const char* reasonAuthorityName(ReasonAuthority authority);

void setCanonicalReasonAttrs(nlohmann::json& attrs, size_t reason_bytes_width);

std::string stringFromFixedBuffer(const char* buffer, size_t size);

int32_t clampToInt32(int64_t value);

int32_t clampUint64ToInt32(uint64_t value);

std::vector<std::string> collect_runs_fs(const std::string& root_path,
                                         const std::string& group_dir,
                                         std::initializer_list<std::string> required_arrays);

std::string extractLatestRunName(const nlohmann::json& attrs);

void populateDetectionMetadata(const nlohmann::json& attrs, ZarrDetectionData& data);

void populateInterpolationMetadata(const nlohmann::json& attrs,
                                   InterpolationRunData& interp);

std::array<float, 4> normalizedBoxToPixels(
    const std::array<float, 4>& norm_box,
    int image_width,
    int image_height);

// ---------------------------------------------------------------------------
// Template function implementations (must be in the header)
// ---------------------------------------------------------------------------

template <typename T, int Rank>
ts::Result<ts::TensorStore<T, Rank>> openArrayAny(
    const ts::kvstore::KvStore& store,
    const std::string& path,
    const ts::Context& context) {

    using json = nlohmann::json;

    auto store_spec = store.spec();
    if (!store_spec.ok()) {
        return store_spec.status();
    }

    auto kv_json_or = store_spec.value().ToJson();
    if (!kv_json_or.ok()) {
        return kv_json_or.status();
    }

    json spec = {
        {"driver", "zarr3"},
        {"kvstore", kv_json_or.value()},
        {"path", path}
    };
    return ts::Open<T, Rank>(
               spec,
               ts::OpenMode::open,
               ts::ReadWriteMode::read,
               context)
        .result();
}

template <typename T, int Rank>
ts::Result<ts::TensorStore<T, Rank>> openArrayForWrite(
    const ts::kvstore::KvStore& store,
    const std::string& path,
    const nlohmann::json& metadata,
    const ts::Context& context) {

    using json = nlohmann::json;

    auto store_spec = store.spec();
    if (!store_spec.ok()) {
        return store_spec.status();
    }

    auto kv_json_or = store_spec.value().ToJson();
    if (!kv_json_or.ok()) {
        return kv_json_or.status();
    }

    json spec = {
        {"driver", "zarr3"},
        {"kvstore", kv_json_or.value()},
        {"path", path},
        {"metadata", metadata}
    };
    return ts::Open<T, Rank>(
               spec,
               ts::OpenMode::open | ts::OpenMode::create,
               ts::ReadWriteMode::write,
               context)
        .result();
}

template <typename T>
bool writeNumericArray1D(const ts::kvstore::KvStore& store,
                         const ts::Context& context,
                         const std::string& path,
                         const std::string& data_type,
                         const std::vector<T>& values,
                         ts::Index chunk_size,
                         const nlohmann::json& fill_value,
                         bool bytes_endian_little,
                         std::string* error_message) {
    const ts::Index shape_0 = static_cast<ts::Index>(values.size());
    const ts::Index chunk_0 = std::max<ts::Index>(1, chunk_size);
    const nlohmann::json metadata =
        makeNumericArrayMetadata({shape_0}, {chunk_0}, data_type, fill_value,
                                 bytes_endian_little);
    auto open_result = openArrayForWrite<T, 1>(store, path, metadata, context);
    if (!open_result.ok()) {
        if (error_message) {
            *error_message = "Failed to open array '" + path + "' for write: " +
                             open_result.status().ToString();
        }
        return false;
    }

    const ts::Index extents[1] = {shape_0};
    auto source = ts::AllocateArray<T>(extents);
    if (!values.empty()) {
        std::copy(values.begin(), values.end(), source.data());
    }
    auto write_result = ts::Write(source, open_result.value()).commit_future.result();
    if (!write_result.ok()) {
        if (error_message) {
            *error_message =
                "Failed to write array '" + path + "': " +
                write_result.status().ToString();
        }
        return false;
    }
    return true;
}

template <typename T>
bool writeNumericArray2DFlat(const ts::kvstore::KvStore& store,
                             const ts::Context& context,
                             const std::string& path,
                             const std::string& data_type,
                             const std::vector<T>& values,
                             ts::Index rows,
                             ts::Index cols,
                             ts::Index chunk_rows,
                             ts::Index chunk_cols,
                             const nlohmann::json& fill_value,
                             bool bytes_endian_little,
                             std::string* error_message) {
    if (rows < 0 || cols < 0) {
        if (error_message) {
            *error_message =
                "Invalid negative shape for array '" + path + "'";
        }
        return false;
    }
    const size_t expected =
        static_cast<size_t>(rows) * static_cast<size_t>(cols);
    if (values.size() != expected) {
        if (error_message) {
            *error_message = "Shape/value mismatch for array '" + path + "'";
        }
        return false;
    }

    const ts::Index chunk_0 = std::max<ts::Index>(1, chunk_rows);
    const ts::Index chunk_1 = std::max<ts::Index>(1, chunk_cols);
    const nlohmann::json metadata = makeNumericArrayMetadata(
        {rows, cols}, {chunk_0, chunk_1}, data_type, fill_value,
        bytes_endian_little);
    auto open_result = openArrayForWrite<T, 2>(store, path, metadata, context);
    if (!open_result.ok()) {
        if (error_message) {
            *error_message = "Failed to open array '" + path + "' for write: " +
                             open_result.status().ToString();
        }
        return false;
    }

    const ts::Index extents[2] = {rows, cols};
    auto source = ts::AllocateArray<T>(extents);
    if (!values.empty()) {
        std::copy(values.begin(), values.end(), source.data());
    }
    auto write_result = ts::Write(source, open_result.value()).commit_future.result();
    if (!write_result.ok()) {
        if (error_message) {
            *error_message =
                "Failed to write array '" + path + "': " +
                write_result.status().ToString();
        }
        return false;
    }
    return true;
}

template <typename Source>
int32_t convertToInt32WithClamp(Source value, bool& overflow_flag) {
    if constexpr (std::is_same_v<Source, bool>) {
        return value ? 1 : 0;
    } else if constexpr (std::is_signed_v<Source>) {
        int64_t as64 = static_cast<int64_t>(value);
        if (as64 < std::numeric_limits<int32_t>::min()) {
            overflow_flag = true;
            return std::numeric_limits<int32_t>::min();
        }
        if (as64 > std::numeric_limits<int32_t>::max()) {
            overflow_flag = true;
            return std::numeric_limits<int32_t>::max();
        }
        return static_cast<int32_t>(as64);
    } else {
        if (value > static_cast<Source>(std::numeric_limits<int32_t>::max())) {
            overflow_flag = true;
            return std::numeric_limits<int32_t>::max();
        }
        return static_cast<int32_t>(value);
    }
}

template <typename Source>
int64_t convertToInt64WithClamp(Source value, bool& overflow_flag) {
    if constexpr (std::is_same_v<Source, bool>) {
        return value ? 1 : 0;
    } else if constexpr (std::is_signed_v<Source>) {
        return static_cast<int64_t>(value);
    } else {
        if (value > static_cast<Source>(std::numeric_limits<int64_t>::max())) {
            overflow_flag = true;
            return std::numeric_limits<int64_t>::max();
        }
        return static_cast<int64_t>(value);
    }
}
