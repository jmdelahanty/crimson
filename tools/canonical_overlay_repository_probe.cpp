#include "gui/canonical_overlay_presentation.h"
#include "zarr/archive_context.h"
#include <chrono>
#include <iostream>
#include <thread>
#include <sys/resource.h>
#include <nlohmann/json.hpp>

using namespace crimson;
using namespace std::chrono_literals;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
bool terminal(gui::CanonicalOverlayState state) {
  return state != gui::CanonicalOverlayState::Pending && state != gui::CanonicalOverlayState::Opening;
}
template<typename Product>
Json product(const Product& value) {
  Json output = {{"state", gui::canonicalOverlayStateName(value.state)},
                 {"run",value.descriptor.run_name},{"error",value.error}};
  output["keys"] = Json::array();
  if (value.frame) {
    output["frame"] = value.frame->camera_frame;
    for (const auto& row : value.frame->detections) output["keys"].push_back(row.instance_key);
  }
  return output;
}
bool ready(gui::CanonicalOverlayState state) {
  return state == gui::CanonicalOverlayState::Ready || state == gui::CanonicalOverlayState::Empty;
}
int main(int argc, char** argv) {
  if (argc < 2 || argc > 34) {
    std::cerr << "Usage: canonical_overlay_repository_probe ARCHIVE.zarr [FRAME ...] (max32)\n";
    return 2;
  }
  std::string error;
  auto archive = zarr::ArchiveContext::Open(argv[1],&error);
  if (!archive) { std::cerr << error << '\n'; return 1; }
  auto selection = zarr::SelectCanonicalOverlaySources(archive,{},&error);
  if (!selection) { std::cerr << error << '\n'; return 1; }
  std::vector<int64_t> frames;
  for (int i=2;i<argc;++i) {
    try {
      size_t end=0; auto frame=std::stoll(argv[i],&end);
      if (end != std::string(argv[i]).size() || frame<0 ||
          static_cast<uint64_t>(frame)>=selection->frame_count) throw std::invalid_argument("frame");
      frames.push_back(frame);
    } catch (...) { std::cerr << "Invalid frame: " << argv[i] << '\n'; return 2; }
  }
  if (frames.empty()) frames={0,54000,static_cast<int64_t>(selection->frame_count-1)};
  auto scheduler=std::make_shared<data::DataAccessScheduler>(64,4,1,1);
  gui::CanonicalOverlaySession session(scheduler);
  gui::CanonicalOverlayOpenRequest request;
  request.archive_path=argv[1]; request.recording_id=selection->recording_id;
  request.frame_count=selection->frame_count;
  request.source_width=selection->source_width; request.source_height=selection->source_height;
  request.eye_run=selection->eye.run_id;
  if (!session.beginOpen(request,&error) || !session.waitUntilOpen(120s)) {
    std::cerr << "Open failed/timed out: " << error << '\n'; return 1;
  }
  auto initial=session.snapshot(-1);
  Json output={{"archive",argv[1]},{"recording",selection->recording_id},
               {"open_ms",initial.open_ms},{"open_error",initial.error},
               {"mask_storage_read_chunk_rows",initial.masks.descriptor.storage_chunk_rows}};
  bool passed=initial.state==gui::CanonicalOverlayState::Ready;
  for (auto frame:frames) {
    auto started=Clock::now();
    if (!session.requestFrame(frame,true,true,true,true)) passed=false;
    gui::CanonicalOverlaySnapshot snapshot;
    do {
      snapshot=session.snapshot(frame);
      if (terminal(snapshot.keypoints.state) && terminal(snapshot.masks.state) &&
          terminal(snapshot.shapes.state)) break;
      std::this_thread::sleep_for(1ms);
    } while (Clock::now()-started < 30s);
    auto presentation=gui::makeCanonicalOverlayPresentation(snapshot,0,frame,
        request.source_width,request.source_height,{});
    snapshot=presentation.snapshot;
    bool frame_pass=ready(snapshot.keypoints.state) && ready(snapshot.masks.state) && ready(snapshot.shapes.state);
    Json sample={{"frame",frame},{"passed",frame_pass},
      {"resolve_ms",std::chrono::duration<double,std::milli>(Clock::now()-started).count()},
      {"keypoints",product(snapshot.keypoints)},{"masks",product(snapshot.masks)},
      {"shapes",product(snapshot.shapes)},{"mask_rasters",presentation.masks.raster_masks.size()},
      {"shape_primitives",presentation.shapes.primitives.size()}};
    sample["observations"]=Json::array();
    for (const auto& row:presentation.keypoints.detections) {
      Json observation={{"key",row.instance_key},{"valid",row.instance_key_valid},
                        {"frame",row.acquisition_frame},{"keypoint_valid",row.keypoint_valid}};
      observation["points"]=Json::array();
      for (const auto& p:row.keypoints) observation["points"].push_back({p.x,p.y});
      if (row.heading_degrees) observation["heading_deg"]=*row.heading_degrees;
      sample["observations"].push_back(observation);
    }
    if (snapshot.masks.frame) for (const auto& row:snapshot.masks.frame->detections) {
      Json mask={{"key",row.instance_key},{"roi",{row.roi_x,row.roi_y,row.roi_width,row.roi_height}}};
      mask["components"]=Json::array();
      for (const auto& c:row.components) {
        uint64_t foreground=0;
        if (c.mask) for (auto byte:*c.mask) foreground+=byte!=0;
        mask["components"].push_back({{"label",c.label},{"present",c.present},{"foreground",foreground}});
      }
      sample["mask_samples"].push_back(mask);
    }
    const auto& m=snapshot.mask_metrics;
    sample["mask_metrics"]={{"mapping_retained",m.metadata_retained_bytes},
      {"cached_payload",m.cached_payload_bytes},{"peak_cached_payload",m.peak_cached_payload_bytes},
      {"logical_payload_bytes",m.chunk_source_bytes_read},{"payload_read_calls",m.dense_mask_payload_reads},
      {"maximum_chunk_read_ms",m.maximum_chunk_read_ms},{"chunk_read_ms",m.chunk_read_ms},
      {"prefetch_requests",m.prefetch_requests},{"chunk_load_failures",m.chunk_load_failures}};
    output["samples"].push_back(std::move(sample));
    std::cerr << "frame=" << frame << " passed=" << frame_pass << '\n';
    passed=passed&&frame_pass;
  }
  session.shutdown();
  auto metrics=scheduler->metrics();
  output["scheduler"]={{"work_started",metrics.work_started},{"work_completed",metrics.work_completed},
    {"failed",metrics.queue.failed_completions},{"cancelled",metrics.queue.cancelled_requests},
    {"discarded",metrics.queue.discarded_completions},{"peak_pending",metrics.queue.peak_pending_requests}};
  scheduler->shutdown();
  rusage usage{};
  if (getrusage(RUSAGE_SELF,&usage)==0) output["max_rss_kib"]=usage.ru_maxrss;
  output["success"]=passed;
  std::cout << output.dump(2) << '\n';
  return passed?0:1;
}
