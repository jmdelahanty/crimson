#include "coordinate_contract.h"
#include "zarr/archive_context.h"
#include "zarr/canonical_detection_repository.h"
#include "zarr/detection_repository_selection.h"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr size_t kMaximumSelections = 64;
constexpr int64_t kMaximumFramesPerSelection = 256;
constexpr int64_t kMaximumFramesAcrossSelections = 512;
constexpr std::array<int64_t, 7> kDefaultFrames = {
    53990, 54000, 54010, 2592020, 2592030, 2592040, 2937603};

struct FrameSelection {
  int64_t first = 0;
  int64_t last = 0;
};

struct Options {
  std::filesystem::path archive;
  std::string run;
  std::vector<FrameSelection> selections;
  std::optional<std::filesystem::path> output;
};

void require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

double elapsedMilliseconds(Clock::time_point started) {
  return std::chrono::duration<double, std::milli>(Clock::now() - started)
      .count();
}

int64_t parseNonnegativeInt64(std::string_view text,
                              const std::string &description) {
  require(!text.empty(), description + " is empty");
  require(text.front() != '-', description + " must be nonnegative");
  size_t parsed_characters = 0;
  unsigned long long value = 0;
  try {
    value = std::stoull(std::string(text), &parsed_characters);
  } catch (const std::exception &exception) {
    throw std::runtime_error("Invalid " + description + " '" +
                             std::string(text) + "': " + exception.what());
  }
  require(parsed_characters == text.size() &&
              value <= static_cast<unsigned long long>(
                           std::numeric_limits<int64_t>::max()),
          "Invalid " + description + " '" + std::string(text) + "'");
  return static_cast<int64_t>(value);
}

FrameSelection parseWindow(std::string_view text) {
  const auto delimiter = text.find(':');
  require(delimiter != std::string_view::npos &&
              text.find(':', delimiter + 1) == std::string_view::npos,
          "Window must use inclusive FIRST:LAST syntax");
  FrameSelection selection{
      parseNonnegativeInt64(text.substr(0, delimiter), "window start"),
      parseNonnegativeInt64(text.substr(delimiter + 1), "window end")};
  require(selection.last >= selection.first, "Window end precedes its start");
  return selection;
}

void addSelection(Options *options, FrameSelection selection) {
  require(options != nullptr, "Internal option parser error");
  require(options->selections.size() < kMaximumSelections,
          "Too many selections; the maximum is " +
              std::to_string(kMaximumSelections));
  const int64_t frame_delta = selection.last - selection.first;
  require(frame_delta < kMaximumFramesPerSelection,
          "A selection may span at most " +
              std::to_string(kMaximumFramesPerSelection) + " frames");
  const int64_t frames = frame_delta + 1;
  int64_t total = frames;
  for (const auto &existing : options->selections) {
    total += existing.last - existing.first + 1;
  }
  require(total <= kMaximumFramesAcrossSelections,
          "Selections may request at most " +
              std::to_string(kMaximumFramesAcrossSelections) +
              " frames in total");
  options->selections.push_back(selection);
}

void printUsage(const char *program) {
  std::cerr
      << "Usage: " << program
      << " --zarr ARCHIVE.zarr --run EXACT_RUN [--frame N | --window A:B]..."
         " [--output RESULT.json]\n"
      << "With no --frame/--window, probes frames 53990,54000,54010,"
         "2592020,2592030,2592040,2937603. Windows are inclusive.\n";
}

Options parseOptions(int argc, char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    require(index + 1 < argc, "Missing value for " + argument);
    const std::string value = argv[++index];
    if (argument == "--zarr") {
      require(options.archive.empty(), "--zarr was specified more than once");
      options.archive = value;
    } else if (argument == "--run") {
      require(options.run.empty(), "--run was specified more than once");
      options.run = value;
    } else if (argument == "--frame") {
      const auto frame = parseNonnegativeInt64(value, "frame");
      addSelection(&options, {frame, frame});
    } else if (argument == "--window") {
      addSelection(&options, parseWindow(value));
    } else if (argument == "--output") {
      require(!options.output.has_value(),
              "--output was specified more than once");
      options.output = value;
    } else {
      throw std::runtime_error("Unknown argument: " + argument);
    }
  }
  require(!options.archive.empty(), "--zarr is required");
  require(!options.run.empty(), "--run is required");
  require(options.run != "." && options.run != ".." &&
              options.run.find('/') == std::string::npos,
          "--run must be one exact run name");
  require(std::filesystem::is_directory(options.archive),
          "Zarr archive is not a directory: " + options.archive.string());
  if (options.selections.empty()) {
    for (const auto frame : kDefaultFrames) {
      addSelection(&options, {frame, frame});
    }
  }
  return options;
}

const char *selectionKindName(
    crimson::zarr::DetectionRepositorySelectionKind kind) {
  using Kind = crimson::zarr::DetectionRepositorySelectionKind;
  switch (kind) {
  case Kind::ExplicitRefinedV1:
    return "explicit_refined_v1";
  case Kind::ApprovedAuthoritativeRefinedV1:
    return "approved_authoritative_refined_v1";
  case Kind::ExplicitlyPermittedCanonicalRaw:
    return "explicitly_permitted_canonical_raw";
  }
  return "unknown";
}

const char *surfaceKindName(crimson::zarr::DetectionSurfaceKind kind) {
  using Kind = crimson::zarr::DetectionSurfaceKind;
  switch (kind) {
  case Kind::CanonicalRawV1:
    return "canonical_raw_v1";
  case Kind::RefinedSnapshotV1:
    return "refined_snapshot_v1";
  }
  return "unknown";
}

const char *pageStatusName(crimson::zarr::CanonicalDetectionPageStatus status) {
  using Status = crimson::zarr::CanonicalDetectionPageStatus;
  switch (status) {
  case Status::Ready:
    return "ready";
  case Status::OutOfRange:
    return "out_of_range";
  case Status::ReadFailed:
    return "read_failed";
  }
  return "unknown";
}

json canonicalOpenMetricsJson(
    const crimson::zarr::CanonicalDetectionRepositoryOpenMetrics &metrics) {
  return {
      {"total_ms", metrics.total_ms},
      {"root_metadata_ms", metrics.root_metadata_ms},
      {"exact_handle_open_ms", metrics.exact_handle_open_ms},
      {"offset_read_ms", metrics.offset_read_ms},
      {"root_metadata_reads", metrics.root_metadata_reads},
      {"direct_group_metadata_reads", metrics.direct_group_metadata_reads},
      {"consolidated_array_declarations",
       metrics.consolidated_array_declarations},
      {"exact_handle_opens", metrics.exact_handle_opens},
      {"fallback_metadata_reads", metrics.fallback_metadata_reads},
      {"fallback_dtype_opens", metrics.fallback_dtype_opens},
      {"offset_read_calls", metrics.offset_read_calls},
      {"retained_offset_bytes", metrics.retained_offset_bytes},
  };
}

json repositoryMetricsJson(
    const crimson::zarr::CanonicalDetectionRepositoryMetrics &metrics) {
  return {
      {"range_reads", metrics.range_reads},
      {"paged_range_reads", metrics.paged_range_reads},
      {"resident_range_reads", metrics.resident_range_reads},
      {"resolved_frames", metrics.resolved_frames},
      {"resolved_rows", metrics.resolved_rows},
      {"failed_reads", metrics.failed_reads},
      {"ui_field_reads", metrics.ui_field_reads},
      {"residency_chunk_reads", metrics.residency_chunk_reads},
      {"residency_rows_read", metrics.residency_rows_read},
      {"residency_decoded_bytes", metrics.residency_decoded_bytes},
      {"resident_publications", metrics.resident_publications},
      {"resident_retained_bytes", metrics.resident_retained_bytes},
      {"peak_concurrent_ui_field_reads",
       metrics.peak_concurrent_ui_field_reads},
      {"maximum_range_read_ms", metrics.maximum_range_read_ms},
      {"maximum_residency_chunk_read_ms",
       metrics.maximum_residency_chunk_read_ms},
      {"last_error", metrics.last_error},
  };
}

json detectionJson(const crimson::zarr::CanonicalDetection &detection,
                   const crimson::coordinates::TransformAuthority &authority) {
  const crimson::coordinates::CenterSizeBox normalized{
      detection.normalized_cxcywh[0], detection.normalized_cxcywh[1],
      detection.normalized_cxcywh[2], detection.normalized_cxcywh[3]};
  const auto pixels =
      crimson::coordinates::normalizedCenterSizeBoxToContinuousPixelXyxy(
          normalized, authority);
  require(pixels.has_value(),
          "A ready canonical detection could not be converted to source "
          "camera pixels");
  return {
      {"row_index", detection.row_index},
      {"bbox_norm_cxcywh", detection.normalized_cxcywh},
      {"bbox_source_camera_pixel_xyxy",
       {pixels->x_min, pixels->y_min, pixels->x_max, pixels->y_max}},
      {"score", detection.score},
      {"class_id", detection.class_id},
  };
}

json runPass(const char *name,
             const std::vector<FrameSelection> &selections,
             crimson::zarr::CanonicalDetectionRepository *repository,
             const crimson::coordinates::TransformAuthority &authority) {
  require(repository != nullptr, "Detection repository is unavailable");
  json result = {{"name", name}, {"selections", json::array()}};
  uint64_t decoded_bytes = 0;
  const auto pass_started = Clock::now();
  for (const auto &selection : selections) {
    const auto read_started = Clock::now();
    const auto page = repository->resolveCameraFrameRange(selection.first,
                                                          selection.last);
    json selection_json = {
        {"first_camera_frame", selection.first},
        {"last_camera_frame", selection.last},
        {"elapsed_ms", elapsedMilliseconds(read_started)},
        {"status", pageStatusName(page.status)},
        {"decoded_logical_bytes", page.decoded_bytes},
        {"error", page.error},
        {"frames", json::array()},
    };
    require(page.status == crimson::zarr::CanonicalDetectionPageStatus::Ready,
            "Detection range " + std::to_string(selection.first) + ":" +
                std::to_string(selection.last) + " failed: " + page.error);
    decoded_bytes += page.decoded_bytes;
    for (const auto &frame : page.frames) {
      json frame_json = {
          {"camera_frame", frame.camera_frame},
          {"detection_count", frame.detections.size()},
          {"detections", json::array()},
      };
      for (const auto &detection : frame.detections) {
        frame_json["detections"].push_back(detectionJson(detection, authority));
      }
      selection_json["frames"].push_back(std::move(frame_json));
    }
    result["selections"].push_back(std::move(selection_json));
  }
  result["elapsed_ms"] = elapsedMilliseconds(pass_started);
  result["decoded_logical_bytes"] = decoded_bytes;
  result["repository_metrics_after"] =
      repositoryMetricsJson(repository->metrics());
  return result;
}

uint64_t peakRssBytes() {
#if defined(__unix__) || defined(__APPLE__)
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0) {
    return 0;
  }
#if defined(__APPLE__)
  return static_cast<uint64_t>(usage.ru_maxrss);
#else
  return static_cast<uint64_t>(usage.ru_maxrss) * 1024;
#endif
#else
  return 0;
#endif
}

void emitResult(const json &result,
                const std::optional<std::filesystem::path> &output) {
  if (output) {
    std::ofstream stream(*output);
    require(stream.good(), "Could not open output: " + output->string());
    stream << result.dump(2) << '\n';
    require(stream.good(), "Could not write output: " + output->string());
  }
  std::cout << result.dump(2) << '\n';
}

} // namespace

int main(int argc, char **argv) {
  const auto process_started = Clock::now();
  std::optional<Options> options;
  json result = {
      {"schema_id", "crimson.canonical_detection_window_probe"},
      {"schema_version", 1},
      {"pass", false},
      {"read_only", true},
      {"residency", "disabled"},
      {"byte_accounting",
       {{"decoded_logical_bytes_are_wire_bytes", false},
        {"physical_io_measured", false},
        {"description",
         "Decoded logical bytes count requested UI columns only; sharding, "
         "compression, cache, filesystem, and network transfer are outside "
         "this counter."}}},
  };
  try {
    options = parseOptions(argc, argv);
    result["archive"] = options->archive.string();
    result["requested_run"] = options->run;
    result["requested_selections"] = json::array();
    for (const auto &selection : options->selections) {
      result["requested_selections"].push_back(
          {{"first_camera_frame", selection.first},
           {"last_camera_frame", selection.last}});
    }

    std::string error;
    const auto archive_started = Clock::now();
    auto archive = crimson::zarr::ArchiveContext::Open(options->archive, &error);
    const double archive_open_ms = elapsedMilliseconds(archive_started);
    require(archive != nullptr, "Archive open failed: " + error);

    crimson::zarr::DetectionRepositorySelectionRequest request;
    request.canonical_raw_run = options->run;
    request.raw_fallback_policy =
        crimson::zarr::DetectionRawFallbackPolicy::AllowOnlyWhenNoRefinedAuthority;
    crimson::zarr::DetectionRepositorySelectionMetrics selection_metrics;
    const auto repository_started = Clock::now();
    auto repository = crimson::zarr::OpenSelectedDetectionRepository(
        archive, request, &error, &selection_metrics);
    const double repository_open_wall_ms =
        elapsedMilliseconds(repository_started);
    require(repository != nullptr, "Detection selection/open failed: " + error);
    require(selection_metrics.kind ==
                crimson::zarr::DetectionRepositorySelectionKind::
                    ExplicitlyPermittedCanonicalRaw,
            "The selected repository is not the requested canonical raw run");

    const auto &descriptor = repository->descriptor();
    require(descriptor.surface_kind ==
                crimson::zarr::DetectionSurfaceKind::CanonicalRawV1 &&
                descriptor.run_name == options->run && descriptor.ready(),
            "Canonical descriptor does not identify the requested ready run");
    require(descriptor.source_width <=
                    static_cast<size_t>(std::numeric_limits<int64_t>::max()) &&
                descriptor.source_height <=
                    static_cast<size_t>(std::numeric_limits<int64_t>::max()),
            "Source dimensions exceed the coordinate transform domain");
    for (const auto &selection : options->selections) {
      require(selection.last <
                  static_cast<int64_t>(descriptor.camera_frame_count),
              "Requested frame is outside the canonical archive");
    }

    crimson::coordinates::TransformAuthority authority;
    authority.source_dimensions = {
        static_cast<int64_t>(descriptor.source_width),
        static_cast<int64_t>(descriptor.source_height)};
    require(authority.validForSourceCamera(),
            "Canonical descriptor has invalid source dimensions");

    result["initialization"] = {
        {"archive_open_ms", archive_open_ms},
        {"repository_open_wall_ms", repository_open_wall_ms},
        {"cache_pool_bytes", archive->cachePoolBytes()},
        {"selection_kind", selectionKindName(selection_metrics.kind)},
        {"authority_metadata_reads",
         selection_metrics.authority_metadata_reads},
        {"canonical_open",
         canonicalOpenMetricsJson(selection_metrics.canonical_open)},
    };
    result["descriptor"] = {
        {"surface_kind", surfaceKindName(descriptor.surface_kind)},
        {"source_group", descriptor.source_group},
        {"run_name", descriptor.run_name},
        {"instance_group", descriptor.instance_group},
        {"run_manifest_digest", descriptor.run_manifest_digest},
        {"row_count", descriptor.row_count},
        {"camera_frame_count", descriptor.camera_frame_count},
        {"source_width", descriptor.source_width},
        {"source_height", descriptor.source_height},
        {"retained_offset_bytes", descriptor.retained_offset_bytes},
        {"offset_read_calls", descriptor.offset_read_calls},
        {"consolidated_metadata", descriptor.consolidated_metadata},
        {"stable_identity", descriptor.stable_identity},
        {"source_audit_lazy", descriptor.source_audit_lazy},
        {"authority_approved", descriptor.authority_approved},
        {"coordinate_catalog_validated",
         descriptor.coordinate_catalog_validated},
    };

    result["passes"] = json::array();
    result["passes"].push_back(runPass("first_read", options->selections,
                                        repository.get(), authority));
    result["passes"].push_back(runPass("warm_repeat", options->selections,
                                        repository.get(), authority));
    result["repository_final"] = repositoryMetricsJson(repository->metrics());
    require(!repository->residentUiColumnsReady() &&
                repository->metrics().residency_chunk_reads == 0 &&
                repository->metrics().resident_publications == 0,
            "Probe unexpectedly activated canonical detection residency");
    result["peak_rss_bytes"] = peakRssBytes();
    result["elapsed_ms"] = elapsedMilliseconds(process_started);
    result["pass"] = true;
    emitResult(result, options->output);
    return 0;
  } catch (const std::exception &exception) {
    result["error"] = exception.what();
    result["peak_rss_bytes"] = peakRssBytes();
    result["elapsed_ms"] = elapsedMilliseconds(process_started);
    try {
      emitResult(result, options ? options->output : std::nullopt);
    } catch (const std::exception &output_exception) {
      std::cerr << "canonical_detection_window_probe: "
                << output_exception.what() << '\n';
    }
    printUsage(argv[0]);
    return 1;
  }
}
