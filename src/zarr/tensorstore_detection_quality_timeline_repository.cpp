#include "zarr/tensorstore_detection_quality_timeline_repository.h"

#include "zarr/archive_context_internal.h"
#include "zarr/canonical_detection_contract.h"
#include "zarr/refined_detection_contract.h"
#include "zarr/zarr_metadata_equivalence.h"

#include <tensorstore/index_space/dim_expression.h>
#include <tensorstore/open.h>
#include <tensorstore/tensorstore.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace crimson::zarr {
namespace {

namespace ts = tensorstore;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

double elapsedMilliseconds(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}

void assignError(std::string *destination, std::string value) {
  if (destination) {
    *destination = std::move(value);
  }
}

const json *consolidatedEntry(const json &root, const std::string &path) {
  try {
    const auto &metadata = root.at("consolidated_metadata").at("metadata");
    const auto found = metadata.find(path);
    return found == metadata.end() ? nullptr : &*found;
  } catch (const json::exception &) {
    return nullptr;
  }
}

bool validateDirectGroup(const ArchiveContext::Impl &archive, const json &root,
                         const std::string &path, size_t *read_count,
                         std::string *error) {
  const auto *consolidated = consolidatedEntry(root, path);
  const auto direct = internal::ReadArchiveJson(archive, path + "/zarr.json");
  if (read_count) {
    ++*read_count;
  }
  if (!direct || !consolidated ||
      !internal::EquivalentDirectAndConsolidatedZarrNode(*direct,
                                                         *consolidated)) {
    assignError(
        error,
        "Detection-quality direct and consolidated metadata disagree: " + path);
    return false;
  }
  return true;
}

template <typename T, ts::DimensionIndex Rank>
std::optional<ts::TensorStore<T, Rank>>
openExact(const ArchiveContext::Impl &archive, const json &root,
          const std::string &path, std::string_view dtype, size_t columns,
          size_t expected_rows, std::string *error) {
  const auto *metadata = consolidatedEntry(root, path);
  auto spec = internal::MakeReadOnlyArraySpec(archive, path);
  if (!metadata || !spec || metadata->value("node_type", "") != "array" ||
      metadata->value("data_type", "") != dtype ||
      metadata->contains("consolidated_metadata")) {
    assignError(error, "Missing exact detection-quality array: " + path);
    return std::nullopt;
  }
  try {
    const auto shape = metadata->at("shape").get<std::vector<size_t>>();
    if (shape.size() != static_cast<size_t>(Rank) || shape.empty() ||
        shape[0] != expected_rows || (Rank == 2 && shape[1] != columns)) {
      assignError(error, "Detection-quality array rank is invalid: " + path);
      return std::nullopt;
    }
  } catch (const json::exception &) {
    assignError(error, "Detection-quality array shape is invalid: " + path);
    return std::nullopt;
  }
  (*spec)["metadata"] = *metadata;
  auto opened = ts::Open<T, Rank>(
                    *spec, ts::OpenMode::open | ts::OpenMode::assume_metadata,
                    ts::ReadWriteMode::read, archive.context)
                    .result();
  if (!opened.ok()) {
    assignError(error, path + ": " + opened.status().ToString());
    return std::nullopt;
  }
  return *opened;
}

template <typename T>
bool readRows(const ts::TensorStore<T, 1> &store, size_t first, size_t last,
              std::vector<T> *output, std::string *error) {
  if (!output || last < first ||
      last > static_cast<size_t>(store.domain().shape()[0])) {
    assignError(error, "Detection-quality row range is invalid");
    return false;
  }
  if (first == last) {
    output->clear();
    return true;
  }
  ts::Box<1> domain(store.domain().box());
  domain.origin()[0] = static_cast<ts::Index>(first);
  domain.shape()[0] = static_cast<ts::Index>(last - first);
  const auto read = ts::Read(store | ts::IdentityTransform(domain)).result();
  if (!read.ok()) {
    assignError(error, read.status().ToString());
    return false;
  }
  output->resize(last - first);
  const auto *origin = reinterpret_cast<const uint8_t *>(
      read->byte_strided_origin_pointer().get());
  for (size_t row = 0; row < last - first; ++row) {
    (*output)[row] = *reinterpret_cast<const T *>(
        origin + static_cast<ts::Index>(row) * read->byte_strides()[0]);
  }
  return true;
}

bool validateOffsets(const std::vector<int64_t> &offsets, size_t frame_count,
                     size_t row_count, const char *label, std::string *error) {
  if (offsets.size() != frame_count + 1 || offsets.empty() ||
      offsets.front() != 0 || offsets.back() < 0 ||
      static_cast<size_t>(offsets.back()) != row_count ||
      !std::is_sorted(offsets.begin(), offsets.end())) {
    assignError(error, std::string(label) + " frame_row_offsets are invalid");
    return false;
  }
  return true;
}

std::vector<timeline::DetectionReasonDescriptor>
parseReasonCodes(const json &payload, std::string *error) {
  std::vector<timeline::DetectionReasonDescriptor> result;
  try {
    const auto &codes =
        payload.at("reason_registries").at("source_detections").at("codes");
    for (auto it = codes.begin(); it != codes.end(); ++it) {
      size_t consumed = 0;
      const unsigned long parsed = std::stoul(it.key(), &consumed, 10);
      if (consumed != it.key().size() || parsed > UINT16_MAX ||
          !it.value().is_string()) {
        assignError(error, "Refined source reason registry is invalid");
        return {};
      }
      result.push_back(
          {static_cast<uint16_t>(parsed), it.value().get<std::string>()});
    }
  } catch (const json::exception &) {
    assignError(error, "Refined source reason registry is unavailable");
    return {};
  }
  std::sort(result.begin(), result.end(), [](const auto &lhs, const auto &rhs) {
    return lhs.code < rhs.code;
  });
  if (result.empty() || result.front().code != 0 ||
      result.front().label != "none") {
    assignError(error, "Refined source reason registry lacks code zero");
    return {};
  }
  return result;
}

bool validateRefinedCodeMaps(const json &payload, std::string *error) {
  try {
    const auto &maps = payload.at("logical_schema").at("code_maps");
    const auto &kinds = maps.at("source_kind_codes");
    const auto &decisions = maps.at("source_detections/decision_codes");
    if (kinds.at("raw_detect") != 1 || kinds.at("manual") != 3 ||
        decisions.at("accepted") != 0 || decisions.at("filtered") != 1 ||
        decisions.at("duplicate") != 2 || decisions.at("manual_clear") != 3) {
      assignError(error, "Refined detection code maps are incompatible");
      return false;
    }
  } catch (const json::exception &) {
    assignError(error, "Refined detection code maps are unavailable");
    return false;
  }
  return true;
}

class TensorStoreDetectionQualityRepository final
    : public timeline::DetectionQualityTimelineRepository {
public:
  TensorStoreDetectionQualityRepository(
      timeline::DetectionQualityTimelineDescriptor descriptor,
      std::vector<int64_t> source_offsets,
      std::vector<int64_t> instance_offsets,
      ts::TensorStore<float, 1> source_scores,
      std::optional<ts::TensorStore<uint8_t, 1>> source_decisions,
      std::optional<ts::TensorStore<uint16_t, 1>> source_reasons,
      std::optional<ts::TensorStore<uint8_t, 1>> instance_kinds)
      : descriptor_(std::move(descriptor)),
        source_offsets_(std::move(source_offsets)),
        instance_offsets_(std::move(instance_offsets)),
        source_scores_(std::move(source_scores)),
        source_decisions_(std::move(source_decisions)),
        source_reasons_(std::move(source_reasons)),
        instance_kinds_(std::move(instance_kinds)) {}

  const timeline::DetectionQualityTimelineDescriptor &
  descriptor() const override {
    return descriptor_;
  }

  timeline::DetectionQualityTimelineWindow
  resolveWindow(int64_t first_camera_frame,
                int64_t last_camera_frame) const override {
    const auto started = Clock::now();
    timeline::DetectionQualityTimelineWindow window;
    if (first_camera_frame < 0 || last_camera_frame < first_camera_frame ||
        static_cast<size_t>(last_camera_frame) >= descriptor_.frame_count) {
      window.status = timeline::DetectionQualityTimelineStatus::OutOfRange;
      return window;
    }
    const size_t first_frame = static_cast<size_t>(first_camera_frame);
    const size_t last_frame = static_cast<size_t>(last_camera_frame);
    const size_t source_first =
        static_cast<size_t>(source_offsets_[first_frame]);
    const size_t source_last =
        static_cast<size_t>(source_offsets_[last_frame + 1]);
    const size_t instance_first =
        static_cast<size_t>(instance_offsets_[first_frame]);
    const size_t instance_last =
        static_cast<size_t>(instance_offsets_[last_frame + 1]);
    std::vector<float> scores;
    std::vector<uint8_t> decisions;
    std::vector<uint16_t> reasons;
    std::vector<uint8_t> kinds;
    struct ReadResult {
      bool ready = false;
      std::string error;
    };
    auto runRead = [this](auto read) {
      const size_t active = active_fields_.fetch_add(1) + 1;
      size_t peak = peak_active_fields_.load();
      while (active > peak &&
             !peak_active_fields_.compare_exchange_weak(peak, active)) {
      }
      ReadResult result;
      result.ready = read(&result.error);
      active_fields_.fetch_sub(1);
      return result;
    };
    auto score_future = std::async(std::launch::async, [&] {
      return runRead([&](std::string *error) {
        return readRows(source_scores_, source_first, source_last, &scores,
                        error);
      });
    });
    std::optional<std::future<ReadResult>> decision_future;
    std::optional<std::future<ReadResult>> reason_future;
    std::optional<std::future<ReadResult>> kind_future;
    if (source_decisions_) {
      decision_future.emplace(std::async(std::launch::async, [&] {
        return runRead([&](std::string *error) {
          return readRows(*source_decisions_, source_first, source_last,
                          &decisions, error);
        });
      }));
      reason_future.emplace(std::async(std::launch::async, [&] {
        return runRead([&](std::string *error) {
          return readRows(*source_reasons_, source_first, source_last, &reasons,
                          error);
        });
      }));
      kind_future.emplace(std::async(std::launch::async, [&] {
        return runRead([&](std::string *error) {
          return readRows(*instance_kinds_, instance_first, instance_last,
                          &kinds, error);
        });
      }));
    }
    const auto score_result = score_future.get();
    const auto decision_result =
        decision_future ? decision_future->get() : ReadResult{true, {}};
    const auto reason_result =
        reason_future ? reason_future->get() : ReadResult{true, {}};
    const auto kind_result =
        kind_future ? kind_future->get() : ReadResult{true, {}};
    if (!score_result.ready || !decision_result.ready || !reason_result.ready ||
        !kind_result.ready) {
      window.status = timeline::DetectionQualityTimelineStatus::ReadFailed;
      window.error = !score_result.ready      ? score_result.error
                     : !decision_result.ready ? decision_result.error
                     : !reason_result.ready   ? reason_result.error
                                              : kind_result.error;
    } else {
      if (!source_decisions_) {
        decisions.assign(scores.size(), 0);
        reasons.assign(scores.size(), 0);
        kinds.assign(instance_last - instance_first, 1);
      }
      window = timeline::buildDetectionQualityTimelineWindow(
          descriptor_, first_camera_frame, last_camera_frame, source_offsets_,
          source_first, scores, decisions, reasons, instance_offsets_,
          instance_first, kinds);
    }
    const double elapsed = elapsedMilliseconds(started);
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    ++metrics_.range_reads;
    metrics_.maximum_range_read_ms =
        std::max(metrics_.maximum_range_read_ms, elapsed);
    metrics_.peak_concurrent_field_reads = std::max(
        metrics_.peak_concurrent_field_reads, peak_active_fields_.load());
    if (window.ready()) {
      metrics_.source_rows_read += window.source_rows_read;
      metrics_.instance_rows_read += window.instance_rows_read;
      metrics_.decoded_bytes += window.decoded_bytes;
    } else if (window.status ==
               timeline::DetectionQualityTimelineStatus::ReadFailed) {
      ++metrics_.failed_reads;
      metrics_.last_error = window.error;
    }
    return window;
  }

  timeline::DetectionQualityTimelineOverview
  resolveOverview(size_t maximum_points_per_trace, size_t maximum_decoded_bytes,
                  const std::function<bool()> &cancelled) const override {
    const auto started = Clock::now();
    timeline::DetectionQualityTimelineOverview overview;
    const size_t source_bytes_per_row =
        sizeof(float) + (descriptor_.source_audit ? sizeof(uint8_t) : 0);
    const size_t instance_bytes_per_row =
        descriptor_.source_audit ? sizeof(uint8_t) : 0;
    if (maximum_points_per_trace < 2 ||
        maximum_decoded_bytes < source_bytes_per_row) {
      overview.error = "Detection-quality overview limits are invalid";
      return overview;
    }
    timeline::DetectionQualityOverviewAccumulator accumulator(
        descriptor_.frame_count, maximum_points_per_trace,
        descriptor_.source_audit);
    size_t first_frame = 0;
    std::string error;
    while (first_frame < descriptor_.frame_count) {
      if (cancelled && cancelled()) {
        overview.error = "Detection-quality overview was cancelled";
        return overview;
      }
      const size_t source_first =
          static_cast<size_t>(source_offsets_[first_frame]);
      const size_t instance_first =
          static_cast<size_t>(instance_offsets_[first_frame]);
      const auto decodedThrough = [&](size_t frame) {
        return (static_cast<size_t>(source_offsets_[frame]) - source_first) *
                   source_bytes_per_row +
               (static_cast<size_t>(instance_offsets_[frame]) -
                instance_first) *
                   instance_bytes_per_row;
      };
      size_t low = first_frame + 1;
      size_t high = descriptor_.frame_count;
      size_t last_frame = low;
      while (low <= high) {
        const size_t middle = low + (high - low) / 2;
        if (decodedThrough(middle) <= maximum_decoded_bytes ||
            middle == first_frame + 1) {
          last_frame = middle;
          low = middle + 1;
        } else {
          high = middle - 1;
        }
      }
      const size_t source_last =
          static_cast<size_t>(source_offsets_[last_frame]);
      const size_t instance_last =
          static_cast<size_t>(instance_offsets_[last_frame]);
      std::vector<float> scores;
      std::vector<uint8_t> decisions;
      std::vector<uint8_t> kinds;
      struct ReadResult {
        bool ready = false;
        std::string error;
      };
      auto score_future = std::async(std::launch::async, [&] {
        ReadResult result;
        result.ready = readRows(source_scores_, source_first, source_last,
                                &scores, &result.error);
        return result;
      });
      std::optional<std::future<ReadResult>> decision_future;
      std::optional<std::future<ReadResult>> kind_future;
      if (descriptor_.source_audit) {
        decision_future.emplace(std::async(std::launch::async, [&] {
          ReadResult result;
          result.ready = readRows(*source_decisions_, source_first, source_last,
                                  &decisions, &result.error);
          return result;
        }));
        kind_future.emplace(std::async(std::launch::async, [&] {
          ReadResult result;
          result.ready = readRows(*instance_kinds_, instance_first,
                                  instance_last, &kinds, &result.error);
          return result;
        }));
      }
      const auto score_result = score_future.get();
      const auto decision_result =
          decision_future ? decision_future->get() : ReadResult{true, {}};
      const auto kind_result =
          kind_future ? kind_future->get() : ReadResult{true, {}};
      if (!score_result.ready || !decision_result.ready || !kind_result.ready) {
        overview.error = !score_result.ready      ? score_result.error
                         : !decision_result.ready ? decision_result.error
                                                  : kind_result.error;
        break;
      }
      for (size_t frame = first_frame; frame < last_frame; ++frame) {
        const size_t source_begin =
            static_cast<size_t>(source_offsets_[frame]) - source_first;
        const size_t source_end =
            static_cast<size_t>(source_offsets_[frame + 1]) - source_first;
        const size_t instance_begin =
            static_cast<size_t>(instance_offsets_[frame]) - instance_first;
        const size_t instance_end =
            static_cast<size_t>(instance_offsets_[frame + 1]) - instance_first;
        const float *frame_scores =
            source_end > source_begin ? scores.data() + source_begin : nullptr;
        const uint8_t *frame_decisions =
            descriptor_.source_audit && source_end > source_begin
                ? decisions.data() + source_begin
                : nullptr;
        const uint8_t *frame_kinds =
            descriptor_.source_audit && instance_end > instance_begin
                ? kinds.data() + instance_begin
                : nullptr;
        if (!accumulator.addFrame(
                static_cast<int64_t>(frame), frame_scores, frame_decisions,
                source_end - source_begin, frame_kinds,
                descriptor_.source_audit ? instance_end - instance_begin : 0,
                &error)) {
          overview.error = std::move(error);
          break;
        }
      }
      if (!overview.error.empty()) {
        break;
      }
      first_frame = last_frame;
    }
    if (overview.error.empty()) {
      overview = accumulator.finish();
    }
    const double elapsed = elapsedMilliseconds(started);
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    ++metrics_.overview_reads;
    metrics_.peak_concurrent_field_reads =
        std::max(metrics_.peak_concurrent_field_reads,
                 descriptor_.source_audit ? size_t{3} : size_t{1});
    metrics_.maximum_overview_read_ms =
        std::max(metrics_.maximum_overview_read_ms, elapsed);
    if (overview.ready()) {
      metrics_.overview_source_rows_read += overview.source_rows_read;
      metrics_.overview_instance_rows_read += overview.instance_rows_read;
      metrics_.overview_decoded_bytes += overview.decoded_bytes;
    } else if (!(cancelled && cancelled())) {
      ++metrics_.failed_overview_reads;
      metrics_.last_error = overview.error;
    }
    return overview;
  }

  timeline::DetectionQualityTimelineRepositoryMetrics metrics() const override {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    return metrics_;
  }

private:
  timeline::DetectionQualityTimelineDescriptor descriptor_;
  std::vector<int64_t> source_offsets_;
  std::vector<int64_t> instance_offsets_;
  ts::TensorStore<float, 1> source_scores_;
  std::optional<ts::TensorStore<uint8_t, 1>> source_decisions_;
  std::optional<ts::TensorStore<uint16_t, 1>> source_reasons_;
  std::optional<ts::TensorStore<uint8_t, 1>> instance_kinds_;
  mutable std::atomic<size_t> active_fields_{0};
  mutable std::atomic<size_t> peak_active_fields_{0};
  mutable std::mutex metrics_mutex_;
  mutable timeline::DetectionQualityTimelineRepositoryMetrics metrics_;
};

} // namespace

std::unique_ptr<timeline::DetectionQualityTimelineRepository>
OpenDetectionQualityTimelineRepository(
    const std::shared_ptr<ArchiveContext> &archive,
    const DetectionQualityTimelineOpenRequest &request,
    std::string *error_message,
    DetectionQualityTimelineOpenMetrics *open_metrics) {
  DetectionQualityTimelineOpenMetrics metrics;
  const auto all_started = Clock::now();
  if (!archive || !archive->impl_ || request.run_name.empty() ||
      request.run_name.find('/') != std::string::npos) {
    assignError(error_message,
                "An archive and exact detection-quality run are required");
    return nullptr;
  }
  const bool refined =
      request.surface_kind == DetectionSurfaceKind::RefinedSnapshotV1;
  const std::string base =
      (refined ? "refined_detect_runs/" : "detect_runs/") + request.run_name;
  const auto metadata_started = Clock::now();
  const auto root = internal::ReadArchiveJson(*archive->impl_, "zarr.json");
  metrics.root_metadata_reads = 1;
  if (!root || root->value("zarr_format", 0) != 3 ||
      root->value("node_type", "") != "group" ||
      !root->contains("consolidated_metadata")) {
    assignError(error_message,
                "Detection-quality archive metadata is unavailable");
    return nullptr;
  }
  const auto *run_metadata = consolidatedEntry(*root, base);
  if (!run_metadata || run_metadata->value("node_type", "") != "group") {
    assignError(error_message, "Detection-quality run is unavailable");
    return nullptr;
  }
  for (const std::string &path :
       refined ? std::vector<std::string>{base, base + "/instances",
                                          base + "/source_detections"}
               : std::vector<std::string>{base, base + "/instances"}) {
    if (!validateDirectGroup(*archive->impl_, *root, path,
                             &metrics.direct_group_metadata_reads,
                             error_message)) {
      return nullptr;
    }
  }

  timeline::DetectionQualityTimelineDescriptor descriptor;
  descriptor.surface_kind = request.surface_kind;
  descriptor.source_group = refined ? "refined_detect_runs" : "detect_runs";
  descriptor.run_name = request.run_name;
  std::vector<timeline::DetectionReasonDescriptor> reasons;
  try {
    const auto &manifest = run_metadata->at("attributes").at("run_manifest");
    if (refined) {
      RefinedDetectionManifestSummary summary;
      if (!ValidateRefinedDetectionRunManifest(
              manifest, request.run_name,
              request.allow_selector_ineligible_refined_run, &summary,
              error_message) ||
          !validateRefinedCodeMaps(manifest.at("payload"), error_message)) {
        return nullptr;
      }
      reasons = parseReasonCodes(manifest.at("payload"), error_message);
      if (reasons.empty()) {
        return nullptr;
      }
      descriptor.run_manifest_digest = summary.payload_digest;
      descriptor.frame_count = summary.frame_count;
      descriptor.source_row_count = summary.source_detection_count;
      descriptor.instance_row_count = summary.instance_count;
      descriptor.source_audit = true;
    } else {
      CanonicalDetectionManifestSummary summary;
      if (!ValidateCanonicalDetectionRunManifest(manifest, request.run_name,
                                                 &summary, error_message)) {
        return nullptr;
      }
      descriptor.run_manifest_digest = summary.payload_digest;
      descriptor.frame_count = summary.frame_count;
      descriptor.source_row_count = summary.instance_count;
      descriptor.instance_row_count = summary.instance_count;
      descriptor.source_audit = false;
      reasons = {{0, "none"}};
      const auto &payload = manifest.at("payload");
      const auto &evidence = payload.at("source_evidence");
      descriptor.model_artifact_sha256 =
          evidence.at("model_artifact").value("sha256", "");
      descriptor.producer_id = evidence.at("producer").value("id", "");
      descriptor.producer_version =
          evidence.at("producer").value("version", "");
    }
  } catch (const json::exception &exception) {
    assignError(error_message, "Detection-quality manifest is invalid: " +
                                   std::string(exception.what()));
    return nullptr;
  }
  descriptor.source_reason_codes = std::move(reasons);
  metrics.metadata_ms = elapsedMilliseconds(metadata_started);

  const std::string source =
      base + (refined ? "/source_detections/" : "/instances/");
  const std::string instances = base + "/instances/";
  const auto handles_started = Clock::now();
  auto scores =
      openExact<float, 1>(*archive->impl_, *root, source + "scores", "float32",
                          1, descriptor.source_row_count, error_message);
  auto source_offsets = openExact<int64_t, 1>(
      *archive->impl_, *root, source + "frame_row_offsets", "int64", 1,
      descriptor.frame_count + 1, error_message);
  metrics.exact_handle_opens += scores.has_value() + source_offsets.has_value();
  std::optional<ts::TensorStore<uint8_t, 1>> decisions;
  std::optional<ts::TensorStore<uint16_t, 1>> reason_codes;
  std::optional<ts::TensorStore<uint8_t, 1>> instance_kinds;
  std::optional<ts::TensorStore<int64_t, 1>> instance_offsets;
  if (refined) {
    decisions = openExact<uint8_t, 1>(
        *archive->impl_, *root, source + "decision_codes", "uint8", 1,
        descriptor.source_row_count, error_message);
    reason_codes = openExact<uint16_t, 1>(
        *archive->impl_, *root, source + "reason_codes", "uint16", 1,
        descriptor.source_row_count, error_message);
    instance_kinds = openExact<uint8_t, 1>(
        *archive->impl_, *root, instances + "source_kind_codes", "uint8", 1,
        descriptor.instance_row_count, error_message);
    instance_offsets = openExact<int64_t, 1>(
        *archive->impl_, *root, instances + "frame_row_offsets", "int64", 1,
        descriptor.frame_count + 1, error_message);
    metrics.exact_handle_opens +=
        decisions.has_value() + reason_codes.has_value() +
        instance_kinds.has_value() + instance_offsets.has_value();
  }
  metrics.exact_handle_open_ms = elapsedMilliseconds(handles_started);
  if (!scores || !source_offsets ||
      (refined &&
       (!decisions || !reason_codes || !instance_kinds || !instance_offsets))) {
    return nullptr;
  }

  const auto offsets_started = Clock::now();
  std::vector<int64_t> retained_source_offsets;
  std::vector<int64_t> retained_instance_offsets;
  if (!readRows(*source_offsets, 0, descriptor.frame_count + 1,
                &retained_source_offsets, error_message) ||
      !validateOffsets(retained_source_offsets, descriptor.frame_count,
                       descriptor.source_row_count, "Source detection",
                       error_message)) {
    return nullptr;
  }
  ++metrics.offset_read_calls;
  if (refined) {
    if (!readRows(*instance_offsets, 0, descriptor.frame_count + 1,
                  &retained_instance_offsets, error_message) ||
        !validateOffsets(retained_instance_offsets, descriptor.frame_count,
                         descriptor.instance_row_count, "Refined instance",
                         error_message)) {
      return nullptr;
    }
    ++metrics.offset_read_calls;
  } else {
    retained_instance_offsets = retained_source_offsets;
  }
  metrics.offset_read_ms = elapsedMilliseconds(offsets_started);
  metrics.retained_offset_bytes =
      (retained_source_offsets.size() +
       (refined ? retained_instance_offsets.size() : 0)) *
      sizeof(int64_t);
  descriptor.offset_read_calls = metrics.offset_read_calls;
  descriptor.retained_offset_bytes = metrics.retained_offset_bytes;
  metrics.total_ms = elapsedMilliseconds(all_started);
  if (open_metrics) {
    *open_metrics = metrics;
  }
  if (error_message) {
    error_message->clear();
  }
  return std::make_unique<TensorStoreDetectionQualityRepository>(
      std::move(descriptor), std::move(retained_source_offsets),
      std::move(retained_instance_offsets), std::move(*scores),
      std::move(decisions), std::move(reason_codes), std::move(instance_kinds));
}

} // namespace crimson::zarr
