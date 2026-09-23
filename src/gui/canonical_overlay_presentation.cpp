#include "gui/canonical_overlay_presentation.h"

#include "zarr/subject_mask_overlay_scene_adapter.h"
#include "zarr/subject_shape_overlay_scene_adapter.h"

#include <cmath>
#include <iterator>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace crimson::gui {
namespace {
bool resolved(CanonicalOverlayState state) {
  return state == CanonicalOverlayState::Ready || state == CanonicalOverlayState::Empty;
}
template <typename Product>
void fail(Product& product, const char* message) {
  product.state = CanonicalOverlayState::Failed;
  product.error = message;
  product.frame.reset();
}
template <typename Product>
void checkFrame(Product& product, int64_t frame) {
  if (product.frame && product.frame->camera_frame != frame)
    fail(product, "Overlay frame does not match the authoritative presented frame");
}
template <typename Product>
bool sameKeys(const std::unordered_set<uint64_t>& keys, const Product& product) {
  if (!product.frame || keys.size() != product.frame->detections.size()) return false;
  std::unordered_set<uint64_t> seen;
  for (const auto& detection : product.frame->detections) {
    if (!keys.count(detection.instance_key) || !seen.insert(detection.instance_key).second) return false;
  }
  return true;
}
}

CanonicalOverlayPresentation makeCanonicalOverlayPresentation(
    CanonicalOverlaySnapshot snapshot, int view, int64_t presented_frame,
    int source_width, int source_height,
    const overlay::ReadOnlyOverlayControlState& controls) {
  CanonicalOverlayPresentation result;
  checkFrame(snapshot.keypoints, presented_frame);
  checkFrame(snapshot.masks, presented_frame);
  checkFrame(snapshot.mask_contours, presented_frame);
  checkFrame(snapshot.shapes, presented_frame);
  if (snapshot.selection) {
    const auto check_source = [](auto& product, const auto& binding) {
      if (product.frame && (!binding.valid || product.descriptor.run_name != binding.run_id))
        fail(product, "Overlay source differs from the immutable bound selection");
    };
    check_source(snapshot.keypoints, snapshot.selection->keypoints);
    check_source(snapshot.masks, snapshot.selection->mask);
    check_source(snapshot.mask_contours, snapshot.selection->mask);
    check_source(snapshot.shapes, snapshot.selection->shape);
  }
  std::unordered_set<uint64_t> keys;
  bool keys_available = resolved(snapshot.keypoints.state) && snapshot.keypoints.frame;
  if (keys_available) {
    bool invalid_identity = false;
    for (const auto& detection : snapshot.keypoints.frame->detections) {
      if (!detection.instance_key_valid || !keys.insert(detection.instance_key).second) {
        invalid_identity = true;
        break;
      }
    }
    if (invalid_identity) {
      fail(snapshot.keypoints, "Canonical keypoint identity is missing or duplicated");
      keys_available = false;
    }
  }
  // These exact eye-bound products advertise identical observation identity.
  // Do not join any of them to unrelated raw detector rows by ordinal.
  if (keys_available && resolved(snapshot.masks.state) && snapshot.masks.frame &&
      !sameKeys(keys, snapshot.masks))
    fail(snapshot.masks, "Bound mask/keypoint observation keys disagree for this frame");
  if (keys_available && resolved(snapshot.mask_contours.state) &&
      snapshot.mask_contours.frame && !sameKeys(keys, snapshot.mask_contours))
    fail(snapshot.mask_contours,
         "Bound contour/keypoint observation keys disagree for this frame");
  if (keys_available && resolved(snapshot.shapes.state) && snapshot.shapes.frame &&
      !sameKeys(keys, snapshot.shapes))
    fail(snapshot.shapes, "Bound shape/keypoint observation keys disagree for this frame");
  if (resolved(snapshot.shapes.state) && snapshot.shapes.frame) {
    std::unordered_set<uint64_t> shape_keys;
    bool invalid_identity = false;
    for (const auto& shape : snapshot.shapes.frame->detections) {
      if (!shape.instance_key_valid || !shape_keys.insert(shape.instance_key).second) {
        invalid_identity = true;
        break;
      }
    }
    if (invalid_identity)
      fail(snapshot.shapes, "Canonical shape identity is missing or duplicated");
  }
  if (!keys_available && resolved(snapshot.masks.state) && snapshot.masks.frame &&
      resolved(snapshot.shapes.state) && snapshot.shapes.frame) {
    std::unordered_set<uint64_t> mask_keys;
    for (const auto& mask : snapshot.masks.frame->detections) mask_keys.insert(mask.instance_key);
    if (mask_keys.size() != snapshot.masks.frame->detections.size() || !sameKeys(mask_keys, snapshot.shapes))
      fail(snapshot.shapes, "Bound shape/mask observation keys disagree for this frame");
  }

  if (resolved(snapshot.keypoints.state) && snapshot.keypoints.frame) {
    result.keypoints = *snapshot.keypoints.frame;
    result.keypoints_ready = true;
    if (resolved(snapshot.shapes.state) && snapshot.shapes.frame) {
      std::unordered_map<uint64_t, const zarr::SubjectShapeOverlayDetection*> shapes;
      for (const auto& shape : snapshot.shapes.frame->detections) {
        if (shape.instance_key_valid) shapes.emplace(shape.instance_key, &shape);
      }
      for (auto& point : result.keypoints.detections) {
        auto found = shapes.find(point.instance_key);
        if (!point.instance_key_valid || found == shapes.end()) continue;
        const auto& geometry = found->second->geometry;
        if (!geometry.body_frame_valid || !geometry.body_axis_valid ||
            !geometry.heading_degrees || !std::isfinite(*geometry.heading_degrees) ||
            !std::isfinite(geometry.body_origin.x) || !std::isfinite(geometry.body_origin.y)) continue;
        point.heading_origin = zarr::KeypointOverlayPoint{geometry.body_origin.x, geometry.body_origin.y};
        point.heading_degrees = geometry.heading_degrees;
        point.heading_valid = true;
        point.heading_from_body_frame = true;
      }
    }
  }
  if (resolved(snapshot.masks.state) && snapshot.masks.frame) {
    auto input = zarr::makeSubjectMaskOverlaySceneInput(
        snapshot.masks.descriptor, *snapshot.masks.frame, view, presented_frame,
        view, source_width, source_height);
    overlay::applyReadOnlyOverlayControls(controls, &input);
    result.masks = overlay::buildReadOnlyOverlayScene(input);
  }
  const bool contours_requested = controls.independent_mask_contours &&
      (controls.show_subject_body_contour || controls.show_eye_left_contour ||
       controls.show_eye_right_contour || controls.show_swim_bladder_contour);
  if (contours_requested &&
      snapshot.mask_contours.state == CanonicalOverlayState::Ready &&
      snapshot.mask_contours.frame && resolved(snapshot.masks.state) &&
      snapshot.masks.frame && result.masks.ready()) {
    const auto &dense = snapshot.masks.descriptor;
    const auto &contours = snapshot.mask_contours.descriptor;
    const bool bound_source = dense.strict_v1 && contours.strict_v1 &&
        contours.contour_only && !contours.presentation_cache_run.empty() &&
        dense.run_name == contours.run_name &&
        dense.run_manifest_payload_digest == contours.run_manifest_payload_digest &&
        dense.cache_namespace == contours.cache_namespace &&
        snapshot.selection && snapshot.selection->mask.valid &&
        contours.run_manifest_payload_digest ==
            snapshot.selection->mask.manifest_payload_digest;
    if (!bound_source) {
      fail(snapshot.mask_contours,
           "Contour source differs from the exact bound dense mask source");
    } else {
      std::unordered_map<uint64_t, const zarr::SubjectMaskOverlayDetection*>
          dense_by_key;
      std::unordered_set<uint64_t> contour_keys;
      bool aligned = snapshot.masks.frame->detections.size() ==
                     snapshot.mask_contours.frame->detections.size();
      for (const auto &row : snapshot.masks.frame->detections)
        aligned = dense_by_key.emplace(row.instance_key, &row).second && aligned;
      for (const auto &row : snapshot.mask_contours.frame->detections) {
        const auto found = dense_by_key.find(row.instance_key);
        if (!contour_keys.insert(row.instance_key).second ||
            found == dense_by_key.end() ||
            found->second->source_crop_row_id != row.source_crop_row_id ||
            found->second->roi_x != row.roi_x ||
            found->second->roi_y != row.roi_y ||
            found->second->roi_width != row.roi_width ||
            found->second->roi_height != row.roi_height) {
          aligned = false;
          break;
        }
        std::set<std::pair<size_t, std::string>> dense_components;
        for (const auto &candidate : found->second->components)
          aligned = dense_components.emplace(candidate.channel_index,
                                              candidate.label).second && aligned;
        std::set<std::pair<size_t, std::string>> contour_components;
        for (const auto &component : row.components) {
          const auto identity = std::make_pair(component.channel_index,
                                               component.label);
          aligned = contour_components.insert(identity).second &&
                    dense_components.count(identity) == 1 && aligned;
        }
        aligned = contour_components == dense_components && aligned;
        if (!aligned) break;
      }
      aligned = contour_keys.size() == dense_by_key.size() && aligned;
      if (!aligned) {
        fail(snapshot.mask_contours,
             "Contour observation/component identity differs from dense masks");
      } else {
        auto input = zarr::makeSubjectMaskOverlaySceneInput(
            contours, *snapshot.mask_contours.frame, view, presented_frame,
            view, source_width, source_height);
        overlay::applyReadOnlyOverlayControls(controls, &input);
        input.show_subject_mask_fills = false;
        auto contour_scene = overlay::buildReadOnlyOverlayScene(input);
        if (contour_scene.ready()) {
          result.masks.primitives.insert(result.masks.primitives.end(),
              std::make_move_iterator(contour_scene.primitives.begin()),
              std::make_move_iterator(contour_scene.primitives.end()));
        }
      }
    }
  }
  if (resolved(snapshot.shapes.state) && snapshot.shapes.frame) {
    auto input = zarr::makeSubjectShapeOverlaySceneInput(
        snapshot.shapes.descriptor, *snapshot.shapes.frame, view, presented_frame,
        view, source_width, source_height);
    overlay::applyReadOnlyOverlayControls(controls, &input);
    result.shapes = overlay::buildReadOnlyOverlayScene(input);
  }
  result.snapshot = std::move(snapshot);
  return result;
}
} // namespace crimson::gui
