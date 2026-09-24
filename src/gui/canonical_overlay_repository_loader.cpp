#include "gui/canonical_overlay_session.h"
#include "gui/bound_subject_mask_contour_cache.h"

#include "zarr/archive_context.h"
#include "zarr/tensorstore_bound_keypoint_overlay_repository.h"
#include "zarr/tensorstore_subject_mask_overlay_repository.h"
#include "zarr/tensorstore_subject_shape_overlay_repository.h"
#include "zarr/tensorstore_bound_subject_shape_overlay_repository.h"
#include "zarr/tensorstore_bound_eye_geometry_overlay_repository.h"
#include "zarr/shared_mask_frame_index.h"

#include <exception>

namespace crimson::gui {
CanonicalOverlayRepositories openCanonicalOverlayRepositories(
    const CanonicalOverlayOpenRequest& request) {
  CanonicalOverlayRepositories result;
  auto archive = zarr::ArchiveContext::Open(request.archive_path, &result.error);
  if (!archive) return result;
  zarr::CanonicalOverlaySelectionRequest selection_request;
  selection_request.eye_run = request.eye_run;
  selection_request.expected_recording_id = request.recording_id;
  selection_request.expected_frame_count = request.frame_count;
  selection_request.expected_source_width = request.source_width;
  selection_request.expected_source_height = request.source_height;
  auto selection = zarr::SelectCanonicalOverlaySources(archive, selection_request, &result.error);
  if (!selection) return result;
  result.selection = *selection;
  std::string shared_index_error;
  std::shared_ptr<const zarr::SharedMaskFrameIndex> shared_mask_frame_index;
  if (selection->mask.valid) {
    try {
      shared_mask_frame_index = zarr::OpenSharedMaskFrameIndex(
          archive, *selection, &shared_index_error);
    } catch (const std::exception& exception) {
      shared_index_error = exception.what();
    } catch (...) {
      shared_index_error = "Shared mask index opener threw an unknown exception";
    }
    if (!shared_mask_frame_index && shared_index_error.empty())
      shared_index_error = "Bound mask frame index is unavailable";
  }

  const auto open_product = [](const auto& open, std::string& error) {
    try { open(); }
    catch (const std::exception& exception) { error = exception.what(); }
    catch (...) { error = "Overlay product opener threw an unknown exception"; }
  };
  result.keypoint_error = selection->keypoints.error;
  result.mask_error = selection->mask.error;
  result.shape_error = selection->shape.error;
  if (selection->mask.valid && !shared_mask_frame_index) {
    result.mask_error = shared_index_error;
    result.mask_contour_error = shared_index_error;
    if (selection->shape.valid) result.shape_error = shared_index_error;
  }
  if (selection->keypoints.valid) {
    open_product([&] {
      zarr::BoundKeypointOverlayOpenRequest keypoints;
      keypoints.archive = archive;
      keypoints.selection = *selection;
      result.keypoints = zarr::OpenBoundKeypointOverlayRepository(keypoints, &result.keypoint_error);
    }, result.keypoint_error);
  }
  if (selection->mask.valid && shared_mask_frame_index) {
    open_product([&] {
      zarr::SubjectMaskOverlayOpenOptions masks;
      masks.requested_run = selection->mask.run_id;
      masks.expected_manifest_payload_digest = selection->mask.manifest_payload_digest;
      masks.allow_selector_ineligible = selection->mask.bound_selector_exception;
      masks.require_strict_v1 = true;
      masks.shared_mask_frame_index = shared_mask_frame_index;
      masks.max_read_rows = 8;
      masks.max_cached_payload_bytes = 64ULL * 1024 * 1024;
      masks.max_storage_chunk_bytes = 512ULL * 1024 * 1024;
      masks.max_mapping_bytes = 768ULL * 1024 * 1024;
      masks.max_observations_per_frame = 16;
      masks.disable_prefetch = true;
      masks.serial_dense_channels = true;
      result.masks = zarr::OpenSubjectMaskOverlayRepository(archive, masks, &result.mask_error);
      if (result.masks && result.masks->descriptor().camera_frame_count != request.frame_count) {
        result.masks.reset();
        result.mask_error = "Bound mask frame domain disagrees with indexed video";
      }
    }, result.mask_error);
    if (result.masks) {
      const auto cache = findBoundSubjectMaskContourCache(
          archive->rootPath(), selection->mask.run_id,
          selection->mask.manifest_payload_digest,
          &result.mask_contour_error);
      if (cache) {
        open_product([&] {
          zarr::SubjectMaskOverlayOpenOptions contours;
          contours.requested_run = selection->mask.run_id;
          contours.expected_manifest_payload_digest =
              selection->mask.manifest_payload_digest;
          contours.allow_selector_ineligible =
              selection->mask.bound_selector_exception;
          contours.require_strict_v1 = true;
          contours.presentation_cache_archive = archive;
          contours.presentation_cache_run = cache->run;
          contours.expected_presentation_cache_manifest_payload_digest =
              cache->digest;
          contours.contour_only = true;
          contours.shared_mask_frame_index = shared_mask_frame_index;
          contours.max_read_rows = 8;
          contours.max_cached_payload_bytes = 64ULL * 1024 * 1024;
          contours.max_mapping_bytes = 768ULL * 1024 * 1024;
          contours.max_observations_per_frame = 16;
          contours.disable_prefetch = true;
          result.mask_contours = zarr::OpenSubjectMaskOverlayRepository(
              archive, contours, &result.mask_contour_error);
          if (result.mask_contours &&
              result.mask_contours->descriptor().camera_frame_count !=
                  request.frame_count) {
            result.mask_contours.reset();
            result.mask_contour_error =
                "Bound contour frame domain disagrees with indexed video";
          }
        }, result.mask_contour_error);
      }
    }
  }
  if (selection->shape.valid && shared_mask_frame_index) {
    open_product([&] {
      zarr::BoundSubjectShapeOverlayOpenRequest shapes;
      shapes.archive = archive;
      shapes.selection = *selection;
      shapes.shared_mask_frame_index = shared_mask_frame_index;
      result.shapes = zarr::OpenBoundSubjectShapeOverlayRepository(shapes, &result.shape_error);
      if (result.shapes && result.shapes->descriptor().camera_frame_count != request.frame_count) {
        result.shapes.reset();
        result.shape_error = "Bound shape frame domain disagrees with indexed video";
      }
    }, result.shape_error);
  }
  result.eye_error = selection->eye.error;
  if (selection->eye.valid && selection->mask.valid &&
      !shared_mask_frame_index) result.eye_error = shared_index_error;
  if (selection->eye.valid && shared_mask_frame_index) {
    open_product([&] {
      zarr::BoundEyeGeometryOverlayOpenRequest eyes;
      eyes.archive = archive;
      eyes.selection = *selection;
      eyes.shared_mask_frame_index = shared_mask_frame_index;
      result.eyes = zarr::OpenBoundEyeGeometryOverlayRepository(
          eyes, &result.eye_error);
      if (result.eyes && result.eyes->descriptor().camera_frame_count !=
                             request.frame_count) {
        result.eyes.reset();
        result.eye_error = "Bound eye frame domain disagrees with indexed video";
      }
    }, result.eye_error);
  }
  return result;
}
} // namespace crimson::gui
