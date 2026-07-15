#include "analysis_series_timeline.h"
#include "zarr/archive_context.h"
#include "zarr/tensorstore_analysis_series_timeline_repository.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

const char *roleName(crimson::timeline::AnalysisSeriesTraceRole role) {
  using crimson::timeline::AnalysisSeriesTraceRole;
  switch (role) {
  case AnalysisSeriesTraceRole::PrimarySpeed:
    return "primary_speed";
  case AnalysisSeriesTraceRole::SecondarySpeed:
    return "secondary_speed";
  case AnalysisSeriesTraceRole::HeadingRaw:
    return "heading_raw";
  case AnalysisSeriesTraceRole::HeadingSmoothed:
    return "heading_smoothed";
  case AnalysisSeriesTraceRole::PositionX:
    return "position_x";
  case AnalysisSeriesTraceRole::PositionY:
    return "position_y";
  case AnalysisSeriesTraceRole::TailTipAngle:
    return "tail_tip_angle";
  case AnalysisSeriesTraceRole::MaxAbsTailAngle:
    return "max_abs_tail_angle";
  case AnalysisSeriesTraceRole::TailTipLateralDeflection:
    return "tail_tip_lateral_deflection";
  case AnalysisSeriesTraceRole::MaxAbsTailCurvature:
    return "max_abs_tail_curvature";
  case AnalysisSeriesTraceRole::Other:
    return "other";
  }
  return "other";
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 3 || argc > 6) {
    std::cerr << "Usage: analysis_series_timeline_repository_probe ZARR "
                 "motion|tail [FRAME] [FRAME_COUNT] [SOURCE]\n";
    return 2;
  }
  const std::string kind = argv[2];
  const int64_t frame = argc >= 4 ? std::strtoll(argv[3], nullptr, 10) : 0;
  const size_t frame_count =
      argc >= 5 ? static_cast<size_t>(std::strtoull(argv[4], nullptr, 10)) : 0;
  std::string error;
  const auto open_start = std::chrono::steady_clock::now();
  auto archive = crimson::zarr::ArchiveContext::Open(argv[1], &error);
  if (!archive) {
    std::cerr << error << '\n';
    return 1;
  }
  std::unique_ptr<crimson::timeline::AnalysisSeriesTimelineRepository>
      repository;
  if (kind == "motion") {
    repository = crimson::zarr::OpenMotionSeriesTimelineRepository(
        archive, frame_count, &error);
  } else if (kind == "tail") {
    repository = crimson::zarr::OpenTailKinematicsTimelineRepository(
        archive, frame_count, {}, &error);
  } else {
    std::cerr << "Kind must be motion or tail\n";
    return 2;
  }
  if (!repository) {
    std::cerr << error << '\n';
    return 1;
  }
  const double open_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - open_start)
                             .count();
  const auto &descriptor = repository->descriptor();
  const std::string source =
      argc >= 6 ? argv[5]
                : crimson::timeline::defaultAnalysisSeriesSource(descriptor);
  const auto bounds = crimson::timeline::analysisSeriesTimelinePageBounds(
      frame, descriptor.frame_count, 4096, 2048);
  if (!bounds.valid()) {
    std::cerr << "Frame is outside the descriptor range\n";
    return 1;
  }
  crimson::timeline::AnalysisSeriesTimelineRequest request;
  request.source_key = source;
  request.first_frame = bounds.first_frame;
  request.last_frame = bounds.last_frame;
  request.anchor_frame = frame;
  request.max_points_per_trace = 1200;
  request.fallback_frames_per_second = 100.0;
  const auto read_start = std::chrono::steady_clock::now();
  const auto window = repository->resolveWindow(request);
  const double read_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - read_start)
                             .count();
  if (!window.ready()) {
    std::cerr << window.error << '\n';
    return 1;
  }
  std::cout << "kind=" << kind << " title=" << descriptor.title
            << " sources=" << descriptor.sources.size()
            << " default=" << descriptor.default_source
            << " frame_count=" << descriptor.frame_count << " source=" << source
            << " frames=" << bounds.first_frame << ':' << bounds.last_frame
            << " rows=" << window.source_row_count
            << " traces=" << window.traces.size()
            << " points=" << window.published_point_count
            << " open_ms=" << open_ms << " read_ms=" << read_ms << '\n';
  for (const auto &trace : window.traces) {
    std::cout << "trace=" << trace.descriptor.key
              << " role=" << roleName(trace.descriptor.role)
              << " units=" << trace.descriptor.units
              << " default=" << (trace.descriptor.default_visible ? 1 : 0)
              << " points=" << trace.values.size() << '\n';
  }
  return 0;
}
