#include "zarr_loader_internal.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using json = nlohmann::json;

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "[FAIL] " << message << std::endl;
    std::exit(1);
}

void expect(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

json shardedReasonMetadata(ts::Index rows,
                           ts::Index width,
                           ts::Index outer_rows,
                           ts::Index inner_rows) {
    return json{
        {"shape", json::array({rows, width})},
        {"data_type", "uint8"},
        {"chunk_grid",
         {{"name", "regular"},
          {"configuration",
           {{"chunk_shape", json::array({outer_rows, width})}}}}},
        {"chunk_key_encoding",
         {{"name", "default"}, {"configuration", {{"separator", "/"}}}}},
        {"fill_value", 0},
        {"codecs",
         json::array(
             {{{"name", "sharding_indexed"},
               {"configuration",
                {{"chunk_shape", json::array({inner_rows, width})},
                 {"codecs",
                  json::array(
                      {{{"name", "bytes"}},
                       {{"name", "zstd"},
                        {"configuration", {{"level", 0}, {"checksum", false}}}}})},
                 {"index_codecs",
                  json::array(
                      {{{"name", "bytes"},
                        {"configuration", {{"endian", "little"}}}},
                       {{"name", "crc32c"}}})},
                 {"index_location", "end"}}}}})},
        {"attributes", json::object()},
        {"zarr_format", 3},
        {"node_type", "array"},
        {"storage_transformers", json::array()}};
}

bool writeReasonBytes(const ts::kvstore::KvStore& store,
                      const ts::Context& context,
                      const std::string& path,
                      const std::vector<uint8_t>& bytes,
                      ts::Index rows,
                      ts::Index width,
                      bool sharded,
                      std::string& error) {
    json metadata = sharded
                        ? shardedReasonMetadata(rows, width, 8, 2)
                        : makeNumericArrayMetadata(
                              {rows, width}, {std::max<ts::Index>(1, rows), width},
                              "uint8", 0, false);
    auto opened = openArrayForWrite<uint8_t, 2>(
        store, path, metadata, context);
    if (!opened.ok()) {
        error = opened.status().ToString();
        return false;
    }
    const ts::Index extents[2] = {rows, width};
    auto source = ts::AllocateArray<uint8_t>(extents);
    std::copy(bytes.begin(), bytes.end(), source.data());
    auto written = ts::Write(source, opened.value()).commit_future.result();
    if (!written.ok()) {
        error = written.status().ToString();
        return false;
    }
    return true;
}

void putLabel(std::vector<uint8_t>& bytes,
              size_t width,
              size_t row,
              const std::string& label,
              bool terminate = true) {
    expect(label.size() + (terminate ? 1 : 0) <= width,
           "test label exceeds row width");
    const size_t offset = row * width;
    std::copy(label.begin(), label.end(), bytes.begin() + offset);
    if (terminate) {
        bytes[offset + label.size()] = 0;
    }
}

void testDecodeAndEdgeCases(const ts::kvstore::KvStore& store,
                            const ts::Context& context) {
    constexpr size_t rows = 4;
    constexpr size_t width = 65;
    std::vector<uint8_t> bytes(rows * width, 0);
    putLabel(bytes, width, 0, "");
    putLabel(bytes, width, 1, std::string(64, 'x'));
    putLabel(bytes, width, 2, std::string(width, 'n'), false);
    const size_t malformed_offset = 3 * width;
    bytes[malformed_offset + 0] = 'b';
    bytes[malformed_offset + 1] = 0xff;
    bytes[malformed_offset + 2] = 'd';
    bytes[malformed_offset + 3] = 0;

    std::string error;
    expect(writeReasonBytes(store, context, "ordinary/reason_bytes", bytes,
                            rows, width, false, error),
           "ordinary reason_bytes write failed: " + error);
    std::vector<std::string> decoded;
    ReasonBytesDecodeStats stats;
    expect(readReasonBytesArray(store, "ordinary/reason_bytes", context,
                                decoded, &stats, &error),
           "ordinary reason_bytes read failed: " + error);
    expect(decoded.size() == rows, "ordinary decode row count mismatch");
    expect(decoded[0].empty(), "empty reason label changed");
    expect(decoded[1] == std::string(64, 'x'), "maximum-width label changed");
    expect(decoded[2] == std::string(width, 'n'),
           "unterminated row was not decoded through full width");
    expect(decoded[3] == std::string("b") + "\xef\xbf\xbd" + "d",
           "malformed UTF-8 was not replaced deterministically");
    expect(stats.rows_without_null_terminator == 1,
           "unterminated-row diagnostic count mismatch");
    expect(stats.rows_with_malformed_utf8 == 1,
           "malformed-UTF8 diagnostic count mismatch");
}

void testShardedRead(const ts::kvstore::KvStore& store,
                     const ts::Context& context) {
    constexpr size_t rows = 6;
    constexpr size_t width = 16;
    std::vector<uint8_t> bytes(rows * width, 0);
    for (size_t row = 0; row < rows; ++row) {
        putLabel(bytes, width, row, "shard-" + std::to_string(row));
    }
    std::string error;
    expect(writeReasonBytes(store, context, "sharded/reason_bytes", bytes,
                            rows, width, true, error),
           "indexed-sharded reason_bytes write failed: " + error);
    std::vector<std::string> decoded;
    expect(readReasonBytesArray(store, "sharded/reason_bytes", context,
                                decoded, nullptr, &error),
           "indexed-sharded reason_bytes read failed: " + error);
    for (size_t row = 0; row < rows; ++row) {
        expect(decoded[row] == "shard-" + std::to_string(row),
               "indexed-sharded reason row mismatch");
    }
}

void testPrecedence() {
    const std::vector<std::string> canonical = {"", "manual", "custom"};
    const std::vector<std::string> matching = canonical;
    const std::vector<std::string> conflicting = {"legacy", "manual", "old"};
    const std::vector<uint8_t> sources = {0, 1, 0};

    auto resolved = resolveReasonColumns(3, canonical, std::nullopt, &sources);
    expect(resolved.authority == ReasonAuthority::ReasonBytes,
           "reason_bytes-only authority mismatch");
    expect(resolved.labels == canonical, "reason_bytes-only values changed");

    resolved = resolveReasonColumns(3, std::nullopt, matching, &sources);
    expect(resolved.authority == ReasonAuthority::LegacyReason,
           "historical reason-only authority mismatch");
    expect(resolved.labels == matching, "historical reason-only values changed");

    resolved = resolveReasonColumns(3, canonical, matching, &sources);
    expect(resolved.authority == ReasonAuthority::ReasonBytes,
           "matching dual-column authority mismatch");
    expect(resolved.conflicting_legacy_rows == 0,
           "matching dual columns reported a conflict");

    resolved = resolveReasonColumns(3, canonical, conflicting, &sources);
    expect(resolved.labels == canonical,
           "legacy reason overrode conflicting reason_bytes");
    expect(resolved.conflicting_legacy_rows == 2,
           "conflicting dual-column count mismatch");

    resolved = resolveReasonColumns(3, std::nullopt, std::nullopt, &sources);
    expect(resolved.authority == ReasonAuthority::DetectionSource,
           "detection_source fallback authority mismatch");
    expect(resolved.labels ==
               std::vector<std::string>({"clean", "interpolated", "clean"}),
           "detection_source fallback values mismatch");

    const std::vector<std::string> short_column = {"short"};
    resolved = resolveReasonColumns(3, short_column, matching, &sources);
    expect(resolved.authority == ReasonAuthority::LegacyReason,
           "mis-sized canonical column did not fall back to aligned legacy reason");
}

void testWriteMetadataContract() {
    json attrs = json::object();
    setCanonicalReasonAttrs(attrs, 65);
    expect(attrs["reason_encoding"] == "utf8-null-terminated",
           "reason_encoding attr mismatch");
    expect(attrs["reason_authority"] == "reason_bytes",
           "reason_authority attr mismatch");
    expect(attrs["reason_bytes_width"] == 65,
           "reason_bytes_width attr mismatch");
    expect(attrs["reason_bytes_null_terminated"] == true,
           "reason_bytes_null_terminated attr mismatch");
    expect(attrs["reason_fallback_order"] ==
               json::array({"reason_bytes", "detection_source"}),
           "reason_fallback_order attr mismatch");
}

}  // namespace

int main() {
    auto spec = ts::kvstore::Spec::FromJson({{"driver", "memory"}});
    if (!spec.ok()) {
        fail("memory kvstore spec failed: " + spec.status().ToString());
    }
    auto opened = ts::kvstore::Open(spec.value()).result();
    if (!opened.ok()) {
        fail("memory kvstore open failed: " + opened.status().ToString());
    }
    const ts::Context context = ts::Context::Default();

    testDecodeAndEdgeCases(opened.value(), context);
    testShardedRead(opened.value(), context);
    testPrecedence();
    testWriteMetadataContract();

    std::cout << "[PASS] reason_bytes canonical read/write contract" << std::endl;
    return 0;
}
