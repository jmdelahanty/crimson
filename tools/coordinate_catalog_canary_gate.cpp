#include "coordinate_contract.h"
#include "zarr/archive_context.h"
#include "zarr/canonical_json.h"
#include "zarr/coordinate_catalog_contract.h"
#include "zarr/tensorstore_analysis_crop_geometry_repository.h"
#include "zarr/tensorstore_canonical_detection_repository.h"
#include "zarr/tensorstore_refined_detection_repository.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#ifndef CRIMSON_GIT_COMMIT
#define CRIMSON_GIT_COMMIT "unknown"
#endif

#ifndef CRIMSON_WORKTREE_DIRTY
#define CRIMSON_WORKTREE_DIRTY 1
#endif

namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr char kArtifactPaletteCommit[] =
    "7a710276beea3037a457f4bfbb5be9f0525de0dc";
constexpr char kCheckpointPaletteCommit[] =
    "1dd7a8f9589dcbf8831b385a5ce36c41b428b85e";

void require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

double elapsedMilliseconds(Clock::time_point started) {
  return std::chrono::duration<double, std::milli>(Clock::now() - started)
      .count();
}

std::pair<std::string, json> readJson(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  require(input.good(), "Could not open JSON document: " + path.string());
  std::ostringstream payload;
  payload << input.rdbuf();
  require(input.good() || input.eof(),
          "Could not read JSON document: " + path.string());
  try {
    return {payload.str(), json::parse(payload.str())};
  } catch (const json::exception &exception) {
    throw std::runtime_error("Could not parse " + path.string() + ": " +
                             exception.what());
  }
}

void writeJson(const std::filesystem::path &path, const json &value) {
  std::ofstream output(path);
  require(output.good(), "Could not open evidence output: " + path.string());
  output << value.dump(2) << '\n';
  require(output.good(), "Could not write evidence output: " + path.string());
}

bool near(double left, double right, double tolerance = 1e-3) {
  return std::abs(left - right) <= tolerance;
}

double maximumError(const std::array<double, 4> &observed,
                    const std::array<double, 4> &expected) {
  double result = 0.0;
  for (size_t index = 0; index < observed.size(); ++index) {
    result = std::max(result, std::abs(observed[index] - expected[index]));
  }
  return result;
}

template <size_t N> std::array<double, N> doubleArray(const json &value) {
  require(value.is_array() && value.size() == N,
          "Coordinate sample has an invalid vector shape");
  std::array<double, N> result{};
  for (size_t index = 0; index < N; ++index) {
    result[index] = value[index].get<double>();
  }
  return result;
}

struct Artifact {
  std::filesystem::path archive;
  std::string run;
  std::string manifest_digest;
  std::string coordinate_digest;
  size_t frames = 0;
  size_t rows = 0;
  size_t source_width = 0;
  size_t source_height = 0;
};

struct Handoff {
  Artifact canonical;
  Artifact refined;
  Artifact crop;
  json samples;
  std::string crop_policy_digest;
  std::string pixel_authority_digest;
  std::string payload_digest;
  std::string artifact_palette_commit;
};

Artifact readArtifact(const json &value) {
  const auto &dimensions = value.at("dimensions");
  Artifact result;
  result.archive = value.at("macos_path").get<std::string>();
  result.run = value.at("run_id").get<std::string>();
  result.manifest_digest = value.at("manifest_digest").get<std::string>();
  result.coordinate_digest =
      value.at("coordinate_catalog_digest").get<std::string>();
  result.frames = dimensions.at("n_frames").get<size_t>();
  result.rows = dimensions.at("n_instances").get<size_t>();
  result.source_width = dimensions.at("source_width").get<size_t>();
  result.source_height = dimensions.at("source_height").get<size_t>();
  require(dimensions.at("n_frame_boundaries").get<size_t>() ==
              result.frames + 1,
          "Handoff frame boundary count is invalid");
  return result;
}

Handoff validateHandoff(const json &document,
                        const std::string &expected_file_digest) {
  require(document.value("schema_id", "") ==
                  "palette.coordinate_catalog.crimson_canary_handoff" &&
              document.value("schema_version", 0) == 1 &&
              document.value("payload_digest_algorithm", "") ==
                  "sha256_canonical_json_v1",
          "Coordinate-catalog handoff envelope is incompatible");
  const std::string payload_digest = document.value("payload_digest", "");
  require(crimson::zarr::IsLowerSha256(expected_file_digest) &&
              crimson::zarr::IsLowerSha256(payload_digest) &&
              crimson::zarr::CanonicalJsonSha256(document.at("payload")) ==
                  payload_digest,
          "Coordinate-catalog handoff digest is invalid");
  const auto &payload = document.at("payload");
  require(payload.value("status", "") == "complete" &&
              payload.value("purpose", "") ==
                  "crimson_coordinate_catalog_archive_gate" &&
              payload.value("benchmark_only", false) &&
              !payload.value("selector_eligible", true) &&
              !payload.value("registry_registered", true) &&
              payload.at("production_state_changes").empty(),
          "Coordinate-catalog handoff publication state is invalid");
  const auto &validation = payload.at("validation");
  require(
      validation.value("strict_json", false) &&
          validation.value("direct_consolidated_metadata_equivalence", false) &&
          validation.value("exact_coordinate_catalog_digests", false) &&
          validation.value("source_metadata_unchanged", false),
      "Coordinate-catalog producer validation is incomplete");

  Handoff result;
  result.payload_digest = payload_digest;
  result.artifact_palette_commit =
      payload.at("palette").at("commit").get<std::string>();
  require(result.artifact_palette_commit == kArtifactPaletteCommit,
          "Handoff was produced by an unexpected Palette revision");
  result.canonical = readArtifact(payload.at("artifacts").at("canonical"));
  result.refined = readArtifact(payload.at("artifacts").at("refined"));
  result.crop = readArtifact(payload.at("artifacts").at("crop"));
  result.samples = payload.at("coordinate_samples");
  const auto &lineage = payload.at("lineage_bindings");
  result.crop_policy_digest =
      lineage.at("crop_policy_digest").get<std::string>();
  result.pixel_authority_digest =
      lineage.at("crop_pixel_authority_manifest_digest").get<std::string>();
  require(
      result.canonical.coordinate_digest ==
              crimson::zarr::ExpectedCoordinateCatalogDigest(
                  crimson::zarr::CoordinateCatalogStage::CanonicalDetection) &&
          result.refined.coordinate_digest ==
              crimson::zarr::ExpectedCoordinateCatalogDigest(
                  crimson::zarr::CoordinateCatalogStage::RefinedDetection) &&
          result.crop.coordinate_digest ==
              crimson::zarr::ExpectedCoordinateCatalogDigest(
                  crimson::zarr::CoordinateCatalogStage::GeometryOnlyCrop) &&
          lineage.at("refined_source_canonical_manifest_digest") ==
              result.canonical.manifest_digest &&
          lineage.at("crop_source_refined_manifest_digest") ==
              result.refined.manifest_digest,
      "Coordinate catalog or cross-stage lineage binding is inconsistent");
  return result;
}

json validateCanonical(const Handoff &handoff) {
  std::string error;
  const auto started = Clock::now();
  auto archive =
      crimson::zarr::ArchiveContext::Open(handoff.canonical.archive, &error);
  require(archive != nullptr, "Canonical archive open failed: " + error);
  crimson::zarr::CanonicalDetectionRepositoryOpenMetrics metrics;
  auto repository = crimson::zarr::OpenCanonicalDetectionRepository(
      archive, handoff.canonical.run, &error, &metrics);
  require(repository != nullptr, "Canonical repository open failed: " + error);
  const auto &descriptor = repository->descriptor();
  require(descriptor.run_manifest_digest == handoff.canonical.manifest_digest &&
              descriptor.coordinate_catalog_validated &&
              descriptor.consolidated_metadata && descriptor.stable_identity &&
              descriptor.camera_frame_count == handoff.canonical.frames &&
              descriptor.row_count == handoff.canonical.rows &&
              descriptor.source_width == handoff.canonical.source_width &&
              descriptor.source_height == handoff.canonical.source_height &&
              descriptor.offset_read_calls == 1,
          "Canonical descriptor disagrees with the handoff");

  const auto &sample = handoff.samples.at("normalized_detection");
  require(sample.at("row_index").get<size_t>() == 0,
          "Canonical sample row is not supported by this gate");
  const auto page = repository->resolveCameraFrameRange(0, 0);
  require(page.status == crimson::zarr::CanonicalDetectionPageStatus::Ready &&
              page.frames.size() == 1 &&
              page.frames.front().detections.size() == 1 &&
              page.frames.front().detections.front().row_index == 0,
          "Canonical sample row did not resolve at camera frame zero");
  const auto &detection = page.frames.front().detections.front();
  const auto expected_normalized =
      doubleArray<4>(sample.at("bbox_norm_cxcywh"));
  for (size_t index = 0; index < 4; ++index) {
    require(
        near(detection.normalized_cxcywh[index], expected_normalized[index]),
        "Canonical normalized detection sample differs");
  }
  crimson::coordinates::TransformAuthority authority;
  authority.source_dimensions = {
      static_cast<int64_t>(descriptor.source_width),
      static_cast<int64_t>(descriptor.source_height)};
  const auto transformed =
      crimson::coordinates::normalizedCenterSizeBoxToContinuousPixelXyxy(
          {detection.normalized_cxcywh[0], detection.normalized_cxcywh[1],
           detection.normalized_cxcywh[2], detection.normalized_cxcywh[3]},
          authority);
  require(transformed.has_value(),
          "Canonical normalized sample could not be transformed");
  const auto expected_box =
      doubleArray<4>(sample.at("expected_bbox_source_camera_xyxy"));
  const std::array<double, 4> observed_box = {
      transformed->x_min, transformed->y_min, transformed->x_max,
      transformed->y_max};
  const double transform_error = maximumError(observed_box, expected_box);
  require(near(transformed->x_min, expected_box[0]) &&
              near(transformed->y_min, expected_box[1]) &&
              near(transformed->x_max, expected_box[2]) &&
              near(transformed->y_max, expected_box[3]),
          "Canonical source-camera transform sample differs");
  return {
      {"elapsed_ms", elapsedMilliseconds(started)},
      {"run", descriptor.run_name},
      {"manifest_digest", descriptor.run_manifest_digest},
      {"coordinate_catalog_validated", descriptor.coordinate_catalog_validated},
      {"frames", descriptor.camera_frame_count},
      {"rows", descriptor.row_count},
      {"offset_read_calls", descriptor.offset_read_calls},
      {"exact_handle_opens", metrics.exact_handle_opens},
      {"sample_row", detection.row_index},
      {"sample_transform_max_error", transform_error},
      {"sample_transform_tolerance", 1e-3}};
}

json validateRefined(const Handoff &handoff) {
  std::string error;
  const auto started = Clock::now();
  auto archive =
      crimson::zarr::ArchiveContext::Open(handoff.refined.archive, &error);
  require(archive != nullptr, "Refined archive open failed: " + error);
  crimson::zarr::RefinedDetectionRepositoryOpenOptions options;
  options.allow_selector_ineligible = true;
  options.expected_manifest_digest = handoff.refined.manifest_digest;
  crimson::zarr::RefinedDetectionRepositoryOpenMetrics metrics;
  auto repository = crimson::zarr::OpenRefinedDetectionRepository(
      archive, handoff.refined.run, options, &error, &metrics);
  require(repository != nullptr, "Refined repository open failed: " + error);
  const auto &descriptor = repository->descriptor();
  require(descriptor.run_manifest_digest == handoff.refined.manifest_digest &&
              descriptor.coordinate_catalog_validated &&
              descriptor.consolidated_metadata && descriptor.stable_identity &&
              descriptor.source_audit_lazy &&
              descriptor.camera_frame_count == handoff.refined.frames &&
              descriptor.row_count == handoff.refined.rows &&
              descriptor.offset_read_calls == 1 &&
              metrics.source_audit_handle_opens == 0,
          "Refined descriptor disagrees with the handoff");
  return {
      {"elapsed_ms", elapsedMilliseconds(started)},
      {"run", descriptor.run_name},
      {"manifest_digest", descriptor.run_manifest_digest},
      {"coordinate_catalog_validated", descriptor.coordinate_catalog_validated},
      {"frames", descriptor.camera_frame_count},
      {"rows", descriptor.row_count},
      {"offset_read_calls", descriptor.offset_read_calls},
      {"exact_handle_opens", metrics.exact_handle_opens},
      {"source_audit_handle_opens", metrics.source_audit_handle_opens}};
}

json validateCrop(const Handoff &handoff) {
  std::string error;
  const auto started = Clock::now();
  auto archive =
      crimson::zarr::ArchiveContext::Open(handoff.crop.archive, &error);
  require(archive != nullptr, "Crop archive open failed: " + error);
  auto repository = crimson::zarr::OpenAnalysisCropGeometryRepository(
      archive, handoff.crop.run, &error);
  require(repository != nullptr, "Crop repository open failed: " + error);
  const auto &descriptor = repository->descriptor();
  require(descriptor.run_manifest_digest == handoff.crop.manifest_digest &&
              descriptor.crop_policy_digest == handoff.crop_policy_digest &&
              descriptor.source_refined_run == handoff.refined.run &&
              descriptor.source_refined_manifest_digest ==
                  handoff.refined.manifest_digest &&
              descriptor.source_pixel_authority_manifest_digest ==
                  handoff.pixel_authority_digest &&
              descriptor.coordinate_catalog_validated &&
              descriptor.consolidated_metadata &&
              descriptor.camera_frame_count == handoff.crop.frames &&
              descriptor.row_count == handoff.crop.rows &&
              descriptor.source_width == handoff.crop.source_width &&
              descriptor.source_height == handoff.crop.source_height,
          "Crop descriptor disagrees with the handoff");

  const auto &sample = handoff.samples.at("rowwise_roi_to_source");
  require(sample.at("row_index").get<size_t>() == 0,
          "Crop sample row is not supported by this gate");
  const auto resolved = repository->resolveCameraFrame(
      0, static_cast<int>(descriptor.source_width),
      static_cast<int>(descriptor.source_height));
  require(
      resolved.status == crimson::zarr::AnalysisCropGeometryStatus::Mapped &&
          resolved.roi_index == 0 && resolved.instance_key.has_value() &&
          resolved.roi_bbox_xyxy.has_value() && resolved.geometry.has_value(),
      "Crop sample row did not resolve at camera frame zero");
  const auto expected_key = sample.at("instance_key").get<uint64_t>();
  const auto expected_roi_box = doubleArray<4>(sample.at("bbox_roi_xyxy"));
  const auto expected_origin =
      doubleArray<2>(sample.at("roi_origin_source_camera_xy"));
  const auto expected_extent = doubleArray<2>(sample.at("roi_extent_wh"));
  require(
      *resolved.instance_key == expected_key &&
          near(resolved.geometry->full_frame_crop.x, expected_origin[0]) &&
          near(resolved.geometry->full_frame_crop.y, expected_origin[1]) &&
          near(resolved.geometry->full_frame_crop.width, expected_extent[0]) &&
          near(resolved.geometry->full_frame_crop.height, expected_extent[1]),
      "Crop placement sample differs");
  for (size_t index = 0; index < 4; ++index) {
    require(near((*resolved.roi_bbox_xyxy)[index], expected_roi_box[index]),
            "Persisted ROI box sample differs");
  }
  crimson::coordinates::RoiPlacement placement;
  placement.source_window = {static_cast<int64_t>(expected_origin[0]),
                             static_cast<int64_t>(expected_origin[1]),
                             static_cast<int64_t>(expected_extent[0]),
                             static_cast<int64_t>(expected_extent[1])};
  placement.authority.source_dimensions = {
      static_cast<int64_t>(descriptor.source_width),
      static_cast<int64_t>(descriptor.source_height)};
  placement.authority.crop_manifest_digest = descriptor.run_manifest_digest;
  placement.authority.crop_policy_digest = descriptor.crop_policy_digest;
  const auto transformed = crimson::coordinates::roiPixelXyxyBoxToSourceCamera(
      {(*resolved.roi_bbox_xyxy)[0], (*resolved.roi_bbox_xyxy)[1],
       (*resolved.roi_bbox_xyxy)[2], (*resolved.roi_bbox_xyxy)[3]},
      placement);
  require(transformed.has_value(), "ROI box sample could not be transformed");
  const auto expected_box =
      doubleArray<4>(sample.at("expected_bbox_source_camera_xyxy"));
  const std::array<double, 4> observed_box = {
      transformed->x_min, transformed->y_min, transformed->x_max,
      transformed->y_max};
  const double transform_error = maximumError(observed_box, expected_box);
  require(near(transformed->x_min, expected_box[0]) &&
              near(transformed->y_min, expected_box[1]) &&
              near(transformed->x_max, expected_box[2]) &&
              near(transformed->y_max, expected_box[3]),
          "ROI-to-source transform sample differs");
  return {
      {"elapsed_ms", elapsedMilliseconds(started)},
      {"run", descriptor.run_name},
      {"manifest_digest", descriptor.run_manifest_digest},
      {"coordinate_catalog_validated", descriptor.coordinate_catalog_validated},
      {"frames", descriptor.camera_frame_count},
      {"rows", descriptor.row_count},
      {"output_size", {descriptor.output_width, descriptor.output_height}},
      {"sample_row", *resolved.roi_index},
      {"sample_instance_key", *resolved.instance_key},
      {"sample_transform_max_error", transform_error},
      {"sample_transform_tolerance", 1e-3}};
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 4) {
    std::cerr << "Usage: " << argv[0]
              << " HANDOFF_MANIFEST.json EXPECTED_FILE_SHA256 OUTPUT.json\n";
    return 2;
  }
  const std::filesystem::path handoff_path = argv[1];
  const std::string expected_file_digest = argv[2];
  const std::filesystem::path output_path = argv[3];
  json evidence = {
      {"schema_id", "crimson.coordinate_catalog_canary_gate"},
      {"schema_version", 1},
      {"classification", "headless_mounted_archive_integration"},
      {"handoff_manifest", handoff_path.string()},
      {"expected_handoff_file_sha256", expected_file_digest},
      {"crimson_base_commit", CRIMSON_GIT_COMMIT},
      {"crimson_worktree_dirty", CRIMSON_WORKTREE_DIRTY != 0},
      {"immutable_crimson_revision_bound", CRIMSON_WORKTREE_DIRTY == 0},
      {"palette_checkpoint_commit", kCheckpointPaletteCommit},
      {"pass", false},
  };
  try {
    const auto [raw_document, document] = readJson(handoff_path);
    const std::string observed_file_digest =
        crimson::zarr::Sha256Hex(raw_document);
    require(observed_file_digest == expected_file_digest,
            "Handoff file SHA-256 differs from the frozen request");
    const auto handoff = validateHandoff(document, expected_file_digest);
    evidence["handoff_file_sha256"] = observed_file_digest;
    evidence["handoff_payload_digest"] = handoff.payload_digest;
    evidence["palette_artifact_commit"] = handoff.artifact_palette_commit;
    evidence["canonical"] = validateCanonical(handoff);
    evidence["refined"] = validateRefined(handoff);
    evidence["crop"] = validateCrop(handoff);
    evidence["cross_stage_lineage_validated"] = true;
    evidence["production_state_changed"] = false;
    evidence["pass"] = true;
    writeJson(output_path, evidence);
    std::cout << evidence.dump() << '\n';
    return 0;
  } catch (const std::exception &exception) {
    evidence["error"] = exception.what();
    writeJson(output_path, evidence);
    std::cerr << "coordinate_catalog_canary_gate: FAIL: " << exception.what()
              << '\n';
    return 1;
  }
}
