# August Sleepyfish Linux overlays — implementation checklist

Date: 2026-09-21

Status: read-only August integration implemented in the active worktree and
native-tested. See [the implementation report](linux_sleepyfish_overlay_integration_2026-09-21.md)
for exact evidence and limits. Unchecked items are not claimed complete.

User decision during implementation: finish this increment first; defer mask
contour outlines and swim-bout/core shading on the speed graph. Filled masks,
keypoints, valid headings and shape lines are in this increment. A fresh
Ubuntu 22 colleague package has not been built or published from these changes.

## Stable checkpoint

- [x] Active worktree is `crimson-linux-priority-20260916`, branch
  `codex/linux-priority-20260916`; the old `crimson-ui-monolith` checkout is an
  archived July branch and is not the implementation target.
- [x] Application baseline is clean commit
  `b90f8e6ca43774009a0f6d3133d3d4e8ceaf2aa2`:
  “Fix accurate seek frame identity and reject mismatched settlement.”
- [x] Existing Ubuntu 22 build passed **95/95 CTest tests** again on September 21.
  This reran the existing build; it did not rebuild or interrupt the user's GUI.
  Local evidence: `/tmp/crimson-overlay-checklist-20260921.CpnB4e/baseline-ctest.log`.
- [x] Baseline pushed to the existing GitHub branch and remote SHA verified.
  Existing GitHub CLI credentials were used over HTTPS after SSH-key
  authentication failed; saved Git configuration was not changed.
- [x] The September 17 colleague package still represents this baseline.
  This integration does not replace or republish that package.

This is a tested **partial-feature checkpoint**, not a claim that the missing
overlays already work or that the colleague's A4000/driver 535 is qualified.
The prior integration and paused-seek evidence remains in
[the timeline report](linux_sleepyfish_timeline_integration_2026-09-17.md),
[the detection report](linux_sleepyfish_detection_integration_2026-09-16.md), and
[the seek report](linux_sleepyfish_paused_seek_fix_2026-09-17.md).

## Scope and non-goals

Make the maintained August keypoints/headings, refined subject masks and
subject-shape geometry available alongside the existing video, canonical boxes,
scores/classes, and eye/speed/bout timelines. The first increment is read-only,
one camera at a time, with exact frame and observation identity.

Do not regenerate data, alter selectors, re-encode clips, fabricate a legacy
detail bundle, or implement editing/export, arbitrary multi-track selection,
tail-trace panels, or unrelated stimulus/quality products. Keep older May/June
archive playback and overlays working.

## Baseline findings that determined implementation order

| Surface | Baseline implementation | Required change |
| --- | --- | --- |
| Source selection | Timeline reads validate bound sources, but the overlay session does not adopt those bindings | One validated, immutable product selection for the session |
| Detection identity | August stores `instances/instance_key`; the raw reader and NVIDIA bridge omit it | Read and transport the published key with validity/source scope, then verify cross-product joins |
| Keypoints | Linux uses the legacy repository; canonical requests explicitly disable it | Exact August-schema reader support and asynchronous canonical snapshots |
| Refined masks | The bound August run matches the existing strict-v1 manifest-v5 envelope; that reader is not wired into Linux canonical presentation | Qualify and reuse the strict reader with the exact bound run/digest; do not use the legacy path |
| Shape | Existing reader expects `refined_subject_mask_rows` and old row-index arrays | Support August schema v5 and its `recording_subject_mask_bundle_rows` mapping |
| Presentation | Canonical boxes bypass old details, but mask/shape controls and readiness still depend on them | Compose typed read-only scenes without reinstating legacy-detail gating |

Evidence entry points:

- [Linux setup and route](../src/red.cpp): legacy keypoint repository at 459–486;
  canonical Frame Inspect requests at 2740–2759; camera requests at 3667–3683.
- [Legacy loading gate](../src/zarr_loader.cpp): 638–697 skips keypoint, mask,
  shape and tail opening when the old detection layout is absent.
- [Canonical raw reader](../src/zarr/tensorstore_canonical_detection_repository.cpp):
  353–363 does not populate the existing `CanonicalDetection::instance_key`.
- [NVIDIA bridge](../src/platform/nvidia/nvidia_detection_repository.cpp):
  571–583 does not transport that key to `DetectionObservation`.
- [Shape reader](../src/zarr/tensorstore_subject_shape_overlay_repository.cpp):
  673–715 requires the older row axis and row-index arrays.
- [Eye-angle reader](../src/zarr/tensorstore_eye_angle_timeline_repository.cpp):
  645–682 already validates explicit keypoint/mask/shape source bindings.

Line numbers refer to the baseline and may move during implementation.

### Stored identity is present; the gap is in the consumer

For camera 2010093, the selected raw detection run publishes
`instances/instance_key` as uint64 with 2,937,192 entries. Its manifest defines
the key as a stable content-derived observation identity, with
`row_identity=instance_key`. The current raw reader opens boxes, scores, classes
and offsets, but does not open this key column or assign the existing C++ key
field. The NVIDIA bridge also omits the key. This is a Crimson integration gap,
not evidence that the underlying data or maintained mappings are wrong.

The eye-bound keypoint and mask runs each advertise 2,936,291 keys and the same
key-array digest/shape. Shape also advertises a matching row-identity contract.
These are metadata facts, not recomputed payload hashes. The raw detection and
downstream row counts differ, and the crop snapshot feeding keypoints names a
different refined-detection source. Verify sampled per-frame keys and their
published lineage before associating downstream geometry with a raw box;
neither assume an error nor assume every downstream key is in the raw set.
Do not require unrelated raw-box joins to render a product whose own frame,
coordinate and upstream identity contracts have been validated.

The bounded camera-2010093 audit selected these exact run IDs:

- Eye: `eye_angles_sleepyfish_2026_08_06_worker_receipt_20260901_v002_sleepyfish_cam2010093`.
- Keypoints: `keypoints_coordinate_successor_sleepyfish_2026_08_06_direct_hybrid_20260826_v002_sleepyfish_cam2010093`.
- Mask: `refined_subject_masks_sleepyfish_2026_08_06_component_area_support_20260830_v004_sleepyfish_cam2010093`.
- Shape: `subject_shape_sleepyfish_2026_08_06_component_area_support_20260831_v002_sleepyfish_cam2010093`.

Keypoints and mask are complete but selector-ineligible dependencies explicitly
bound by the selected eye product. The shape is eligible and binds that mask.
These exact names are an audit example, not hardcoded defaults for other cameras.

## 1. Freeze exact product and identity contracts

- [ ] Add a bounded, read-only source-inventory probe for all four August cameras.
  Record recording ID, parent-frame domain, source dimensions, selected products,
  schema versions, completion/eligibility, exact upstream paths, advertised
  manifest identities and available mapping/coordinate columns.
- [x] Default the scientifically paired overlay set to the validated upstream
  bindings of the selected eye-angle/shape products. The eye-bound
  coordinate-successor keypoints differ from `keypoints_runs.latest`.
  Do not independently choose every group's `latest`.
- [x] Accept a complete, selector-ineligible upstream only through its explicit
  validated binding. An empty parent selector is not proof that a bound source
  is absent. Do not add a global “ignore eligibility” fallback or mutate selectors.
- [x] Expose that immutable selection through a canonical overlay-session
  request/snapshot, reusing or factoring the timeline source-selection logic.
  Include archive/recording, runs, schema/manifest identities, frame domain and
  coordinate authority in source/cache identities.
- [ ] Qualify the published raw `instances/instance_key` column on all four
  cameras and compare bounded per-frame samples against the exact downstream
  run bindings. Use the existing keys, not new synthetic identities. If a
  pairing uses different identities, validate its published upstream crosswalk
  before association; never derive a key from a row ordinal.
- [x] Specify joins by recording/source scope, exact bound run, parent frame and
  validated `instance_key` (or an explicitly proven contract crosswalk).
  Preserve keys as uint64; distinguish absent identity from numeric zero.
  Reject duplicates, wrong frames, wrong runs and ambiguous matches.
  Observation keys are not subject/track IDs; keep those concepts separate.
- [x] Support filtered/reordered downstream rows without assuming raw-key-set
  inclusion: no equal-row-count requirement, positional zip, nearest-frame
  match, or use of clip-local indices as parent IDs.
  Preserve raw detections when a product has no valid corresponding observation.

Exit: small synthetic fixtures and bounded real metadata establish one exact,
reviewed selection/identity contract before a renderer or selector is changed.

## 2. Implement narrow schema-aware readers and joins

- [x] Extend the canonical detection read/bridge path to expose validated
  observation identities without losing score/class validity, normalized-box
  storage semantics, source row handles or provenance.
  Include keys in paged and resident reads, byte accounting and downstream
  observation/scene types. The existing descriptor's `stable_identity` flag
  means the complete refined identity set; do not blindly repurpose it as a
  raw-key-present flag. Define explicit identity availability/authority semantics.
- [x] Evaluate `OpenKeypointV2Repository` against the actual August publication
  envelope, required quality/body-frame bindings and coordinate declarations.
  Reuse compatible code; add an explicit versioned branch where needed.
  Do not simply turn on the legacy repository or broaden generic schema gates.
  This API requires raw, quality and keypoint-body-frame artifacts; the selected
  eye product instead binds raw coordinate-successor keypoints and subject
  shape. Do not invent missing companion selections. A narrow raw-v2 overlay
  view plus the exact shape binding may be the appropriate adapter.
- [x] Read per-frame keypoints, confidences, per-point validity, success flags
  and declared heading/body-frame authority. Preserve missing or invalid values;
  do not invent confidence or geometry for unavailable observations.
  The bound `keypoints_img` is already full-camera continuous pixels with a
  top-left origin and x-right/y-down axes. Preserve its five-point order:
  swim bladder, left eye, right eye, snout tip, tail tip. Prefer this declared
  image surface over reconstructing it from auxiliary normalized/ROI arrays.
- [x] Qualify and reuse `OpenSubjectMaskOverlayRepository`'s strict-v1 path
  (`OpenStrictSubjectMaskV1`) for the exact August bound
  mask: `palette.subject_mask_core.run_manifest` v5, logical
  `palette.stage.refined_subject_mask_dense_core` v1,
  `recording_observations_with_frame_row_offsets_v1`, stored acquisition-frame
  and instance-key columns, and persisted four-component label order.
  Supply the explicit bound run/digest and narrowly authorized ineligible-run
  option. Keep strict validation intact; add a versioned branch only if a
  concrete unsupported contract is found. Metadata compatibility alone is not
  a successful runtime open or payload qualification.
- [ ] Validate a compatible digest-bound sampled-contour cache before using it
  for outline presentation. Do not assume a cache from another recording or
  publication version is usable. Filled masks require explicit bounded dense/
  packed/RLE reads when no suitable presentation surface exists.
- [x] Support shape v5's exact frame/key/mask-row contract and propagate stable
  observation identity through its declared rows in the exact bound mask run
  into shape resolution and scene types. Do not use positional joins or rename
  `recording_subject_mask_bundle_rows` to the old row axis.
  Reuse bound-mask frame ranges only after validating ordered key identity;
  otherwise provide a bounded index from the published mappings. The August
  heading authority is shape `body_frame/heading_deg`, gated by `axis_valid`:
  zero is camera +x, positive is counterclockwise after the y-axis flip.
  Do not substitute a legacy keypoint heading or unrelated body-frame run.
- [x] Preserve source coordinates on disk. Convert declared full-image,
  ROI-local or normalized coordinates through validated crop placement,
  coordinate dimensions and shared transform helpers. Apply camera scaling
  and Y-axis display inversion once, in presentation.
- [x] Fail only the affected product or incompatible pairing when possible.
  Keep valid boxes, video and independent timelines available with a visible
  reason for the missing overlay.

Exit: repository probes resolve exact requested frames/observations and agree
with an independent direct-array oracle on coordinates, component presence and
source identity. No UI is needed to prove these contracts.

## 3. Preserve storage-aware loading and bounded resources

- [x] Open/validate repositories off the UI thread. Reuse an archive context
  across paired overlay readers where safe; record existing consolidated-root
  metadata and offset costs rather than claiming all opening is constant-size.
  The strict mask path currently reads full frame/key/crop-placement mappings
  at open. Include their retained and temporary allocations in admission and
  startup measurements; generic legacy mapping-page limits do not cover them.
- [ ] Reuse `KeypointOverlayBuffer`, `SubjectMaskOverlayBuffer`,
  `ReadOnlyOverlayFrameCoordinator` and
  `SubjectMaskPresentationCoordinator`. UI queries must inspect snapshots,
  not call synchronous TensorStore repository resolution.
- [x] Adapt `SubjectShapeOverlayBuffer` to the shared scheduler/lifecycle or
  provide an equivalent bounded session wrapper. It currently owns a private
  worker; keypoint and mask buffers already accept the shared scheduler.
- [x] Keep current-frame demand ahead of directional prefetch and visible
  windows. Preserve reserved current-frame capacity so mask/timeline work
  cannot consume all workers while detection demand waits.
- [x] Coordinate or disable redundant repository-level mask prefetch when a
  session buffer already schedules it. The current mask repository has its
  own prefetch thread and can wait for an in-progress chunk.
- [x] Advance source/seek generations, cancel obsolete queued work, and reject
  stale completion at publication and drawing. Do not promise cancellation
  interrupts an already-running filesystem/TensorStore read.
- [x] Keep close/reopen and worker retirement off latency-critical UI paths:
  existing buffer close methods wait for source work to drain.
- [ ] Set and test explicit retained CPU/GPU and in-flight decode/upload limits.
  Use existing byte-budget admission helpers where applicable, counting shared
  ownership correctly. Reject oversized work predictably; do not silently grow
  caches or block the UI to satisfy it.
- [ ] Budget metadata/index retention, decoded mask chunks, per-frame snapshots,
  temporary conversions, TensorStore cache and GPU textures separately.
  Logical frame/page limits do not limit physical compressed-chunk reads or
  total RSS.
  Correction from the full sharding codec metadata: the August `masks_roi`
  outer shard is 2,872 rows × one 384×384 channel (about 404 MiB logically),
  but its indexed inner read chunks are eight rows × one 384×384 channel
  (1.125 MiB uncompressed). The published storage plan distinguishes both.
  Do not treat the outer shard as the decompression unit. Verify TensorStore's
  read layout and measure actual read, decompression and retained/transient
  behavior; neither logical extent is a measured physical read or RSS result.
- [x] Audit existing count-only limits: mask repository payload cache
  (3 dense or 8 packed/contour chunks), mask/keypoint buffers (default 24 frames),
  shape buffer (16), and Linux read-only mask texture cache (64 textures).
  Existing mask mapping caches are byte-limited, but this is not an end-to-end
  decoded-byte or GPU-memory guarantee.
- [x] Key GPU textures by immutable payload identity: archive/run/manifest,
  mask row/source-crop row and component. Keep frame/geometry placement in
  scene/presentation identity, not texture identity, to preserve payload reuse.
  Reject stale placement separately; invalidate safely on source change and
  release textures on the owning GL thread. Budget temporary RGBA uploads too.
- [ ] Instrument logical rows, physical reads/bytes, decoded/retained bytes,
  queue/service latency, cancellations, stale results, CPU RSS and GPU memory.
  Select and record numerical acceptance budgets before feature enablement,
  including the A4000's 16 GiB and the existing video-buffer allocation.

Exit: deterministic scheduler/cache tests demonstrate bounded admission,
current-frame priority, cache-only UI access and zero stale publication.
Benchmark evidence must name the exact August source and package, not inherit
qualification from a different subject-mask bundle.

## 4. Integrate canonical frame presentation and controls

- [x] Add a canonical overlay session around the existing Linux open/close flow
  and supply validated product snapshots to `CameraFrameDataAdapter` or an
  explicit companion interface. Keep legacy adapters intact.
- [x] Demand the authoritative presented camera parent frame, including the
  existing retained-frame policy during clip handoff. Never draw a newly
  requested frame's overlay over the previous camera surface.
- [x] Reuse keypoint, mask and shape scene adapters plus the shared read-only
  renderer. Do not fabricate `ZarrDetectionLoader::FrameDetections` solely to
  pass old camera-window or inspector gates.
- [x] Wire keypoint markers, declared heading arrows, mask components/fill
  and valid shape geometry into Camera View and Frame Inspect.
  Disable legacy editing tools on these read-only canonical sources.
- [ ] Deferred by user: add mask contour outlines and restore swim-bout/core
  shading on the speed trace after this increment is stable.
- [x] Represent each product as opening, pending, ready, valid-empty, unavailable
  or failed. A product failure must not become a silent empty result or prevent
  independent valid layers from drawing.
- [x] Make ready-empty clear the prior frame's geometry. Turning a layer off,
  changing source, seeking or closing must not leave old masks/textures visible.
- [x] Show exact product/run provenance and observation/frame identities in
  diagnostics. Distinguish raw camera boxes from the bound geometry product.
- [ ] If eye axes/gaze/angle arcs are included, separately qualify the v7
  eye-geometry reader and conventions. Working precomputed angle traces do not
  establish support for rendering their underlying geometry.
- [x] Label old “metadata-only / no scores / no class IDs” diagnostics as legacy
  probe results. Emit a route-aware session summary and explicit canonical
  product states; missing integration must not masquerade as missing archive data.

Exit: one camera has a fully identified, read-only frame view with correct
loading/error states and no legacy-detail dependency; existing plots still work.

## 5. Regression and real-data acceptance

- [x] Add metadata-contract negatives: missing/incomplete bound run, forbidden
  unbound ineligible run, wrong digest/schema/recording/dimensions, selector
  disagreement and mismatched shape-to-mask lineage.
- [x] Add identity fixtures with 0/1/multiple observations, reordered/filtered
  rows, sparse acquisition frames, duplicate keys, high uint64 keys, absent
  identities and identical row ordinals belonging to different runs.
- [ ] Add coordinate fixtures with non-origin/non-square ROIs, differing mask
  and camera resolutions, invalid/NaN points, empty components, orientation
  conventions, clipping and camera-display Y inversion.
- [ ] Add async tests for rapid forward/reverse seeks, in-flight source changes,
  reopening the same run, blocked storage, queue pressure, oversize admission,
  worker exceptions and current-frame work competing with speculative masks.
- [ ] Extend scene/Frame Inspect tests for product independence, valid-empty
  clearing, toggles, stale frame/source rejection and read-only controls.
- [x] Extend GUI capture markers: require exact video/query/overlay frame,
  selected run/manifest/observation identities and actual rendered primitive/
  component counts. The old `--ui-reference-state overlays` gate still
  assumes legacy eye masks; update the canonical gate, not merely the timeout.
- [x] Run the complete headless suite plus focused repeated scheduler/session
  regressions. Reuse existing keypoint/mask/shape repository, scene-adapter,
  frame-coordinator and NVIDIA frame-data tests; add canonical session/join tests.
- [ ] Probe all four August cameras at start, ordinary non-keyframes, both sides
  of 54,000, the final frame and per-camera metadata-derived late boundaries.
  Camera 94 frame 2,565,015 is a known raw-detection-empty case; do not assume
  its downstream product state without checking the product's own mapping.
- [ ] Compare sampled keypoints, ROI placement, masks/contours and shape geometry
  to an independent raw-array oracle; compare advertised identities separately
  from any full-payload cryptographic validation claim.
- [ ] Run authenticated local NVIDIA playback/captures: all four cameras across
  53,990–54,010, paused non-keyframe seeks such as 54,010, reverse seeks, reopen,
  and camera 93's unequal late boundary at 2,592,030.
- [x] Keep the required June GoodCopBadCop 0:300 smoke and a May legacy-overlay
  control. Do not operate on the user's running Crimson window.
- [ ] Measure warm/random-seek behavior, continuous playback, repeated seek/
  source changes and an endurance interval. Report storage/cache conditions,
  current-frame delays, stale counts, peak RSS/VRAM and return-to-baseline behavior.

Exit: independent identity/geometry evidence plus visible rendered layers,
not just a successful archive open or a timeline-ready log.

## 6. Delivery checkpoints

Implement as small reviewable commits, in dependency order:

1. Exact source contracts and negative fixtures.
2. Observation identity transport, August repository branches and joins.
3. Asynchronous bounded overlay session and lifecycle.
4. Canonical scene/control integration and truthful diagnostics.
5. Real-data/GUI qualification and a fresh Ubuntu 22 package.

Reader tasks may proceed in parallel once shared identity and selection APIs are
frozen. Session/storage and UI work should use agreed fixtures and interfaces;
one integrator owns `red.cpp` and shared adapter changes.

- [ ] Rebuild with the pinned Ubuntu 22/CUDA 12.4/TRT10 builder; verify all bundled
  ELF baseline requirements, native sm_86 code, closure and package metadata.
- [ ] Use the packaged `bin/crimson` launcher with strict runtime selection for
  GUI qualification. Preserve the known-good package for rollback.
- [ ] Account for CMake's shared source-root `release/` output despite separate
  build directories: preserve/restore native artifacts and never repackage a
  restored native executable with `--skip-build`.
- [ ] Run the target-machine check on Ginny's actual A4000/driver 535. Local
  A6000 success is not target validation.
- [ ] Publish a newly versioned archive/checksum and update `START_HERE.md` only
  after acceptance and authorization. Do not overwrite the current share during
  implementation or change recording permissions/selectors.

## Audit basis and open decisions

Two parallel Luna xhigh read-only audits covered source contracts and Linux UI
integration; the supervising audit covered git state, storage and acceptance.
No application changes, data writes or remote desktop actions were performed.

Before implementation, verify cross-product key/frame joins and upstream
lineage, qualify the exact August keypoint and strict-mask readers at runtime,
check sampled-contour-cache availability, and select numerical memory/latency
budgets. Stored raw keys and the mask envelope family are established by this
audit; consumer wiring and observed joins remain to be proven. These are
explicit first milestone gates, not reasons to regenerate data or bypass
schema validation.
