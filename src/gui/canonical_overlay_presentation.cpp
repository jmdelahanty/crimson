#include "gui/canonical_overlay_presentation.h"

#include "zarr/subject_mask_overlay_scene_adapter.h"
#include "zarr/subject_shape_overlay_scene_adapter.h"

#include <cmath>
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
  checkFrame(snapshot.shapes, presented_frame);
  if (snapshot.selection) {
    const auto check_source = [](auto& product, const auto& binding) {
      if (product.frame && (!binding.valid || product.descriptor.run_name != binding.run_id))
        fail(product, "Overlay source differs from the immutable bound selection");
    };
    check_source(snapshot.keypoints, snapshot.selection->keypoints);
    check_source(snapshot.masks, snapshot.selection->mask);
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
