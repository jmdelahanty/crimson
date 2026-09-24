#include "zarr/shared_mask_frame_index.h"

#include "zarr/archive_context.h"
#include "zarr/archive_context_internal.h"
#include "zarr/canonical_json.h"
#include "zarr/subject_mask_v1_contract.h"
#include "zarr/zarr_metadata_equivalence.h"

#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <nlohmann/json.hpp>
#include <string_view>
#include <utility>

namespace crimson::zarr {
namespace {
namespace ts = tensorstore;
using json = nlohmann::json;

void assignError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

bool validRunName(const std::string& run) {
  return !run.empty() && run != "." && run != ".." &&
         run.find('/') == std::string::npos &&
         run.find('\\') == std::string::npos;
}

std::string canonicalLittleEndianBytes(const std::vector<int64_t>& values) {
  std::string bytes;
  bytes.reserve(values.size() * sizeof(int64_t));
  for (const int64_t signed_value : values) {
    const uint64_t value = static_cast<uint64_t>(signed_value);
    for (unsigned shift = 0; shift < 64; shift += 8) {
      bytes.push_back(static_cast<char>((value >> shift) & 0xff));
    }
  }
  return bytes;
}

} // namespace

SharedMaskFrameIndex::SharedMaskFrameIndex(
    std::shared_ptr<ArchiveContext> archive,
    const CanonicalOverlaySelection& selection,
    std::vector<int64_t> offsets, size_t maximum_per_frame)
    : archive_(archive), archive_identity_(selection.archive_identity),
      mask_run_(selection.mask.run_id),
      manifest_payload_digest_(selection.mask.manifest_payload_digest),
      offset_values_digest_(selection.frame_row_offsets_digest),
      frame_count_(selection.frame_count),
      row_count_(selection.observation_count),
      maximum_per_frame_(maximum_per_frame), offsets_(std::move(offsets)) {}

bool SharedMaskFrameIndex::matches(
    const std::shared_ptr<ArchiveContext>& archive,
    const CanonicalOverlaySelection& selection) const {
  return selection.archive_identity == archive_identity_ &&
         selection.mask.valid &&
         selection.mask.group == "refined_subject_masks_runs" &&
         matchesMask(archive, selection.mask.run_id,
                     selection.mask.manifest_payload_digest,
                     selection.frame_row_offsets_digest,
                     selection.frame_count, selection.observation_count);
}

bool SharedMaskFrameIndex::matchesMask(
    const std::shared_ptr<ArchiveContext>& archive,
    const std::string& run, const std::string& manifest_digest,
    const std::string& offset_values_digest,
    size_t frame_count, size_t row_count) const {
  return archive_ && archive && archive_.get() == archive.get() &&
         archive->rootPath().lexically_normal().string() == archive_identity_ &&
         run == mask_run_ && manifest_digest == manifest_payload_digest_ &&
         offset_values_digest == offset_values_digest_ &&
         frame_count == frame_count_ && row_count == row_count_ &&
         offsets_.size() == frame_count_ + 1 &&
         offsets_.back() == static_cast<int64_t>(row_count_);
}

std::shared_ptr<const SharedMaskFrameIndex> OpenSharedMaskFrameIndex(
    const std::shared_ptr<ArchiveContext>& archive,
    const CanonicalOverlaySelection& selection,
    std::string* error_message) {
  if (!archive || !archive->impl_ || !selection.mask.valid ||
      selection.mask.group != "refined_subject_masks_runs" ||
      !validRunName(selection.mask.run_id) ||
      !IsLowerSha256(selection.mask.manifest_payload_digest) ||
      !IsLowerSha256(selection.frame_row_offsets_digest) ||
      selection.archive_identity !=
          archive->rootPath().lexically_normal().string() ||
      selection.frame_count == 0 || selection.observation_count == 0 ||
      selection.frame_count == std::numeric_limits<size_t>::max() ||
      selection.observation_count >
          static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
    assignError(error_message, "Shared mask index requires an exact bound selection");
    return nullptr;
  }
  const auto& impl = *archive->impl_;
  const std::string run_base = selection.mask.group + "/" + selection.mask.run_id;
  const std::string array_path = run_base + "/frame_row_offsets";
  const auto attrs = internal::ReadArchiveAttributes(impl, run_base);
  if (!attrs || !attrs->contains("run_manifest")) {
    assignError(error_message, "Bound mask index manifest is unavailable");
    return nullptr;
  }
  SubjectMaskV1ManifestSummary summary;
  if (!ValidateSubjectMaskV1Manifest(attrs->at("run_manifest"),
                                     selection.mask.run_id, &summary,
                                     error_message) ||
      summary.manifest_schema_version != 5 ||
      summary.payload_digest != selection.mask.manifest_payload_digest ||
      summary.manifest_digest != selection.mask.identity_digest ||
      summary.selector_eligible != selection.mask.selector_eligible ||
      (!summary.selector_eligible) !=
          selection.mask.bound_selector_exception ||
      summary.frame_count != selection.frame_count ||
      summary.row_count != selection.observation_count) {
    if (error_message && error_message->empty())
      *error_message = "Bound mask index manifest disagrees with selection";
    return nullptr;
  }
  json direct;
  json consolidated;
  try {
    const auto& declaration = attrs->at("run_manifest")
                                  .at("payload")
                                  .at("logical_content")
                                  .at("document")
                                  .at("arrays")
                                  .at("frame_row_offsets");
    if (declaration.at("shape").get<std::vector<size_t>>() !=
            std::vector<size_t>{selection.frame_count + 1} ||
        declaration.value("dtype", "") != "int64" ||
        declaration.value("digest_algorithm", "") !=
            "sha256_c_contiguous_bytes_v1" ||
        declaration.value("sha256", "") !=
            selection.frame_row_offsets_digest) {
      assignError(error_message, "Bound mask offset declaration disagrees with selection");
      return nullptr;
    }
    const auto metadata = internal::ReadArchiveRunMetadata(impl, {run_base});
    const auto direct_value = internal::ReadArchiveJson(impl, array_path + "/zarr.json");
    if (!metadata || !direct_value) {
      assignError(error_message, "Bound mask offset metadata is unavailable");
      return nullptr;
    }
    const auto& entries = metadata->at("consolidated_metadata").at("metadata");
    direct = *direct_value;
    consolidated = entries.at(array_path);
    if (!internal::EquivalentDirectAndConsolidatedZarrNode(direct,
                                                            consolidated) ||
        direct.value("zarr_format", 0) != 3 ||
        direct.value("node_type", "") != "array" ||
        direct.value("data_type", "") != "int64" ||
        direct.at("shape").get<std::vector<size_t>>() !=
            std::vector<size_t>{selection.frame_count + 1}) {
      assignError(error_message, "Bound mask offset array metadata is invalid");
      return nullptr;
    }
  } catch (const json::exception&) {
    assignError(error_message, "Bound mask offset metadata is malformed");
    return nullptr;
  }

  auto spec = internal::MakeReadOnlyArraySpec(impl, array_path);
  if (!spec) {
    assignError(error_message, "Bound mask offset array is unavailable");
    return nullptr;
  }
  (*spec)["metadata"] = consolidated;
  auto opened = ts::Open<int64_t, 1>(
      *spec, ts::OpenMode::open | ts::OpenMode::assume_metadata,
      ts::ReadWriteMode::read, impl.context).result();
  if (!opened.ok() || opened->domain().shape()[0] !=
                          static_cast<ts::Index>(selection.frame_count + 1)) {
    assignError(error_message, opened.ok()
        ? "Bound mask offset handle has an invalid shape"
        : "Bound mask offset handle could not open: " +
              opened.status().ToString());
    return nullptr;
  }
  auto read = ts::Read(*opened).result();
  if (!read.ok() || read->rank() != 1 || read->byte_strides().size() != 1 ||
      read->shape()[0] != static_cast<ts::Index>(selection.frame_count + 1)) {
    assignError(error_message, read.ok()
        ? "Bound mask offset read has an invalid shape"
        : "Bound mask offset read failed: " + read.status().ToString());
    return nullptr;
  }
  std::vector<int64_t> offsets(selection.frame_count + 1);
  const auto* origin = reinterpret_cast<const uint8_t*>(
      read->byte_strided_origin_pointer().get());
  for (size_t row = 0; row < offsets.size(); ++row) {
    std::memcpy(&offsets[row],
                origin + static_cast<ts::Index>(row) * read->byte_strides()[0],
                sizeof(int64_t));
  }
  if (offsets.front() != 0 ||
      offsets.back() != static_cast<int64_t>(selection.observation_count) ||
      !std::is_sorted(offsets.begin(), offsets.end()) ||
      Sha256Hex(canonicalLittleEndianBytes(offsets)) !=
          selection.frame_row_offsets_digest) {
    assignError(error_message, "Bound mask offset values fail identity validation");
    return nullptr;
  }
  size_t maximum = 0;
  for (size_t frame = 0; frame < selection.frame_count; ++frame) {
    const auto count = offsets[frame + 1] - offsets[frame];
    if (count < 0 || static_cast<uint64_t>(count) >
                         std::numeric_limits<size_t>::max()) {
      assignError(error_message, "Bound mask frame offset count is invalid");
      return nullptr;
    }
    maximum = std::max(maximum, static_cast<size_t>(count));
  }
  return std::shared_ptr<const SharedMaskFrameIndex>(
      new SharedMaskFrameIndex(archive, selection, std::move(offsets), maximum));
}

} // namespace crimson::zarr
