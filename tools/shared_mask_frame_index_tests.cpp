#include "zarr/archive_context.h"
#include "zarr/canonical_json.h"
#include "zarr/shared_mask_frame_index.h"
#include "zarr/subject_mask_v1_contract.h"

#include <nlohmann/json.hpp>
#include <tensorstore/array.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace crimson::zarr {
struct SharedMaskFrameIndexTestAccess {
  static std::shared_ptr<const SharedMaskFrameIndex> make(
      std::shared_ptr<ArchiveContext> archive,
      const CanonicalOverlaySelection& selection,
      std::vector<int64_t> offsets, size_t maximum) {
    return std::shared_ptr<const SharedMaskFrameIndex>(
        new SharedMaskFrameIndex(std::move(archive), selection,
                                 std::move(offsets), maximum));
  }
};
} // namespace crimson::zarr

namespace {
using namespace crimson::zarr;
using json = nlohmann::json;
namespace ts = tensorstore;
#define CHECK(value) do { if (!(value)) { \
  std::cerr << "CHECK failed: " #value << " at " << __LINE__ << '\n'; \
  return 1; } } while (false)

struct TemporaryDirectory {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("crimson-shared-mask-index-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  TemporaryDirectory() { std::filesystem::create_directories(path); }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

bool writeJson(const std::filesystem::path& path, const json& value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << value.dump(2) << '\n';
  return output.good();
}

json digestEnvelope(const char* schema, int version, const json& document) {
  return {{"schema_id", schema}, {"schema_version", version},
          {"digest_algorithm", "sha256_canonical_json_v1"},
          {"digest", CanonicalJsonSha256(document)}, {"document", document}};
}

json validManifest(const std::string& offset_digest) {
  constexpr size_t frames = 4, rows = 6;
  const json dimensions = {{"n_frames", frames}, {"n_frame_boundaries", frames + 1},
      {"n_instances", rows}, {"n_rois", rows}, {"n_channels", 1},
      {"H", 2}, {"W", 2}, {"roi_height", 2}, {"roi_width", 2}};
  const json components = {{"schema_id", "palette.subject_mask.component_registry"},
      {"schema_version", 1}, {"labels", {"subject_body"}},
      {"channel_axis", 1}, {"ordering", "persisted_exact_order"}};
  const std::vector<std::string> contract_ids = {
      "palette.array.subject_mask.source_crop_row_ids",
      "palette.array.detection.instance_key",
      "palette.array.detection.source_acquisition_frame_index",
      "palette.array.frame_row_offsets", "palette.array.crop.source_crop_xywh",
      "palette.array.subject_masks_roi_dense",
      "palette.array.subject_mask.available_channels",
      "palette.array.subject_mask.mask_present",
      "palette.array.subject_mask.area_px",
      "palette.array.subject_mask.centroid_xy",
      "palette.array.subject_mask.centroid_valid",
      "palette.array.subject_mask.bbox_xyxy",
      "palette.array.subject_mask.bbox_valid"};
  json bindings = json::array();
  json arrays = json::object();
  SubjectMaskV1ManifestSummary summary;
  summary.frame_count = frames;
  summary.row_count = rows;
  summary.channel_count = 1;
  summary.mask_height = 2;
  summary.mask_width = 2;
  for (size_t index = 0; index < kSubjectMaskV1ArrayDeclarations.size(); ++index) {
    const auto& declaration = kSubjectMaskV1ArrayDeclarations[index];
    bindings.push_back({{"path", declaration.path},
                        {"contract_id", contract_ids[index]},
                        {"contract_version", 1}, {"required", true}});
    arrays[declaration.path] = {
        {"shape", ExpectedSubjectMaskV1Shape(declaration, summary)},
        {"dtype", declaration.dtype},
        {"digest_algorithm", "sha256_c_contiguous_bytes_v1"},
        {"sha256", std::string(64, 'b')}};
  }
  arrays["frame_row_offsets"]["sha256"] = offset_digest;
  const json units = json::array({{{"start_row", 0}, {"stop_row", rows},
      {"decoded_bytes", rows * 4}, {"sha256", std::string(64, 'c')}}});
  arrays["masks_roi"] = {{"shape", {rows, 1, 2, 2}}, {"dtype", "uint8"},
      {"digest_algorithm", "sha256_c_contiguous_global_row_units_v1"},
      {"identity_unit_rows", rows}, {"unit_count", 1},
      {"units_digest", CanonicalJsonSha256(units)}, {"units", units}};
  const json content = {{"schema_id", "palette.subject_mask_core.logical_content"},
      {"schema_version", 2}, {"kind", "refined_dense_core"},
      {"dimensions", dimensions}, {"components", components}, {"arrays", arrays}};
  json payload = {{"run_id", "exact_mask"},
      {"stage_family", "refined_subject_masks_runs"},
      {"kind", "refined_dense_core"},
      {"publication", {{"completion_contract", "palette.zarr_run_completion.v1"},
          {"completion_status", "complete"}, {"stage_selector_eligible", false},
          {"metadata_state", "direct_and_consolidated_validated"},
          {"metadata_digest_scope",
           "exact_run_group_and_array_declarations_redacting_only_run_manifest"},
          {"metadata_digest", std::string(64, 'd')}}},
      {"logical_schema", {{"schema_id", "palette.stage.refined_subject_mask_dense_core"},
          {"schema_version", 1},
          {"layout", "recording_observations_with_frame_row_offsets_v1"},
          {"dimensions", dimensions}, {"components", components},
          {"authority", "dense_binary_masks_roi"}, {"bindings", bindings},
          {"invariants", {{"instances_per_frame", "zero_one_or_many"},
              {"frame_index", "retained_int64_f_plus_one_offsets"},
              {"row_order", "nondecreasing_source_acquisition_frame_index"},
              {"crop_contract", "palette.stage.crop_geometry_v1_float32_placement"},
              {"derived_surfaces", "must_exactly_match_dense_authority"},
              {"legacy_aliases", "forbidden"}}}}},
      {"storage_plan", json::object()}, {"source", json::object()},
      {"write_receipt", json::object()},
      {"logical_content", {{"digest_algorithm", "sha256_canonical_json_v1"},
          {"digest", CanonicalJsonSha256(content)}, {"document", content}}},
      {"coordinate_contract", digestEnvelope(
          "palette.persisted_coordinate_catalog", 1, json::object())},
      {"coordinate_dependencies", digestEnvelope(
          "palette.subject_mask_core.coordinate_dependencies", 3, json::object())}};
  return {{"schema_id", "palette.subject_mask_core.run_manifest"},
          {"schema_version", 5},
          {"digest_algorithm", "sha256_canonical_json_v1"},
          {"payload_digest", CanonicalJsonSha256(payload)},
          {"payload", std::move(payload)}};
}

bool writeOffsetFixture(const std::filesystem::path& root,
                        const std::vector<int64_t>& offsets,
                        const json& manifest) {
  const std::string path = "refined_subject_masks_runs/exact_mask/frame_row_offsets";
  const json spec = {{"driver", "zarr3"},
      {"kvstore", {{"driver", "file"}, {"path", root.string() + "/"}}},
      {"path", path}, {"metadata", {{"shape", {offsets.size()}},
          {"data_type", "int64"}, {"chunk_grid", {{"name", "regular"},
              {"configuration", {{"chunk_shape", {offsets.size()}}}}}},
          {"chunk_key_encoding", {{"name", "default"},
              {"configuration", {{"separator", "/"}}}}},
          {"fill_value", 0}, {"codecs", json::array({{{"name", "bytes"},
              {"configuration", {{"endian", "little"}}}}})}}}};
  auto opened = ts::Open<int64_t, 1>(
      spec, ts::OpenMode::open | ts::OpenMode::create,
      ts::ReadWriteMode::read_write).result();
  if (!opened.ok()) return false;
  auto array = ts::AllocateArray<int64_t>({static_cast<ts::Index>(offsets.size())});
  std::copy(offsets.begin(), offsets.end(), array.data());
  if (!ts::Write(array, *opened).commit_future.result().ok()) return false;
  std::ifstream input(root / path / "zarr.json");
  json array_metadata;
  input >> array_metadata;
  const std::string run = "refined_subject_masks_runs/exact_mask";
  const json run_metadata = {{"zarr_format", 3}, {"node_type", "group"},
      {"attributes", {{"run_manifest", manifest}}}};
  return writeJson(root / run / "zarr.json", run_metadata) &&
      writeJson(root / "zarr.json", {{"zarr_format", 3},
          {"node_type", "group"}, {"consolidated_metadata",
              {{"kind", "inline"}, {"must_understand", false},
               {"metadata", {{run, run_metadata}, {path, array_metadata}}}}}});
}
} // namespace

int main() {
  TemporaryDirectory first_root;
  TemporaryDirectory second_root;
  std::string error;
  auto archive = ArchiveContext::Open(first_root.path, &error);
  auto next_epoch = ArchiveContext::Open(first_root.path, &error);
  auto other_archive = ArchiveContext::Open(second_root.path, &error);
  CHECK(archive && next_epoch && other_archive);
  CanonicalOverlaySelection selection;
  selection.archive_identity = archive->rootPath().string();
  selection.mask.valid = true;
  selection.mask.group = "refined_subject_masks_runs";
  selection.mask.run_id = "exact_mask";
  selection.mask.manifest_payload_digest = std::string(64, 'a');
  selection.frame_row_offsets_digest = std::string(64, 'b');
  selection.frame_count = 4;
  selection.observation_count = 6;
  auto shared = SharedMaskFrameIndexTestAccess::make(
      archive, selection, {0, 2, 2, 3, 6}, 3);
  auto dense_handle = shared;
  auto contour_handle = shared;
  auto shape_handle = shared;
  auto eye_handle = shared;
  CHECK(dense_handle.get() == contour_handle.get());
  CHECK(shape_handle.get() == eye_handle.get());
  CHECK(shared->offsets() == std::vector<int64_t>({0, 2, 2, 3, 6}));
  CHECK(shared->maximumObservationsPerFrame() == 3);
  CHECK(shared->retainedBytes() >= 5 * sizeof(int64_t));
  CHECK(shared->matches(archive, selection));
  CHECK(shared->matchesMask(archive, "exact_mask", std::string(64, 'a'),
                            std::string(64, 'b'), 4, 6));
  CHECK(shared->admits(3) && shared->admits(4));
  CHECK(!shared->admits(0) && !shared->admits(2));
  CHECK(!shared->matches(next_epoch, selection));
  CHECK(!shared->matches(other_archive, selection));
  auto altered = selection;
  altered.mask.run_id = "other_mask";
  CHECK(!shared->matches(archive, altered));
  altered = selection;
  altered.mask.manifest_payload_digest = std::string(64, 'c');
  CHECK(!shared->matches(archive, altered));
  altered = selection;
  altered.frame_row_offsets_digest = std::string(64, 'd');
  CHECK(!shared->matches(archive, altered));
  altered = selection;
  altered.frame_count++;
  CHECK(!shared->matches(archive, altered));
  altered = selection;
  altered.observation_count++;
  CHECK(!shared->matches(archive, altered));
  altered = selection;
  altered.mask.valid = false;
  CHECK(!shared->matches(archive, altered));
  CHECK(!OpenSharedMaskFrameIndex(archive, altered, &error));
  CHECK(!error.empty());

  TemporaryDirectory physical_root;
  const std::vector<int64_t> physical_offsets{0, 2, 2, 3, 6};
  std::string offset_bytes;
  for (const int64_t signed_value : physical_offsets) {
    const uint64_t value = static_cast<uint64_t>(signed_value);
    for (unsigned shift = 0; shift < 64; shift += 8)
      offset_bytes.push_back(static_cast<char>((value >> shift) & 0xff));
  }
  const std::string offset_digest = Sha256Hex(offset_bytes);
  const auto manifest = validManifest(offset_digest);
  CHECK(writeOffsetFixture(physical_root.path, physical_offsets, manifest));
  auto physical_archive = ArchiveContext::Open(physical_root.path, &error);
  CHECK(physical_archive);
  CanonicalOverlaySelection physical_selection = selection;
  physical_selection.archive_identity = physical_archive->rootPath().string();
  physical_selection.mask.identity_digest = CanonicalJsonSha256(manifest);
  physical_selection.mask.manifest_payload_digest =
      manifest.at("payload_digest").get<std::string>();
  physical_selection.frame_row_offsets_digest = offset_digest;
  physical_selection.mask.bound_selector_exception = true;
  auto physical = OpenSharedMaskFrameIndex(physical_archive,
                                            physical_selection, &error);
  if (!physical) std::cerr << "provider error: " << error << '\n';
  CHECK(physical);
  CHECK(physical->matches(physical_archive, physical_selection));
  CHECK(physical->offsets() == physical_offsets);
  CHECK(physical->maximumObservationsPerFrame() == 3);
  CHECK(physical->admits(3) && !physical->admits(2));
  auto wrong_digest_selection = physical_selection;
  wrong_digest_selection.frame_row_offsets_digest = std::string(64, 'e');
  CHECK(!OpenSharedMaskFrameIndex(physical_archive,
                                  wrong_digest_selection, &error));
  CHECK(!error.empty());
  const auto chunk = physical_root.path /
      "refined_subject_masks_runs/exact_mask/frame_row_offsets/c/0";
  {
    std::fstream payload(chunk, std::ios::binary | std::ios::in | std::ios::out);
    CHECK(payload.good());
    payload.seekp(8);
    payload.put('\x03');
    CHECK(payload.good());
  }
  auto corrupted_archive = ArchiveContext::Open(physical_root.path, &error);
  CHECK(corrupted_archive);
  CHECK(!OpenSharedMaskFrameIndex(corrupted_archive, physical_selection, &error));
  CHECK(!error.empty());
  std::cout << "shared_mask_frame_index_tests: PASS\n";
  return 0;
}
