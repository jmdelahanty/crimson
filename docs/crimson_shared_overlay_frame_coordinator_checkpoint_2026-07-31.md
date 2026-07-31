# Shared Overlay Frame Coordinator Checkpoint

Date: 2026-07-31

Status: backend-neutral frame policy and subject-mask adapter implemented;
macOS subject-mask presentation adopted; Linux legacy adapter adoption pending

## Boundary

`ReadOnlyOverlayFrameCoordinator` owns only frame request and presentation
policy. It has no Zarr, GUI, coordinate, decoder, or renderer dependency. Its
two-stage API:

1. plans whether a layer should issue a request and whether an already active
   source should receive a discontinuity; and
2. accepts the adapter's request/result status and decides whether to wait,
   clear, present, fail, reject, or ignore stale work.

Plans carry a monotonically increasing sequence. A completion from a
superseded plan, a duplicate completion, or a candidate for the wrong camera
frame cannot reach presentation. Metrics cover inactive and unavailable
frames, accepted and rejected requests, discontinuities, pending/missing/failed
frames, exact presentations, mismatches, stale completions, and superseded
plans. Diagnostic formatting is shared as well.

This contract does not perform scientific coordinate conversion. Source/ROI
geometry remains in the existing backend-neutral coordinate and scene
adapters. Metal and OpenGL/CUDA retain only final drawable projection and GPU
resource handling.

## Subject-Mask Adapter

`SubjectMaskPresentationCoordinator` is the first typed adapter over the shared
policy. It owns the complete repository-buffer-to-scene glue:

- forwards current-frame demand to `SubjectMaskOverlayBuffer`;
- maps subject-mask repository statuses into the shared candidate vocabulary;
- rejects nonexact or superseded results;
- appends an exact mapped result through the existing subject-mask scene
  adapter; and
- returns only readiness and presentation counts to the application shell.

The macOS render loop now makes one adapter update call. It still owns user
visibility state, failure reporting, and Metal rendering. The extraction
removed the local `last_subject_mask_camera_request` state and reduced
`crimson_macos_main.mm` by ten lines for this first adoption despite adding
portable diagnostics.

Legacy subject-mask discovery and storage remain unchanged. The strict v1 and
legacy repositories both feed the same shared buffer and presentation adapter.

## Verification

Headless tests cover:

- inactive, unavailable, and invalid frames;
- pending, exact, missing, and failed candidates;
- discontinuity forwarding only after an accepted request;
- rejected requests;
- mismatched-frame, superseded-plan, and duplicate-completion rejection;
- session reset and portable diagnostics; and
- a real shared buffer resolving mapped and missing subject-mask frames into a
  backend-neutral overlay scene.

The tests pass on macOS and in the isolated `ws1` Linux/CUDA 12.4 worktree.
The Linux/NVIDIA `redgui` target also links the new shared components with
architectures `80;86`; no shared checkout or dataset was changed.

A mounted Metal GUI smoke against Palette's immutable 23,287-frame strict-v1
fixture passed frames 1000--1020. It recorded 22 exact mask presentations, zero
stale or mismatched candidates, zero rejected requests, and one pending
presentation probe when the video smoke closed on its final frame. The video
smoke itself passed at frame 1020 with 202.4 MiB peak RSS.

## Next Adoption

The next safe use is the keypoint buffer because its repository, scene adapter,
and long-duration behavior are already validated. Subject-shape and eye-
geometry adoption should wait for their Palette contracts. The Linux legacy
mask path currently performs a synchronous current-frame load inside its
camera-view context builder; it can use the generic frame policy through a thin
legacy adapter without changing storage or editing behavior.

