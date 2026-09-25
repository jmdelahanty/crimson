#include "gui/canonical_overlay_presentation.h"
#include "zarr/subject_mask_overlay_scene_adapter.h"
#include "zarr/subject_shape_overlay_scene_adapter.h"
#include "zarr/archive_context.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <string_view>
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
size_t labelled(const overlay::ReadOnlyOverlayScene& scene, std::string_view prefix) {
  size_t count = 0;
  for (const auto& primitive : scene.primitives)
    count += primitive.label.compare(0, prefix.size(), prefix) == 0;
  return count;
}
Json inspectPresentation(const gui::CanonicalOverlaySnapshot& snapshot,
                         int64_t frame, int width, int height) {
  Json result = Json::object();
  bool passed = true;
  if (snapshot.masks.frame) {
    const auto fill_input = zarr::makeSubjectMaskOverlaySceneInput(
        snapshot.masks.descriptor, *snapshot.masks.frame, 0, frame, 0, width, height);
    auto contours_input = snapshot.mask_contours.frame
        ? zarr::makeSubjectMaskOverlaySceneInput(
              snapshot.mask_contours.descriptor, *snapshot.mask_contours.frame,
              0, frame, 0, width, height)
        : fill_input;
    if (!snapshot.mask_contours.frame) contours_input.subject_masks.clear();
    size_t contour_sources = 0, fill_sources = 0;
    std::map<std::string, size_t> contour_labels, fill_labels;
    for (const auto& component : contours_input.subject_masks) {
      if (component.contour.size() > 1) { ++contour_sources; ++contour_labels[component.label]; }
    }
    for (const auto& component : fill_input.subject_masks) {
      if (component.mask && component.mask_width > 0 && component.mask_height > 0 &&
          component.mask_width <= SIZE_MAX / component.mask_height &&
          component.mask->size() == component.mask_width * component.mask_height &&
          component.source_rect.valid()) { ++fill_sources; ++fill_labels[component.label]; }
    }
    contours_input.show_subject_mask_fills = false;
    const auto contours = overlay::buildReadOnlyOverlayScene(contours_input);
    auto fills_input = fill_input;
    fills_input.show_subject_mask_contours = false;
    const auto fills = overlay::buildReadOnlyOverlayScene(fills_input);
    const size_t drawn_contours = labelled(contours, "##mask_contour_");
    bool contour_coordinates_preserved = true;
    for (const auto& component : contours_input.subject_masks) {
      if (component.contour.size() < 2) continue;
      const auto label = "##mask_contour_" + component.label + "_" +
                         std::to_string(component.source_crop_row_id);
      bool found = false;
      for (const auto& primitive : contours.primitives) if (primitive.label == label) {
        found = true;
        contour_coordinates_preserved &= !primitive.points.empty() &&
            std::abs(primitive.points.front().x - component.contour.front().x) < 1e-3 &&
            std::abs(primitive.points.front().y - component.contour.front().y) < 1e-3;
      }
      contour_coordinates_preserved &= found;
    }
    bool mask_pass = contours.ready() && fills.ready() &&
        contours.raster_masks.empty() && fills.primitives.empty() &&
        drawn_contours == contour_sources && fills.raster_masks.size() == fill_sources &&
        contour_coordinates_preserved;
    Json component_checks = Json::object();
    for (const auto& label : snapshot.masks.descriptor.component_labels) {
      auto component_input = fill_input;
      component_input.subject_masks.insert(component_input.subject_masks.end(),
          contours_input.subject_masks.begin(), contours_input.subject_masks.end());
      component_input.show_subject_body_mask = label == "subject_body";
      component_input.show_eye_left_mask = label == "eye_left";
      component_input.show_eye_right_mask = label == "eye_right";
      component_input.show_swim_bladder_mask = label == "swim_bladder";
      auto scene = overlay::buildReadOnlyOverlayScene(component_input);
      const size_t expected_contours = contour_labels[label];
      const size_t expected_fills = fill_labels[label];
      auto outline_input = contours_input;
      outline_input.independent_mask_contours = true;
      outline_input.show_subject_mask_fills = false;
      outline_input.show_subject_body_mask = false;
      outline_input.show_eye_left_mask = false;
      outline_input.show_eye_right_mask = false;
      outline_input.show_swim_bladder_mask = false;
      outline_input.show_subject_body_contour = label == "subject_body";
      outline_input.show_eye_left_contour = label == "eye_left";
      outline_input.show_eye_right_contour = label == "eye_right";
      outline_input.show_swim_bladder_contour = label == "swim_bladder";
      const auto outline_scene = overlay::buildReadOnlyOverlayScene(outline_input);
      const bool ok = scene.ready() && outline_scene.ready() &&
          labelled(scene, "##mask_contour_") == expected_contours &&
          scene.raster_masks.size() == expected_fills &&
          outline_scene.raster_masks.empty() &&
          labelled(outline_scene, "##mask_contour_") == expected_contours;
      component_checks[label] = {{"passed", ok}, {"contours", expected_contours},
                                 {"fills", expected_fills}};
      mask_pass &= ok;
    }
    result["masks"] = {{"passed", mask_pass}, {"source_contours", contour_sources},
        {"contour_only_drawn", drawn_contours}, {"source_fills", fill_sources},
        {"fill_only_drawn", fills.raster_masks.size()},
        {"contour_coordinates_preserved", contour_coordinates_preserved},
        {"components", component_checks}};
    passed &= mask_pass;
  }
  if (snapshot.shapes.frame) {
    const auto input = zarr::makeSubjectShapeOverlaySceneInput(
        snapshot.shapes.descriptor, *snapshot.shapes.frame, 0, frame, 0, width, height);
    auto all_input = input;
    all_input.show_subject_shape_bspline_debug_points = true;
    all_input.show_subject_shape_bspline_control_points = true;
    all_input.show_subject_shape_tail_samples = true;
    all_input.show_subject_shape_tail_normals = true;
    const auto scene = overlay::buildReadOnlyOverlayScene(all_input);
    const size_t splines = labelled(scene, "##shape_bspline_") -
        labelled(scene, "##shape_bspline_debug_") -
        labelled(scene, "##shape_bspline_control_") -
        labelled(scene, "##shape_bspline_controls_");
    const size_t samples = labelled(scene, "##shape_bspline_debug_");
    const size_t controls = labelled(scene, "##shape_bspline_control_");
    const size_t tails = labelled(scene, "##shape_tail_sample_");
    size_t expected_splines = 0, expected_samples = 0, expected_controls = 0, expected_tails = 0;
    bool coordinate_pass = true;
    for (const auto& shape : input.subject_shapes) {
      if (shape.bspline_valid && shape.bspline_sample.size() > 1) ++expected_splines;
      if (shape.bspline_valid) expected_samples += shape.bspline_sample.size();
      if (shape.bspline_valid) expected_controls += shape.bspline_control_points.size();
      if (shape.tail_sample_valid) expected_tails += shape.tail_samples.size();
      if (snapshot.shapes.descriptor.geometry_in_source_camera_coordinates &&
          shape.bspline_valid && shape.bspline_sample.size() > 1) {
        const auto label = "##shape_bspline_" + std::to_string(shape.shape_row);
        bool found = false;
        for (const auto& primitive : scene.primitives) if (primitive.label == label) {
          found = true;
          coordinate_pass &= !primitive.points.empty() &&
              std::abs(primitive.points.front().x - shape.bspline_sample.front().x) < 1e-3 &&
              std::abs(primitive.points.front().y - shape.bspline_sample.front().y) < 1e-3;
        }
        coordinate_pass &= found;
      }
    }
    const bool shape_pass = scene.ready() && coordinate_pass &&
        splines == expected_splines && samples == expected_samples &&
        controls == expected_controls && tails == expected_tails;
    result["shapes"] = {{"passed", shape_pass}, {"splines", splines},
        {"spline_points", samples}, {"control_points", controls},
        {"tail_samples", tails}, {"source_coordinates_preserved", coordinate_pass}};
    passed &= shape_pass;
  }
  result["passed"] = passed;
  return result;
}
Json inspectContourJoin(const gui::CanonicalOverlaySnapshot& snapshot,
                        int64_t frame) {
  if (!snapshot.mask_contours.frame) return {{"passed", true}, {"checked", false}};
  bool passed = snapshot.masks.frame &&
      snapshot.masks.frame->camera_frame == frame &&
      snapshot.mask_contours.frame->camera_frame == frame;
  size_t compared = 0;
  if (snapshot.masks.frame && snapshot.mask_contours.frame) {
    std::map<uint64_t, const zarr::SubjectMaskOverlayDetection*> fills;
    for (const auto& row : snapshot.masks.frame->detections)
      passed &= fills.emplace(row.instance_key, &row).second;
    passed &= fills.size() == snapshot.mask_contours.frame->detections.size();
    for (const auto& row : snapshot.mask_contours.frame->detections) {
      const auto found = fills.find(row.instance_key);
      if (found == fills.end()) { passed = false; continue; }
      const auto& fill = *found->second;
      passed &= row.source_crop_row_id == fill.source_crop_row_id &&
          row.detection_index == fill.detection_index &&
          std::abs(row.roi_x - fill.roi_x) < 1e-3 &&
          std::abs(row.roi_y - fill.roi_y) < 1e-3 &&
          std::abs(row.roi_width - fill.roi_width) < 1e-3 &&
          std::abs(row.roi_height - fill.roi_height) < 1e-3 &&
          row.components.size() == fill.components.size();
      for (size_t i = 0; i < std::min(row.components.size(), fill.components.size()); ++i)
        passed &= row.components[i].label == fill.components[i].label &&
                  row.components[i].channel_index == fill.components[i].channel_index;
      ++compared;
    }
  }
  return {{"passed", passed}, {"checked", true}, {"rows", compared}};
}
int main(int argc, char** argv) {
  if (argc < 2 || argc > 36) {
    std::cerr << "Usage: canonical_overlay_repository_probe ARCHIVE.zarr [--require-contours] [--require-shape-samples] [FRAME ...] (max32)\n";
    return 2;
  }
  std::string error;
  auto archive = zarr::ArchiveContext::Open(argv[1],&error);
  if (!archive) { std::cerr << error << '\n'; return 1; }
  auto selection = zarr::SelectCanonicalOverlaySources(archive,{},&error);
  if (!selection) { std::cerr << error << '\n'; return 1; }
  bool require_contours = false, require_shape_samples = false;
  std::vector<int64_t> frames;
  for (int i=2;i<argc;++i) {
    if (std::string_view(argv[i]) == "--require-contours") { require_contours = true; continue; }
    if (std::string_view(argv[i]) == "--require-shape-samples") { require_shape_samples = true; continue; }
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
               {"mask_storage_read_chunk_rows",initial.masks.descriptor.storage_chunk_rows},
               {"mask_contour_only",initial.mask_contours.descriptor.contour_only},
               {"mask_contour_cache_run",initial.mask_contours.descriptor.presentation_cache_run},
               {"mask_contour_cache_digest",
                initial.mask_contours.descriptor.presentation_cache_manifest_payload_digest},
               {"mask_open_error",initial.masks.error},
               {"mask_contour_error",initial.mask_contour_error}};
  bool passed=initial.state==gui::CanonicalOverlayState::Ready;
  size_t total_contours = 0, total_shape_samples = 0;
  size_t total_control_points = 0, total_tail_samples = 0;
  std::map<std::string, size_t> total_contours_by_label;
  uint64_t contour_payload_reads_observed = 0;
  bool contour_error_free = initial.mask_contour_error.empty();
  for (auto frame:frames) {
    auto started=Clock::now();
    if (!session.requestFrame(frame,true,true,true,true,{},require_contours)) passed=false;
    const auto immediate=session.snapshot(frame);
    const auto current_or_empty = [frame](const auto& product) {
      return !product.frame || product.frame->camera_frame == frame;
    };
    const bool no_stale_after_seek = current_or_empty(immediate.keypoints) &&
        current_or_empty(immediate.masks) && current_or_empty(immediate.mask_contours) &&
        current_or_empty(immediate.shapes);
    gui::CanonicalOverlaySnapshot snapshot;
    do {
      snapshot=session.snapshot(frame);
      if (terminal(snapshot.keypoints.state) && terminal(snapshot.masks.state) &&
          terminal(snapshot.shapes.state) &&
          (!require_contours || terminal(snapshot.mask_contours.state))) break;
      std::this_thread::sleep_for(1ms);
    } while (Clock::now()-started < 30s);
    overlay::ReadOnlyOverlayControlState controls;
    controls.independent_mask_contours = true;
    controls.show_subject_body_contour = require_contours;
    controls.show_eye_left_contour = require_contours;
    controls.show_eye_right_contour = require_contours;
    controls.show_swim_bladder_contour = require_contours;
    auto presentation=gui::makeCanonicalOverlayPresentation(snapshot,0,frame,
        request.source_width,request.source_height,controls);
    snapshot=presentation.snapshot;
    const bool frame_contour_error_free = snapshot.mask_contour_error.empty() &&
                                          snapshot.mask_contours.error.empty();
    if (require_contours) contour_error_free &= frame_contour_error_free;
    bool frame_pass=ready(snapshot.keypoints.state) && ready(snapshot.masks.state) && ready(snapshot.shapes.state);
    if (require_contours) frame_pass &= ready(snapshot.mask_contours.state);
    auto overlay_checks = inspectPresentation(snapshot, frame, request.source_width,
                                              request.source_height);
    const auto contour_join = inspectContourJoin(snapshot, frame);
    const size_t presented_contours = labelled(presentation.masks, "##mask_contour_");
    const size_t source_contours = overlay_checks.contains("masks")
        ? overlay_checks["masks"]["source_contours"].get<size_t>() : 0;
    frame_pass &= overlay_checks["passed"].get<bool>() &&
                  contour_join["passed"].get<bool>() && no_stale_after_seek &&
                  presented_contours == source_contours;
    if (overlay_checks.contains("masks")) {
      total_contours += overlay_checks["masks"]["contour_only_drawn"].get<size_t>();
      for (auto it = overlay_checks["masks"]["components"].begin();
           it != overlay_checks["masks"]["components"].end(); ++it)
        total_contours_by_label[it.key()] += it.value()["contours"].get<size_t>();
    }
    if (overlay_checks.contains("shapes")) {
      total_shape_samples += overlay_checks["shapes"]["spline_points"].get<size_t>();
      total_control_points += overlay_checks["shapes"]["control_points"].get<size_t>();
      total_tail_samples += overlay_checks["shapes"]["tail_samples"].get<size_t>();
    }
    Json sample={{"frame",frame},{"passed",frame_pass},
      {"resolve_ms",std::chrono::duration<double,std::milli>(Clock::now()-started).count()},
      {"keypoints",product(snapshot.keypoints)},{"masks",product(snapshot.masks)},
      {"mask_contours",product(snapshot.mask_contours)},
      {"shapes",product(snapshot.shapes)},{"mask_rasters",presentation.masks.raster_masks.size()},
      {"presented_contours",presented_contours},
      {"shape_primitives",presentation.shapes.primitives.size()},
      {"overlay_checks",overlay_checks},{"contour_join",contour_join},
      {"no_stale_after_seek",no_stale_after_seek},
      {"contour_error_free",frame_contour_error_free}};
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
    const auto& c=snapshot.mask_contour_metrics;
    contour_payload_reads_observed = std::max(contour_payload_reads_observed,
                                              c.contour_payload_reads);
    sample["mask_metrics"]={{"mapping_retained",m.metadata_retained_bytes},
      {"cached_payload",m.cached_payload_bytes},{"peak_cached_payload",m.peak_cached_payload_bytes},
      {"logical_payload_bytes",m.chunk_source_bytes_read},{"payload_read_calls",m.dense_mask_payload_reads},
      {"contour_payload_reads",m.contour_payload_reads},
      {"contour_source_bytes",m.contour_source_bytes_read},
      {"contour_open_ms",m.contour_open_ms},
      {"contour_load_ms",m.contour_load_ms},
      {"maximum_chunk_read_ms",m.maximum_chunk_read_ms},{"chunk_read_ms",m.chunk_read_ms},
      {"prefetch_requests",m.prefetch_requests},{"chunk_load_failures",m.chunk_load_failures}};
    sample["mask_contour_metrics"]={{"contour_payload_reads",c.contour_payload_reads},
      {"contour_source_bytes",c.contour_source_bytes_read},
      {"contour_open_ms",c.contour_open_ms},{"contour_load_ms",c.contour_load_ms},
      {"chunk_load_failures",c.chunk_load_failures}};
    if (!require_contours) frame_pass &= c.contour_payload_reads == 0;
    sample["passed"] = frame_pass;
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
  output["required_contours"] = require_contours;
  output["required_shape_samples"] = require_shape_samples;
  output["total_contours"] = total_contours;
  output["total_contours_by_label"] = total_contours_by_label;
  output["total_shape_samples"] = total_shape_samples;
  output["total_control_points"] = total_control_points;
  output["total_tail_samples"] = total_tail_samples;
  output["contour_error_free"] = contour_error_free;
  output["contour_payload_reads_observed"] = contour_payload_reads_observed;
  bool each_component_has_contours = true;
  for (const auto& label : initial.masks.descriptor.component_labels)
    each_component_has_contours &= total_contours_by_label[label] > 0;
  passed &= (!require_contours || (total_contours > 0 && contour_error_free)) &&
            (!require_shape_samples || (total_shape_samples > 0 &&
                                        total_control_points > 0 &&
                                        total_tail_samples > 0));
  if (require_contours) passed &= each_component_has_contours;
  rusage usage{};
  if (getrusage(RUSAGE_SELF,&usage)==0) output["max_rss_kib"]=usage.ru_maxrss;
  output["success"]=passed;
  std::cout << output.dump(2) << '\n';
  return passed?0:1;
}
