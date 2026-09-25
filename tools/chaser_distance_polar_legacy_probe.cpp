#include "zarr_loader.h"
#include "chaser_distance_polar.h"
#include "zarr/archive_context.h"
#include "zarr/chaser_distance_polar_legacy_repository.h"
#include "zarr/tensorstore_chaser_distance_polar_repository.h"

#include <nlohmann/json.hpp>
#include <tensorstore/array.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

namespace {
namespace ts = tensorstore;
using json = nlohmann::json;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__    \
                      << ": " #condition << '\n';                             \
            return false;                                                      \
        }                                                                      \
    } while (false)

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(std::string label) {
        const auto seed =
            std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 100; ++attempt) {
            path_ = std::filesystem::temp_directory_path() /
                    ("crimson-polar-" + label + "-" +
                     std::to_string(seed) + "-" + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                return;
            }
        }
        path_.clear();
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

bool writeJson(const std::filesystem::path& path, const json& value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output << value.dump(2) << '\n';
    return output.good();
}

bool writeGroup(const std::filesystem::path& root, const std::string& path,
                const json& attributes = json::object()) {
    json metadata = {{"zarr_format", 3}, {"node_type", "group"}};
    if (!attributes.empty()) {
        metadata["attributes"] = attributes;
    }
    return writeJson(root / path / "zarr.json", metadata);
}

template <typename T, size_t Rank>
bool writeArray(const std::filesystem::path& root, const std::string& path,
                const std::string& data_type,
                const std::array<ts::Index, Rank>& shape,
                const std::vector<T>& values) {
    size_t count = 1;
    json shape_json = json::array();
    json chunk_json = json::array();
    for (const ts::Index extent : shape) {
        count *= static_cast<size_t>(extent);
        shape_json.push_back(extent);
        chunk_json.push_back(std::max<ts::Index>(1, extent));
    }
    if (count != values.size()) {
        return false;
    }

    json bytes = {{"name", "bytes"}};
    if (sizeof(T) > 1) {
        bytes["configuration"] = {{"endian", "little"}};
    }
    const json fill_value = std::is_same_v<T, bool> ? json(false) : json(0);
    const json spec = {
        {"driver", "zarr3"},
        {"kvstore", {{"driver", "file"}, {"path", root.string() + "/"}}},
        {"path", path},
        {"metadata",
         {{"shape", shape_json},
          {"data_type", data_type},
          {"chunk_grid",
           {{"name", "regular"},
            {"configuration", {{"chunk_shape", chunk_json}}}}},
          {"chunk_key_encoding",
           {{"name", "default"},
            {"configuration", {{"separator", "/"}}}}},
          {"fill_value", fill_value},
          {"codecs", json::array({bytes})}}}};

    auto store =
        ts::Open<T, Rank>(spec, ts::OpenMode::open | ts::OpenMode::create,
                          ts::ReadWriteMode::read_write)
            .result();
    if (!store.ok()) {
        std::cerr << "Failed to create " << path << ": " << store.status()
                  << '\n';
        return false;
    }
    auto source = ts::AllocateArray<T>(shape);
    std::copy(values.begin(), values.end(), source.data());
    const auto write = ts::Write(source, *store).commit_future.result();
    if (!write.ok()) {
        std::cerr << "Failed to write " << path << ": " << write.status()
                  << '\n';
        return false;
    }
    return true;
}

bool writeRoot(const std::filesystem::path& root, size_t total_frames = 100) {
    return writeGroup(root, "", {{"total_frames", total_frames},
                                  {"video_width", 640},
                                  {"video_height", 360},
                                  {"fps", 100.0}});
}

bool writeProtocolColorFixture(const std::filesystem::path& root) {
    const std::string group = "analysis/stimulus_runs";
    const std::string run = group + "/stimulus_fixture";
    const json protocol = {
        {"chasers",
         json::array({{{"chaser_index", 0},
                       {"color_r", 0.25},
                       {"color_g", 0.5},
                       {"color_b", 0.75},
                       {"color_a", 1.0}}})}};
    return writeGroup(root, group, {{"latest", "stimulus_fixture"}}) &&
           writeGroup(root, run, {{"protocol_json", protocol.dump()}});
}

bool writePolarComponent(
    const std::filesystem::path& root, const std::string& run_name,
    const std::string& component_name, const std::vector<int64_t>& frames,
    const std::vector<int32_t>& chaser_indices,
    const std::vector<float>& bearings, const std::vector<float>& distances,
    const std::vector<bool>& valid, const json& component_attributes) {
    const std::string run = "analysis/chaser_distance_runs/" + run_name;
    const std::string component_group = run + "/egocentric_bearing";
    const std::string component = component_group + "/" + component_name;
    const size_t rows = frames.size();
    const size_t columns = chaser_indices.size();
    if (rows * columns != bearings.size() ||
        rows * columns != distances.size() || rows * columns != valid.size()) {
        return false;
    }
    std::vector<uint8_t> stored_chaser_indices;
    stored_chaser_indices.reserve(chaser_indices.size());
    std::transform(chaser_indices.begin(), chaser_indices.end(),
                   std::back_inserter(stored_chaser_indices),
                   [](int32_t value) {
                       return static_cast<uint8_t>(value);
                   });
    return writeGroup(root, component, component_attributes) &&
           writeArray<int64_t, 1>(root, component + "/frames/camera_frame_id",
                                  "int64", {static_cast<ts::Index>(rows)},
                                  frames) &&
           writeArray<uint8_t, 1>(root,
                                  component + "/per_chaser/chaser_index",
                                  "uint8",
                                  {static_cast<ts::Index>(columns)},
                                  stored_chaser_indices) &&
           writeArray<float, 2>(
               root, component + "/per_chaser/bearing_deg", "float32",
               {static_cast<ts::Index>(rows),
                static_cast<ts::Index>(columns)},
               bearings) &&
           writeArray<float, 2>(
               root, component + "/per_chaser/distance_mm", "float32",
               {static_cast<ts::Index>(rows),
                static_cast<ts::Index>(columns)},
               distances) &&
           writeArray<bool, 2>(
               root, component + "/per_chaser/valid", "bool",
               {static_cast<ts::Index>(rows),
                static_cast<ts::Index>(columns)},
               valid);
}

bool writeSelectedFixture(const std::filesystem::path& root) {
    CHECK(writeRoot(root));
    CHECK(writeProtocolColorFixture(root));

    const std::string group = "analysis/chaser_distance_runs";
    const std::string run = group + "/run_selected";
    const std::string components = run + "/egocentric_bearing";
    CHECK(writeGroup(root, group,
                     {{"latest_complete", "run_selected"},
                      {"latest", "run_ignored"}}));
    CHECK(writeGroup(root, run,
                     {{"coordinate_frame",
                       std::string(crimson::polar::
                                       kArenaRelativeCanvasPixelFrame)}}));
    CHECK(writeGroup(root, components,
                     {{"latest_completed", "component_selected"},
                      {"latest", "component_ignored"}}));

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::vector<int64_t> frames = {10, 20, 20, 30, 40};
    const std::vector<int32_t> chasers = {0, 1, 2};
    const std::vector<float> bearings = {
        0.0f, 90.0f, 45.0f,
        180.0f, -45.0f, 30.0f,
        10.0f, 20.0f, 30.0f,
        0.0f, 0.0f, -90.0f,
        0.0f, 0.0f, 0.0f,
    };
    const std::vector<float> distances = {
        10.0f, 20.0f, 999.0f,
        30.0f, 60.0f, nan,
        50.0f, 55.0f, 58.0f,
        0.0f, 0.0f, 40.0f,
        70.0f, 80.0f, 90.0f,
    };
    const std::vector<bool> valid = {
        true, true, false,
        false, true, true,
        true, true, true,
        false, false, true,
        false, false, false,
    };
    const json component_attributes = {
        {"parameters",
         {{"angle_convention",
           std::string(crimson::polar::
                           kPositiveAnatomicalLeftAngleConvention)}}},
        {"summary",
         {{"chaser_color_hex", {{"0", "#ff0000"},
                                  {"1", "#00ff00"}}}}},
    };
    CHECK(writePolarComponent(root, "run_selected", "component_selected",
                              frames, chasers, bearings, distances, valid,
                              component_attributes));
    return true;
}

bool writeDiscoveryMarker(const std::filesystem::path& root,
                          const std::string& run_name) {
    const std::string run = "analysis/chaser_distance_runs/" + run_name;
    return writeGroup(root, run) &&
           writeArray<int64_t, 1>(root, run + "/frames/camera_frame_id",
                                  "int64", {1}, {5}) &&
           writeArray<float, 1>(root, run + "/distances/distance_mm",
                                "float32", {1}, {1.0f});
}

bool writeFallbackFixture(const std::filesystem::path& root) {
    CHECK(writeRoot(root));
    const std::string group = "analysis/chaser_distance_runs";
    CHECK(writeGroup(root, group));
    CHECK(writeDiscoveryMarker(root, "run_a"));
    CHECK(writeDiscoveryMarker(root, "run_z"));
    CHECK(writeGroup(root, group + "/run_z",
                     {{"coordinate_frame",
                       std::string(crimson::polar::
                                       kArenaRelativeCanvasPixelFrame)}}));

    const std::string components = group + "/run_z/egocentric_bearing";
    CHECK(writeGroup(root, components));
    const std::vector<int64_t> frames = {5};
    const std::vector<int32_t> chasers = {7};
    const std::vector<float> bearing = {12.0f};
    const std::vector<float> distance = {34.0f};
    const std::vector<bool> valid = {true};
    const json attributes = {
        {"angle_convention",
         std::string(crimson::polar::
                         kPositiveAnatomicalLeftAngleConvention)}};
    CHECK(writePolarComponent(root, "run_z", "component_a", frames, chasers,
                              bearing, distance, valid, attributes));
    CHECK(writePolarComponent(root, "run_z", "component_z", frames, chasers,
                              bearing, distance, valid, attributes));
    return true;
}

bool near(float actual, float expected, float tolerance = 1e-5f) {
    return std::abs(actual - expected) <= tolerance;
}

bool checkColor(const std::array<float, 4>& actual,
                const std::array<float, 4>& expected) {
    for (size_t channel = 0; channel < actual.size(); ++channel) {
        if (!near(actual[channel], expected[channel])) {
            return false;
        }
    }
    return true;
}

bool nearDouble(double actual, double expected, double tolerance = 1e-5) {
    return std::abs(actual - expected) <= tolerance;
}

json descriptorJson(
    const crimson::polar::ChaserDistancePolarDescriptor& descriptor) {
    return {
        {"availability",
         crimson::polar::chaserDistancePolarAvailabilityName(
             descriptor.availability)},
        {"source_group", descriptor.provenance.source_group},
        {"run_name", descriptor.provenance.run_name},
        {"component_name", descriptor.provenance.component_name},
        {"run_selection",
         crimson::polar::chaserDistancePolarSelectionProvenanceName(
             descriptor.provenance.run_selection)},
        {"component_selection",
         crimson::polar::chaserDistancePolarSelectionProvenanceName(
             descriptor.provenance.component_selection)},
        {"row_count", descriptor.row_count},
        {"chaser_count", descriptor.chaser_count},
        {"distance_unit",
         crimson::polar::chaserDistancePolarDistanceUnitName(
             descriptor.distance_unit)},
        {"coordinate_frame", descriptor.coordinate_frame},
        {"angle_convention", descriptor.angle_convention},
        {"normalized_coordinate_frame",
         static_cast<int>(descriptor.normalized_coordinate_frame)},
        {"normalized_angle_convention",
         static_cast<int>(descriptor.normalized_angle_convention)},
        {"dataset_global_max_distance_mm",
         descriptor.dataset_global_max_distance_mm},
        {"radial_scale_provenance",
         static_cast<int>(descriptor.radial_scale.provenance)},
        {"radial_data_max_distance_mm",
         descriptor.radial_scale.data_max_distance_mm},
        {"radial_display_max_distance_mm",
         descriptor.radial_scale.display_max_distance_mm},
        {"error", descriptor.error},
    };
}

bool descriptorsAgree(
    const crimson::polar::ChaserDistancePolarDescriptor& left,
    const crimson::polar::ChaserDistancePolarDescriptor& right) {
    return left.availability == right.availability &&
           left.provenance.source_group == right.provenance.source_group &&
           left.provenance.run_name == right.provenance.run_name &&
           left.provenance.component_name == right.provenance.component_name &&
           left.provenance.run_selection == right.provenance.run_selection &&
           left.provenance.component_selection ==
               right.provenance.component_selection &&
           left.row_count == right.row_count &&
           left.chaser_count == right.chaser_count &&
           left.distance_unit == right.distance_unit &&
           left.coordinate_frame == right.coordinate_frame &&
           left.angle_convention == right.angle_convention &&
           left.normalized_coordinate_frame ==
               right.normalized_coordinate_frame &&
           left.normalized_angle_convention ==
               right.normalized_angle_convention &&
           nearDouble(left.dataset_global_max_distance_mm,
                      right.dataset_global_max_distance_mm) &&
           left.radial_scale.provenance == right.radial_scale.provenance &&
           nearDouble(left.radial_scale.data_max_distance_mm,
                      right.radial_scale.data_max_distance_mm) &&
           nearDouble(left.radial_scale.display_max_distance_mm,
                      right.radial_scale.display_max_distance_mm) &&
           left.error == right.error;
}

bool samplesAgree(
    const crimson::polar::ChaserDistancePolarFrameSample& left,
    const crimson::polar::ChaserDistancePolarFrameSample& right) {
    if (left.availability != right.availability ||
        left.requested_camera_frame != right.requested_camera_frame ||
        left.source_camera_frame != right.source_camera_frame ||
        left.provenance.source_group != right.provenance.source_group ||
        left.provenance.run_name != right.provenance.run_name ||
        left.provenance.component_name != right.provenance.component_name ||
        left.provenance.run_selection != right.provenance.run_selection ||
        left.provenance.component_selection !=
            right.provenance.component_selection ||
        left.distance_unit != right.distance_unit ||
        left.coordinate_frame != right.coordinate_frame ||
        left.angle_convention != right.angle_convention ||
        left.normalized_coordinate_frame !=
            right.normalized_coordinate_frame ||
        left.normalized_angle_convention !=
            right.normalized_angle_convention ||
        left.radial_scale.provenance != right.radial_scale.provenance ||
        !nearDouble(left.radial_scale.data_max_distance_mm,
                    right.radial_scale.data_max_distance_mm) ||
        !nearDouble(left.radial_scale.display_max_distance_mm,
                    right.radial_scale.display_max_distance_mm) ||
        left.source_point_count != right.source_point_count ||
        left.discarded_point_count != right.discarded_point_count ||
        left.points.size() != right.points.size() || left.error != right.error) {
        return false;
    }
    for (size_t index = 0; index < left.points.size(); ++index) {
        const auto& a = left.points[index];
        const auto& b = right.points[index];
        if (a.chaser_index != b.chaser_index ||
            !nearDouble(a.distance_mm, b.distance_mm) ||
            !nearDouble(a.bearing_degrees, b.bearing_degrees) ||
            a.valid != b.valid ||
            a.color.provenance != b.color.provenance ||
            !nearDouble(a.color.rgba.red, b.color.rgba.red) ||
            !nearDouble(a.color.rgba.green, b.color.rgba.green) ||
            !nearDouble(a.color.rgba.blue, b.color.rgba.blue) ||
            !nearDouble(a.color.rgba.alpha, b.color.rgba.alpha)) {
            return false;
        }
    }
    return true;
}

bool compareAdapters(const std::filesystem::path& archive_path,
                     const ZarrDetectionLoader& loader,
                     const std::vector<int64_t>& frames) {
    auto legacy =
        crimson::zarr::MakeLegacyChaserDistancePolarRepository(loader);
    CHECK(legacy != nullptr);
    std::string error;
    auto archive =
        crimson::zarr::ArchiveContext::Open(archive_path, &error);
    CHECK(archive != nullptr);
    auto tensorstore =
        crimson::zarr::OpenTensorStoreChaserDistancePolarRepository(
            archive, {}, &error);
    CHECK(tensorstore != nullptr);
    if (!descriptorsAgree(legacy->descriptor(), tensorstore->descriptor())) {
        std::cerr << "polar adapter descriptors differ for " << archive_path
                  << "\nlegacy: "
                  << descriptorJson(legacy->descriptor()).dump(2)
                  << "\ntensorstore: "
                  << descriptorJson(tensorstore->descriptor()).dump(2)
                  << '\n';
        return false;
    }
    for (const int64_t frame : frames) {
        const auto legacy_sample = legacy->resolveCameraFrame(frame);
        const auto tensorstore_sample =
            tensorstore->resolveCameraFrame(frame);
        if (!samplesAgree(legacy_sample, tensorstore_sample)) {
            std::cerr << "polar adapter samples differ at camera frame "
                      << frame << '\n';
            return false;
        }
    }
    return true;
}

bool testSelectedFixture() {
    TemporaryDirectory temporary("selected");
    CHECK(!temporary.path().empty());
    CHECK(writeSelectedFixture(temporary.path()));

    ZarrDetectionLoader loader;
    std::string error;
    CHECK(loader.loadZarrFile(temporary.path().string(), error));
    CHECK(loader.hasChaserDistancePolarData());
    CHECK(loader.getChaserDistancePolarRunName() == "run_selected");
    CHECK(loader.getChaserDistancePolarComponentName() ==
          "component_selected");
    CHECK(loader.getChaserDistancePolarCoordinateFrame() ==
          crimson::polar::kArenaRelativeCanvasPixelFrame);
    CHECK(loader.getChaserDistancePolarAngleConvention() ==
          crimson::polar::kPositiveAnatomicalLeftAngleConvention);
    CHECK(loader.getChaserDistancePolarRunSelection() == "latest_complete");
    CHECK(loader.getChaserDistancePolarComponentSelection() ==
          "latest_completed");
    CHECK(loader.getChaserDistancePolarRowCount() == 5);
    CHECK(loader.getChaserDistancePolarChaserCount() == 3);

    const auto frame10 = loader.getChaserDistancePolarFrame(10);
    CHECK(frame10.available);
    CHECK(frame10.camera_frame_id == 10);
    CHECK(near(frame10.radial_max_mm, 60.0f));
    CHECK(frame10.points.size() == 2);
    CHECK(frame10.points[0].chaser_index == 0);
    CHECK(frame10.points[0].color_provenance == "stimulus_protocol");
    CHECK(checkColor(frame10.points[0].rgba,
                     {0.25f, 0.5f, 0.75f, 1.0f}));
    CHECK(frame10.points[1].chaser_index == 1);
    CHECK(frame10.points[1].color_provenance == "component_summary");
    CHECK(checkColor(frame10.points[1].rgba,
                     {0.0f, 1.0f, 0.0f, 1.0f}));

    const auto duplicate20 = loader.getChaserDistancePolarFrame(20);
    if (duplicate20.points.size() != 1) {
        std::cerr << "frame 20 points:";
        for (const auto& point : duplicate20.points) {
            std::cerr << " [chaser=" << point.chaser_index
                      << " bearing=" << point.bearing_deg
                      << " distance=" << point.distance_mm << ']';
        }
        std::cerr << '\n';
    }
    CHECK(duplicate20.points.size() == 1);
    CHECK(duplicate20.points[0].chaser_index == 1);
    CHECK(near(duplicate20.points[0].distance_mm, 60.0f));
    CHECK(near(duplicate20.points[0].bearing_deg, -45.0f));

    const auto frame30 = loader.getChaserDistancePolarFrame(30);
    CHECK(frame30.points.size() == 1);
    CHECK(frame30.points[0].chaser_index == 2);
    CHECK(frame30.points[0].color_provenance ==
          "fixed_fallback_palette");
    CHECK(checkColor(frame30.points[0].rgba,
                     {0.10f, 0.78f, 0.32f, 1.0f}));

    const auto missing15 = loader.getChaserDistancePolarFrame(15);
    const auto validEmpty40 = loader.getChaserDistancePolarFrame(40);
    CHECK(missing15.available);
    CHECK(missing15.points.empty());
    CHECK(validEmpty40.available);
    CHECK(validEmpty40.points.empty());
    CHECK(compareAdapters(temporary.path(), loader, {10, 20, 15, 30, 40}));
    return true;
}

bool testFilesystemFallback() {
    TemporaryDirectory temporary("fallback");
    CHECK(!temporary.path().empty());
    CHECK(writeFallbackFixture(temporary.path()));

    ZarrDetectionLoader loader;
    std::string error;
    CHECK(loader.loadZarrFile(temporary.path().string(), error));
    CHECK(loader.hasChaserDistancePolarData());
    CHECK(loader.getChaserDistancePolarRunName() == "run_z");
    CHECK(loader.getChaserDistancePolarComponentName() == "component_z");
    CHECK(loader.getChaserDistancePolarRunSelection() ==
          "lexicographic_compatibility_fallback");
    CHECK(loader.getChaserDistancePolarComponentSelection() ==
          "lexicographic_compatibility_fallback");
    const auto frame = loader.getChaserDistancePolarFrame(5);
    CHECK(frame.points.size() == 1);
    CHECK(frame.points[0].chaser_index == 7);
    CHECK(compareAdapters(temporary.path(), loader, {5, 6}));
    return true;
}

bool testUnavailableFixture() {
    TemporaryDirectory temporary("unavailable");
    CHECK(!temporary.path().empty());
    CHECK(writeRoot(temporary.path()));

    ZarrDetectionLoader loader;
    std::string error;
    CHECK(loader.loadZarrFile(temporary.path().string(), error));
    CHECK(!loader.hasChaserDistancePolarData());
    CHECK(!loader.getChaserDistancePolarFrame(10).available);
    CHECK(compareAdapters(temporary.path(), loader, {10}));
    return true;
}

json pointJson(const ZarrDetectionLoader::ChaserDistancePolarPoint& point) {
    return {{"chaser_index", point.chaser_index},
            {"distance_mm", point.distance_mm},
            {"bearing_deg", point.bearing_deg},
            {"rgba", point.rgba},
            {"has_rgba", point.has_rgba},
            {"color_provenance", point.color_provenance}};
}

int runProductionComparison(
    const std::filesystem::path& archive,
    const std::vector<int64_t>& requested_frames) {
    ZarrDetectionLoader loader;
    std::string error;
    if (!loader.loadZarrFile(archive.string(), error)) {
        std::cerr << "load failed: " << error << '\n';
        return 2;
    }
    if (!compareAdapters(archive, loader, requested_frames)) {
        return 3;
    }
    auto repository =
        crimson::zarr::MakeLegacyChaserDistancePolarRepository(loader);
    const auto& descriptor = repository->descriptor();
    std::cout << "chaser_distance_polar_adapter_compare: PASS"
              << " run=" << descriptor.provenance.run_name
              << " component=" << descriptor.provenance.component_name
              << " rows=" << descriptor.row_count
              << " chasers=" << descriptor.chaser_count
              << " max_mm=" << descriptor.dataset_global_max_distance_mm
              << " frames=" << requested_frames.size() << '\n';
    return 0;
}

int writeProductionProbe(const std::filesystem::path& archive,
                         const std::filesystem::path& output_path,
                         const std::vector<int64_t>& requested_frames) {
    ZarrDetectionLoader loader;
    std::string error;
    if (!loader.loadZarrFile(archive.string(), error)) {
        std::cerr << "load failed: " << error << '\n';
        return 2;
    }

    json frames = json::array();
    for (const int64_t requested_frame : requested_frames) {
        const auto frame = loader.getChaserDistancePolarFrame(requested_frame);
        json points = json::array();
        for (const auto& point : frame.points) {
            points.push_back(pointJson(point));
        }
        frames.push_back({{"requested_camera_frame", requested_frame},
                          {"available", frame.available},
                          {"returned_camera_frame", frame.camera_frame_id},
                          {"run_name", frame.run_name},
                          {"component_name", frame.component_name},
                          {"radial_max_mm", frame.radial_max_mm},
                          {"points", std::move(points)}});
    }

    const json report = {
        {"schema", "crimson.phase5m.legacy-polar-probe.v1"},
        {"archive", std::filesystem::absolute(archive).string()},
        {"loaded", true},
        {"descriptor",
         {{"available", loader.hasChaserDistancePolarData()},
          {"run_name", loader.getChaserDistancePolarRunName()},
          {"component_name", loader.getChaserDistancePolarComponentName()},
          {"run_selection", loader.getChaserDistancePolarRunSelection()},
          {"component_selection",
           loader.getChaserDistancePolarComponentSelection()},
          {"coordinate_frame",
           loader.getChaserDistancePolarCoordinateFrame()},
          {"angle_convention",
           loader.getChaserDistancePolarAngleConvention()},
          {"row_count", loader.getChaserDistancePolarRowCount()},
          {"chaser_count", loader.getChaserDistancePolarChaserCount()}}},
        {"frames", std::move(frames)},
        {"legacy_semantics",
         {{"missing_frame_reports_available_when_dataset_loaded", true},
          {"duplicate_camera_frame_policy", "first_row_wins"},
          {"radial_scale", "global_max_valid_distance_mm"},
          {"color_precedence",
           json::array({"stimulus_protocol", "component_summary",
                        "fixed_fallback_palette"})}}},
    };
    if (!output_path.parent_path().empty()) {
        std::filesystem::create_directories(output_path.parent_path());
    }
    std::ofstream output(output_path);
    output << report.dump(2) << '\n';
    if (!output.good()) {
        std::cerr << "could not write " << output_path << '\n';
        return 3;
    }
    std::cout << "wrote " << output_path << '\n';
    return 0;
}

int runSelfTest() {
    if (!testSelectedFixture() || !testFilesystemFallback() ||
        !testUnavailableFixture()) {
        return 1;
    }
    std::cout << "chaser_distance_polar_legacy_probe: PASS" << '\n';
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--self-test") {
        return runSelfTest();
    }
    if (argc >= 4 && std::string(argv[1]) == "--probe") {
        std::vector<int64_t> frames;
        for (int index = 4; index < argc; ++index) {
            try {
                frames.push_back(std::stoll(argv[index]));
            } catch (const std::exception&) {
                std::cerr << "invalid camera frame: " << argv[index] << '\n';
                return 1;
            }
        }
        if (frames.empty()) {
            frames = {0, 56, 1024, 7024, 140034};
        }
        return writeProductionProbe(argv[2], argv[3], frames);
    }
    if (argc >= 3 && std::string(argv[1]) == "--compare") {
        std::vector<int64_t> frames;
        for (int index = 3; index < argc; ++index) {
            try {
                frames.push_back(std::stoll(argv[index]));
            } catch (const std::exception&) {
                std::cerr << "invalid camera frame: " << argv[index] << '\n';
                return 1;
            }
        }
        if (frames.empty()) {
            frames = {0, 56, 1024, 7024, 140034};
        }
        return runProductionComparison(argv[2], frames);
    }

    std::cerr << "Usage:\n  " << argv[0]
              << " --self-test\n  " << argv[0]
              << " --probe <analysis.zarr> <output.json> [camera_frame ...]\n  "
              << argv[0]
              << " --compare <analysis.zarr> [camera_frame ...]\n";
    return 1;
}
