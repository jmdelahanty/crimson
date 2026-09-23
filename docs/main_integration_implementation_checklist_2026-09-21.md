# Main integration and Linux worktree retirement checklist

Plan date: 2026-09-21. Integration started: 2026-09-22.

Current implementation/evidence: [Linux-first integration record](main_integration_validation_2026-09-22.md).
The original main and tested Linux checkouts and Ginny's published package remain
unchanged. Work is isolated in `codex/main-integration-20260922`.

The user explicitly prioritizes Linux and defers native macOS/Windows testing.
Those checks remain open follow-up gates, not claims of cross-platform acceptance
and not automatic blockers to a later explicitly approved Linux-first main merge.
No main merge, remote publication or worktree retirement is authorized by starting
this integration alone.

## Objective and boundaries

Integrate the validated Linux/shared-backend history into current GitHub `main`
without losing Windows behavior or breaking the Apple Silicon backend. Retire
the Linux-priority worktree only after the merged result and its preserved
artifacts have been verified.

Do not treat this as cherry-picking only the six September Linux commits:
their prerequisite shared/macOS architecture is not all in `main`. Do not reset
local `main`, take an entire conflicted side wholesale, rewrite published
history, or remove unrelated worktrees. No recording/data/permission changes.

## Pinned starting evidence

| Item | Audited state |
| --- | --- |
| GitHub `main` / local `origin/main` | `276dea2fa851d315035444c1b3e2792943ed880b` |
| Linux branch | `codex/linux-priority-20260916`, `5fc71d6b3ca6130deaf7d1609cf28f52a52af62c` |
| Merge base | `40f64ad3cc219e7cf87aa9acd45d0f27765a13a1` |
| GitHub comparison | Linux branch 323 ahead / 116 behind, including merge history |
| Non-merge commits on respective sides | 322 Linux-branch-only / 48 main-only |
| Merge preview | 10 conflicted paths; preview only, refs/worktrees unchanged |
| Local `main` | 14 ahead / 174 behind audited `origin/main`; preserve it |
| Validated Linux baseline | 101/101 tests; runtime 19 OK / 0 warnings / 0 failures; 177-ELF Ubuntu 22 ABI audit |

Recheck remote tips before implementation; these hashes are evidence, not a
license to overwrite newer work. No open PR for the Linux branch was found at
the audit checkpoint. The merge simulation used a temporary Git object directory,
not the real index or worktree; its report is
`/tmp/crimson-main-merge-preview-20260921.txt`.

## Phase 0 — Preserve and isolate

- [x] Inventory tracked, untracked, ignored, and submodule state in each affected
  checkout; carry/commit this checklist deliberately before worktree retirement.
- [x] Preserve local `main`'s distinct history. Base integration on refreshed
  **GitHub `main`**, not the stale local `main` branch.
- [x] Leave `/home/delahantyj@hhmi.org/gitrepos/crimson` untouched: it is on the
  Windows branch and has unrelated modified CMake/decoder/renderer/Zarr files
  plus untracked `palette_clipped_resolver` sources.
- [x] Leave `crimson-mac-plan` and `crimson-ui-monolith` untouched. The former
  has an untracked planning document; the latter is the archived July worktree.
- [x] Create a new integration branch/worktree from the verified remote tip.
  Created `codex/main-integration-20260922` and sibling directory
  `crimson-main-integration-20260922` after verifying neither existed.
- [x] Initialize its pinned submodules and use fresh build directories. Do not
  copy CMake caches with absolute paths from the Linux-priority worktree.
- [ ] Merge the complete Linux branch into that integration branch without
  force-pushing or dropping ancestry; record both parent hashes and resolve all
  conflicts before making the merge commit. Keep native tests and follow-up
  repairs reviewable; do not merge the integration branch into `main` yet.

Exit: existing work remains intact; only the new integration worktree is used
for reconciliation and builds.

## Phase 1 — Resolve conflicts semantically

The audited conflict set is exhaustive for the pinned tips, not a claim that
auto-merged files are safe.

| Conflicted path | Required reconciliation |
| --- | --- |
| `CMakeLists.txt` | Keep backend gates/shared targets, Linux install/closure policy, and Windows packaging/crash-support requirements; see build items below. |
| `src/decoder.cpp` | Preserve exact seek-frame bookkeeping and cancellation/settlement semantics alongside worker failure propagation and cleanup. |
| `src/global.h` | Retain branch telemetry fields, reconcile main's `decode_submit_ms` with branch `decode_ms`, and add main's worker-error map/mutex declarations with exactly one owner. |
| `src/red.cpp` | Keep modular session/transport/canonical overlay/timeline paths, stable camera identity, read-ahead, and error/update UI wiring. Do not restore an older monolithic loop wholesale. |
| `src/render.h` | Reconcile renderer selection and buffer/resource lifetime with worker error handling; retain the tested Linux presentation path without undoing intentional Windows reverts. |
| `src/stimulus_playback.cpp` | Preserve aligned/shared stimulus behavior and software-decode failure reporting, stop/unblock/join, and seek settlement. |
| `tools/install_crimson.ps1` | Preserve run-only layout, replacement safeguards, shortcut/update metadata behavior, and branch runtime pre/postflight checks. |
| `tools/publish_windows_app_drop.ps1` | Preserve versioned releases/current/latest manifests, layout validation, correct interpolated labels, and safe destination handling. Test in a temporary share, never the live one. |
| `tools/windows_app_README.txt` | Describe the actually merged installer/runtime/update options and shortcut behavior, not either old script independently. |
| `docs/crimson_windows_first_validation_guide.md` | Reconcile supported stack, setup/build/install commands and troubleshooting with the merged implementation. |

- [x] Review main-side feature/revert pairs by their **final behavior**, especially
  direct NV12 presentation, custom camera widgets, render scaling, software
  camera decoding and proxy metadata. Do not resurrect a reverted experiment
  merely because its original commit appears in the history.
- [x] Trace worker errors through all owners/call sites, including YOLO and
  media-session loading that may auto-merge. Preserve Windows crash-dump setup
  and linking; Linux already contains some of these features in evolved form.
- [x] Add focused regression tests for the reconciled runtime decisions before
  relying on a build or GUI launch as proof of compatibility.
- [x] Inspect `CMakePresets.json`, `tools/set_windows_dependency_roots.ps1`,
  update/crash helpers, and playback planning documents even when Git reports
  a clean textual merge.
- [x] Confirm no conflict markers, duplicate registrations/symbols, dead launch
  options or lost error paths; run `git diff --check` and review staged changes.
  Integration delta is whitespace-clean against the Linux parent; imported
  historical/generated whitespace is retained and documented in the record.

### Build and distribution reconciliation

- [x] Keep `project(... LANGUAGES CXX)` and conditional NVIDIA/CUDA enablement.
  macOS configuration must not discover/link CUDA, TensorRT, NVDEC or OpenGL.
- [ ] Preserve shared repository/runtime/frame targets outside backend-only
  sections, with explicit dependency ownership rather than accidental header
  include paths. Preserve the Ubuntu 22 builder lock and ABI baseline.
- [ ] Reconcile Windows runtime search roots: CUDA bin directories, TensorRT,
  FFmpeg, vcpkg and OpenCV root/ancestor layouts. Test configured paths with spaces.
- [x] Test the generated Windows dependency-scanner regex with both slash
  styles; the CMake conflict has different escaping depths.
- [x] Audit the Windows unresolved-DLL exception list rather than unioning it
  blindly. In particular, establish that `VCRUNTIME140_1.dll` is available or
  supplied as required; do not hide a missing required runtime behind an ignore.
- [ ] Retain Windows `bin/redgui.exe` layout, system-runtime handling, runtime
  checker and CUDA-device helper installation. Validate `Dbghelp`/crash support.
- [ ] Preserve Linux's executable-relative RUNPATH and explicit bundled closure;
  do not reintroduce generic dependency installation that bundles host drivers or
  libc. Keep packaged launcher/runtime validation as the acceptance surface.
- [ ] Keep JSON dependencies explicit for canonical presentation tests.
- [ ] Inventory tests for each backend. The new camera-window identity test is
  currently inside the NVIDIA CMake section; do not claim it runs on macOS.
  Move/generalize its target when adopting that helper on Apple if appropriate.
- [ ] Ensure native validation builds every test selected by CTest. The macOS
  build preset has an explicit target list that can omit newly added tests;
  update that list or use an all-target build, and fail on missing executables.

Exit: a reviewable merged tree preserving intended behavior from both sides,
with conflict resolutions and any deliberate deviations documented.

### Runtime reconciliation order and acceptance details

The Luna xhigh runtime audit identified the following concrete dependencies.
These are more specific than simply selecting a side of each conflict.

1. [x] Preserve branch `frame_types.h`, `frame_slot.*`,
   `decoder_seek_bookkeeping.*` and demuxer color-range/timebase APIs first.
   They underpin the extracted presenter, decoder and stimulus paths.
2. [x] Reconcile the global declarations and definitions next. Keep
   `packet_total_ms`, `demux_success`, `sample_sequence` and the other branch
   `DecoderPerfSample` fields used by `perf_logging.cpp`. Choose and test one
   meaning/name for decode timing (or an intentional compatibility alias), and
   add `g_decoder_error_messages` / `g_decoder_error_mutex` exactly once.
3. [x] Use the branch decoder/stimulus implementations as the semantic base
   for frame ownership; port main's error clearing/reporting, exception
   boundaries and exception-safe CUDA cleanup around them. **Do not restore
   direct writes to slot availability/frame-number fields** from older main:
   they bypass the frame-slot lease protocol. Include the image-loader path.
4. [x] Keep main's YOLO failure wrappers from the auto-merge and wire their
   dependencies to the same error channel. Decoder failure must disable that
   worker's demand and invalidate its latest-frame progress; all worker types
   must unwind without hanging a subsequent stop/reopen.
5. [x] Retain branch `CameraResources` presentation/NV12 fields, parent/local/PTS
   identity and reset/resize helpers. `camera_view_presenter`, camera-window code
   and NVIDIA diagnostics consume them; main's simpler struct is not a viable
   replacement. Preserve color range both in pixel conversion and metadata.
6. [x] Restore main's sorted, mutex-protected error snapshot/clear UI and
   Windows install/update status in an explicitly chosen Help/Diagnostics
   surface. The old monolithic UI location no longer exists. The current
   `drawDiagnosticsWindow` call is a candidate; do not port the old monolithic
   `PerfLogWriter` into `red.cpp` to recover unrelated UI.

- [x] Preserve Windows crash-handler setup already present in the branch:
  `windows_crash_dump.cpp/.h` are identical on both sides. This is not missing
  functionality that needs a second implementation.
- [x] Decide explicitly whether software-stimulus failed-open should remain
  main's log-and-return behavior or enter the error channel; do not silently
  change failure semantics while claiming a mechanical merge.
- [ ] Add/re-run regressions proving distinct local/parent frame identity,
  superseded seeks cannot settle a newer seek ID, slot reset/write safety while
  read leases exist, NV12 color range, and failure/clear/reopen lifecycle.
  Start with `frame_slot_tests`, `decoder_seek_bookkeeping_tests`,
  `playback_buffer_browser_tests`, `playback_diagnostics_tests`, stimulus
  alignment/repository tests, and canonical overlay tests before full GUI gates.

## Phase 2 — Validate the integrated revision

Use logs/captures stamped with the **integration revision**, toolchain and
fixture. The existing Linux package is the regression baseline, not proof that
the merged revision passes. A missing platform run is not a passing check.

### Linux / NVIDIA — available on this workstation

- [x] Build with `tools/build_linux_release_in_apptainer.sh` in fresh integration
  build/stage directories using the pinned Ubuntu 22 / CUDA 12.4 builder.
- [x] Run the complete CTest suite in that builder, not just selected overlay
  tests. Baseline is 101 tests; investigate unexplained dropped/skipped tests.
- [ ] Recheck runtime closure, GL/NVIDIA access, GLIBC/GLIBCXX/CXXABI ceilings,
  native `sm_86`, clean commit metadata, and absence of bundled host drivers.
- [x] Run the AGENTS.md June GoodCopBadCop 0:300 playback control from the repo
  root using an authenticated display and the packaged launcher. Isolate test
  configuration; do not drive or close a user-owned window.
- [x] Run `scripts/gui_smoke_canonical_overlays.sh` at parent frame 54010 on
  each of the four August cameras, checking bound keys and exact frame identity.
- [x] Run `scripts/gui_smoke_canonical_mask_playback.sh` on camera 2010093 at
  1677990:1678320 with 30 and 60 Hz rendering, and 53990:54320 across a clip
  boundary. Require first-draw mask coverage, zero identity mismatches and cache
  bounds. Report skipped video frames separately from missing masks.
- [ ] Repeat paused seeks within/across clips, including 54010, backward and
  rapid superseded seeks, reopen, and move/resize/drag/release the camera window
  across a clip change. Verify camera/stimulus/overlay parent-frame alignment.
- [ ] Exercise preserved worker-error paths with bounded failure fixtures and
  check that closing/reopening does not hang or retain stale error state.
- [ ] Record the eight-second prewarm in automated mask checks. Add a cold-seek
  check without claiming filesystem-cache flushing or inference completeness.

### macOS / Apple Silicon — native host required

- [ ] Build the integrated checkout natively: `cmake --preset macos-arm64-release`,
  then an all-target build of `build/macos-arm64-release` (or an updated preset
  that includes every selected test).
- [ ] Run `ctest --preset test-macos-arm64-headless` and
  `ctest --preset test-macos-arm64`; record unavailable fixture/display tests
  explicitly. This Linux host cannot qualify Metal or native Apple frameworks.
- [ ] Exercise existing Apple video, stimulus/crop multistream, repository and
  read-only-overlay behavior with maintained fixtures. Reuse the
  `macos_gui_smoke_playback.sh` and `macos_gui_smoke_multistream.sh` scripts with
  native mount paths; do not substitute NVIDIA trace output for Metal evidence.
- [ ] Check rolling-clip index acceptance and parent/local mapping on Apple;
  its playback buffer uses the shared `RecordingClipIndex::Open` implementation.
- [ ] Validate the currently used compatibility buffer API: Apple opens masks
  with lookahead 12/cache 24, not the new adaptive/96 MiB policy. Preserve this
  behavior during the merge unless Phase 3 is explicitly included and tested.

### Windows / NVIDIA — native host required

- [ ] Configure the supported Windows preset and the no-SFM variant; build
  Release and run CTest with the appropriate `-C Release`. Compile-check Debug
  where the maintained workflow requires it.
- [ ] Stage a fresh package; inspect resolved DLL closure and run its checker
  from a clean launch environment, not only the developer PATH.
- [ ] In temporary install/share directories, test fresh install, explicit
  replacement, same/nested source-target rejection, paths with spaces,
  shortcuts, release/install/update metadata, and launch working directory.
- [ ] Test versioned publish and current/latest refresh while preserving an
  earlier release. Do not invoke destructive publish modes on production data.
- [ ] Verify camera/stimulus/YOLO worker errors are surfaced and cleaned up;
  validate crash-dump wiring using the maintained controlled test procedure.
- [ ] Record lack of a native runner as an open gate. Ask for an explicit
  release-risk decision if a platform gate cannot be completed; do not silently
  describe Linux-only validation as cross-platform acceptance.

Exit: Linux results attached to the integration review with native macOS/Windows
validation explicitly deferred per the user's 2026-09-22 scope decision. Review
remaining Linux gaps before requesting the final main merge. Deferred native
platform acceptance must not be described as passing.

## Phase 3 — Separate cross-backend adoption follow-up

This is not necessary merely to resolve Git conflicts. It closes the feature
adoption gap identified in the preceding discussion and should be a separate
reviewable change unless deliberately added to the integration scope.

- [ ] Expose canonical bound-source selection/repository/session/presentation
  components through an explicit backend-neutral target where appropriate.
  Do not create an Apple-only copy of August schema or observation-join logic.
- [ ] Connect the Apple analysis loader to the exact canonical source bindings,
  retaining independent missing/failed product reporting and current legacy/v2
  workflows. Read maintained streams, not exports; preserve source coordinates
  and explicit frame/instance identities without ordinal joins.
- [ ] Extend the existing `SubjectMaskPresentationCoordinator` input to pass
  optional playback direction, source FPS/rate, paused demand and discontinuity.
  Opt the Apple caller into `SubjectMaskOverlayBufferPolicy` and explicit demand;
  retain compatibility semantics for callers not migrated.
- [ ] Preserve the layering boundary: shared policy/data/scene adapters; Metal
  and OpenGL/CUDA retain texture uploads, decoding and GPU resource lifetimes.
- [ ] Make first-draw coverage checks consume equivalent Metal presentation
  evidence, and repeat delayed-reader, seek/reversal, scheduler eviction,
  invalid/missing mask, identity and byte-budget tests on native macOS.
- [ ] Do not promise that merging alone enables the full August UI on macOS.
  Do not fold deferred contour outlines, bout shading, editing/export or new
  inference work into this integration.

## Phase 4 — Merge and retire only the intended worktree

- [ ] Review an integration PR against refreshed GitHub `main`; preserve both
  histories and verify the final merge contains the Linux tip and current main.
  Re-run affected gates if main moved or resolutions changed.
- [ ] Push/merge only after approval and verify the resulting remote hash.
  Preserve the release branch/ref identifying Ginny's validated source revision.
- [ ] Preserve external release artifacts and checksums. Current known-good
  archive is `/misc/public/forGinny/Crimson-linux-x86_64-colleague-20260921T220445Z.tar.gz`,
  SHA-256 `9168a650e1f056553ed5c1aafa6e01ec8ecc2b505981bd5b1e92b5fd481f1d19`.
  Its source is `5fc71d6`; do not relabel it as a build of the future merge.
- [ ] Preserve needed raw validation logs/captures outside the retiring worktree
  and `/tmp`. The public validation note is a summary, not all raw evidence.
- [ ] Inventory approximately 15 GiB `build/`, 7.8 GiB `dist/`, 489 MiB
  `release/`, ignored evidence logs, `imgui.ini` and other local state. Decide
  what to retain before deletion; a clean tracked tree does not protect these.
- [ ] Reconfigure a replacement development/build checkout and update any
  launch shortcuts/notes using the old absolute path. Verify no app, build,
  agent or shell depends on the old worktree before removing it.
- [ ] Remove **only**
  `/home/delahantyj@hhmi.org/gitrepos/crimson-linux-priority-20260916` through Git's
  worktree management after the preservation checks and explicit cleanup go-ahead.
  If Git refuses because of submodules/local files, inspect the reason; do not
  escalate automatically to forced or broad recursive deletion.
- [ ] Recheck `git worktree list`, surviving refs/checkouts, public package
  checksum and the unchanged August recording symlink. Report what was removed,
  what was preserved, and which generated files would need rebuilding.

## Ownership and stop conditions

One integrator owns the integration branch and final CMake/red.cpp assembly.
Bounded parallel implementation tasks can cover runtime error/seek resolution,
build/package reconciliation, and independent platform validation. Do not let
multiple agents edit the same conflicted file without an explicit handoff.

Stop for user direction if reconciliation would discard a supported workflow,
require a target driver upgrade, broaden data access/mutations, change the agreed native-platform deferral into a passing claim, or delete
unarchived work. The integration-starting turn authorizes none of those actions.
