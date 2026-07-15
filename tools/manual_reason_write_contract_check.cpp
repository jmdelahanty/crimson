#include "zarr_loader_internal.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace {

using json = nlohmann::json;

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "[FAIL] " << message << std::endl;
    std::exit(1);
}

void expect(bool condition, const std::string& message) {
    if (!condition) fail(message);
}

struct FixtureCleanup {
    std::filesystem::path path;
    ~FixtureCleanup() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

json groupMeta(json attrs = json::object()) {
    json meta = makeEmptyGroupMetadataV3();
    meta["attributes"] = std::move(attrs);
    return meta;
}

std::vector<uint8_t> encode(const std::vector<std::string>& labels,
                            size_t width) {
    std::vector<uint8_t> bytes(labels.size() * width, 0);
    for (size_t row = 0; row < labels.size(); ++row) {
        expect(labels[row].size() < width, "fixture reason exceeds width");
        std::copy(labels[row].begin(), labels[row].end(),
                  bytes.begin() + row * width);
    }
    return bytes;
}

void createFixture(const std::filesystem::path& root,
                   const ts::kvstore::KvStore& store,
                   const ts::Context& context) {
    std::string error;
    expect(writeNodeMetaV3(
               store, "",
               groupMeta({{"fps", 30.0},
                          {"total_frames", 2},
                          {"width", 100},
                          {"height", 100}}),
               &error),
           error);
    expect(writeNodeMetaV3(store, "detect_runs",
                           groupMeta({{"latest", "detect_fixture"}}), &error),
           error);
    expect(writeNodeMetaV3(store, "detect_runs/detect_fixture",
                           groupMeta(), &error),
           error);
    const std::vector<int32_t> frames = {0, 1};
    const std::vector<double> boxes = {0.5, 0.5, 0.2, 0.2,
                                       0.6, 0.6, 0.2, 0.2};
    const std::vector<float> scores = {0.9f, 0.8f};
    const std::vector<int32_t> classes = {0, 0};
    const std::vector<int32_t> counts = {1, 1};
    expect(writeNumericArray1D(store, context,
                               "detect_runs/detect_fixture/frame_indices",
                               "int32", frames, 2, 0, true, &error), error);
    expect(writeNumericArray2DFlat(store, context,
                                   "detect_runs/detect_fixture/bbox_norm_coords",
                                   "float64", boxes, 2, 4, 2, 4, 0.0, true,
                                   &error), error);
    expect(writeNumericArray1D(store, context,
                               "detect_runs/detect_fixture/scores",
                               "float32", scores, 2, 0.0f, true, &error), error);
    expect(writeNumericArray1D(store, context,
                               "detect_runs/detect_fixture/class_ids",
                               "int32", classes, 2, 0, true, &error), error);
    expect(writeNumericArray1D(store, context,
                               "detect_runs/detect_fixture/n_detections",
                               "int32", counts, 2, 0, true, &error), error);

    expect(writeNodeMetaV3(
               store, "refined_detect_runs",
               groupMeta({{"latest", "refined_detect_fixture"}}), &error),
           error);
    expect(writeNodeMetaV3(store,
                           "refined_detect_runs/refined_detect_fixture",
                           groupMeta(), &error),
           error);
    expect(writeNodeMetaV3(store,
                           "refined_detect_runs/refined_detect_fixture/manual",
                           groupMeta(), &error),
           error);
    const std::vector<std::string> legacy_labels = {
        "legacy_alpha", "legacy_beta"};
    const auto legacy_bytes = encode(legacy_labels, 16);
    expect(writeNumericArray2DFlat(
               store, context,
               "refined_detect_runs/refined_detect_fixture/manual/reason",
               "uint8", legacy_bytes, 2, 16, 2, 16, 0, false, &error),
           error);
    (void)root;
}

}  // namespace

int main() {
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path fixture =
        std::filesystem::temp_directory_path() /
        ("crimson_manual_reason_contract_" + std::to_string(nonce) + ".zarr");
    FixtureCleanup cleanup{fixture};
    std::filesystem::create_directories(fixture);

    const ts::Context context = ts::Context::Default();
    auto spec = ts::kvstore::Spec::FromJson(
        {{"driver", "file"},
         {"path", normalizeKvstoreFileRootPath(fixture)}});
    expect(spec.ok(), "fixture kvstore spec failed");
    auto opened = ts::kvstore::Open(spec.value(), context).result();
    expect(opened.ok(), "fixture kvstore open failed");
    const auto store = opened.value();
    createFixture(fixture, store, context);

    ZarrDetectionLoader loader;
    std::string error;
    expect(loader.loadZarrFile(fixture.string(), error),
           "fixture load failed: " + error);

    const std::vector<int32_t> frames = {0, 1};
    const std::vector<std::array<double, 4>> boxes = {
        {0.5, 0.5, 0.2, 0.2}, {0.6, 0.6, 0.2, 0.2}};
    const std::vector<float> scores = {0.9f, 0.8f};
    const std::vector<int32_t> classes = {0, 0};
    const std::vector<int32_t> counts = {1, 1};
    const std::vector<int8_t> sources = {0, 1};
    expect(loader.writeManualRefinedDetections(
               frames, boxes, scores, classes, counts, sources,
               {}, "manual", "interpolated", error),
           "manual write failed: " + error);

    const std::string manual_base =
        "refined_detect_runs/refined_detect_fixture/manual/";
    expect(arrayExists(store, manual_base + "reason_bytes"),
           "manual write did not create reason_bytes");
    expect(!arrayExists(store, manual_base + "reason"),
           "manual write retained or recreated legacy reason");
    std::vector<std::string> persisted;
    expect(readReasonBytesArray(store, manual_base + "reason_bytes", context,
                                persisted, nullptr, &error),
           "persisted reason_bytes reload failed: " + error);
    expect(persisted ==
               std::vector<std::string>({"legacy_alpha", "legacy_beta"}),
           "legacy effective labels were not materialized losslessly");

    const auto attrs = readAttrsAny(store, manual_base);
    expect(attrs.has_value(), "manual group attrs missing");
    expect((*attrs)["reason_authority"] == "reason_bytes",
           "manual reason_authority mismatch");
    expect((*attrs)["reason_fallback_order"] ==
               json::array({"reason_bytes", "detection_source"}),
           "manual reason_fallback_order mismatch");

    for (const auto& entry : std::filesystem::directory_iterator(
             fixture / "refined_detect_runs" / "refined_detect_fixture")) {
        const std::string name = entry.path().filename().string();
        expect(name.find(".__crimson_staging_") == std::string::npos,
               "staging directory remained after successful write");
        expect(name.find(".__crimson_backup_") == std::string::npos,
               "backup directory remained after successful write");
    }

    ZarrDetectionLoader reloaded;
    expect(reloaded.loadZarrFile(fixture.string(), error),
           "reload after manual edit failed: " + error);
    const auto frame_zero = reloaded.getRawDetections(0, true);
    const auto frame_one = reloaded.getRawDetections(1, true);
    expect(frame_zero.detection_reason ==
               std::vector<std::string>({"legacy_alpha"}),
           "frame 0 reason did not survive reload");
    expect(frame_one.detection_reason ==
               std::vector<std::string>({"legacy_beta"}),
           "frame 1 reason did not survive reload");

    std::cout << "[PASS] manual detection reason_bytes migration and reload"
              << std::endl;
    return 0;
}
