# Linux-first main integration — 2026-09-22

Status: local integration checkpoint with Linux validation passed; not merged to
main or published. Native macOS/Windows validation remains deferred.

## Scope and preserved baseline

- Integration branch: `codex/main-integration-20260922` in a new, separate worktree.
- Main parent: `276dea2fa851d315035444c1b3e2792943ed880b` (remote verified at start).
- Linux parent: `5fc71d6b3ca6130deaf7d1609cf28f52a52af62c`.
- Both histories are preserved by a normal merge, without rebasing/resetting main.
- Linux is the acceptance priority. Native macOS and Windows build/runtime tests
  are explicitly deferred by the user, **not passed**. This is not full August
  canonical-overlay adoption by the Apple UI.
- The existing Linux worktree and Ginny's Sept 21 package are unchanged. No data,
  permissions, recording links, public release files or other worktrees changed.
- [Full implementation checklist](main_integration_implementation_checklist_2026-09-21.md).

## Conflict decisions

The ten textual conflicts were resolved with a semantic review of auto-merges:

- Decoder and software stimulus preserve the Linux parent/local frame mapping,
  seek bookkeeping, frame-slot leases, color-range metadata and conversions.
  Main's worker exception boundaries and error reporting wrap that implementation;
  the older direct slot writes are not restored. Image loading is also protected.
- Main's already auto-merged YOLO failure reporting remains in the same shared
  error map. `red.cpp` owns the map/mutex exactly once. Diagnostics displays a
  sorted error snapshot and allows clearing it. Clearing a report does not restart
  a failed worker; reopening clears that worker's previous error.
  Runtime diagnostics remain available with Frame Inspect hidden or no recording.
- Main's runtime VSync toggle is retained in Diagnostics, with the actual GLFW
  swap interval changed by the NVIDIA application owner.
- Deliberate failure-semantics improvement: failure to open the software stimulus
  decoder now enters the worker error channel, disables demand and invalidates
  progress, instead of only printing and returning. Normal decode/seek ownership
  is unchanged.
- Main's read-only installed-app update check is extracted into
  `app_update_status.*` and displayed in Diagnostics. Metadata is size-bounded and
  must be a JSON object. Incomplete/unavailable metadata is not labeled up to date.
  Different release names indicate a different published drop, not proven version
  ordering. No installer or update is run automatically.
- Linux presentation texture fields **and allocations** are retained. Git's
  automatic merge removed NV12 allocations needed by the newer renderer; those
  were restored. Main's previous renderer experiments are not individually
  resurrected; the tested Linux renderer is the integration baseline. Windows
  performance/renderer acceptance remains a native follow-up.
- Linux keeps explicit runtime closure and executable-relative RUNPATH, without
  main's generic dependency installation. Windows keeps runtime pre/postflight,
  search roots, device helper, shortcuts and installed/published metadata. The
  system-path exclusion regex is tested for both slash styles. Required
  `VCRUNTIME140_1.dll` is no longer silently ignored if unresolved. Duplicate
  `Dbghelp` linking from the auto-merge was removed; existing crash setup remains.
- Decoder telemetry remains `decode_ms`; the CSV column remains
  `camera_decode_decode_ms`. Main's profiler tools accept this as the historical
  `camera_decode_submit_ms` report field, while retaining compatibility with old
  logs. Packet/sample counters remain intact.
- Main-only runtime commits were checked against their final revert state. The
  already-reverted proxy metadata, software camera decode, custom playback widget
  and render-scale experiments are not restored. `CMakePresets.json` and Windows
  dependency-root scripts retain the branch's supported layouts. The new portable
  update-status test is included in the explicit macOS build target list; an
  all-target native build remains the recommended integration validation path.

## Validation record

- Fresh Ubuntu 22 / CUDA 12.4 build: passed in `build/linux-integration-ubuntu22`.
  Pinned builder SHA-256:
  `d14bb0302ad24fa33b8e2c8713323e681fcd52a900cad7f6e66330ecb5b4c62a`.
- Full CTest: **105/105 passed**, 7.11 seconds in the pinned builder (baseline 101).
  Four added tests cover installed-app metadata status, actual CPU image-worker
  failure/lease unwind/reopen, Diagnostics interaction without Frame Inspect,
  and the Windows system-path regex contract (not a native Windows run).
- Staged runtime: **19 OK / 0 WARN / 0 FAIL**, authenticated X display, RTX A6000,
  driver 580.173.02. All **177 ELF files** meet GLIBC <= 2.35, GLIBCXX <= 3.4.30,
  CXXABI <= 1.3.13. No host driver, libc or libstdc++ is bundled. `sm_80` and
  `sm_86` cubins are present. This does not replace a run on Ginny's A4000/535 host.
- June GoodCopBadCop control: passed 0:300 on the packaged launcher, including a
  repeat after final Diagnostics wiring (presented frame 300; 3.00165 seconds).
- August paused overlays: all four cameras 2010093/94/95/96 passed at parent frame
  54010 with matching keypoint/mask/shape instance keys, exact presented frame,
  expected primitive counts and a captured image. Camera 93 capture was visually
  inspected: video, colored overlays, eye angles, speed and bout traces are present.
- August advancing masks, camera 2010093:

  | Range | Render cap | First-draw masks / presented frames | Unpresented video frames |
  | --- | --- | --- | --- |
  | 1677990:1678320 | 30 Hz | 330 / 330 | 1 |
  | 1677990:1678320 | 60 Hz | 331 / 331 | 0 |
  | 53990:54320, clip boundary | 60 Hz | 329 / 329 | 2 |

  All checks use the existing eight-second prewarm and zero excluded grace frames;
  zero identity/coverage errors. Peak decoded mask-payload cache is 5,905,630 bytes
  (< 96 MiB); this is **not** a total-process/mapping-memory limit. The boundary
  case was repeated after final Diagnostics wiring with the same coverage counts.
  Unpresented video frames are clock skips, reported separately from missing masks.
- Profiler compatibility: both analyzer and plot reader passed current-column,
  historical-column and explicit-old-column precedence fixtures.
- Integration edits pass whitespace checks against the Linux parent. A full diff
  against main retains pre-existing whitespace warnings in imported generated SVGs
  and historical capture files; those artifacts were not reformatted.
- Native macOS/Windows: deferred, no local native runner used.
- Main merge/push, new public package, worktree deletion: not performed.

Raw evidence is retained locally under `build/integration-evidence-20260922/`
(ignored generated artifacts): full build/test/runtime/ABI logs, four paused
capture directories and four advancing-mask runs. Ginny's existing package
checksum was reverified as
`9168a650e1f056553ed5c1aafa6e01ec8ecc2b505981bd5b1e92b5fd481f1d19`;
her existing recording symlink is unchanged. Remote main was rechecked and remains
the pinned main parent above.

The staged integration app is `dist/Crimson-linux-integration-20260922/bin/crimson`.
Its `release.json` is refreshed from the clean local merge commit after the
checkpoint; the tested sources do not change during that metadata refresh.

## Remaining review / follow-up

- Manual Linux UI stress (rapid backward/superseding seek sequences and full GUI
  close/reopen after an injected GPU/stimulus/YOLO failure) was not newly performed.
  The suite covers seek supersession, camera layout/slider identity, slot lifetime,
  worker unwind and reopen; four real paused clip seeks and boundary playback passed.
- No OS cache flush was performed. Paused overlay runs start fresh app sessions;
  these are not a storage-cold latency qualification. Playback checks prewarm.
- Native Windows DLL closure/installer/crash checks and native Apple build/Metal
  checks remain outstanding. Full canonical August UI adoption on Apple is a
  separate follow-up, not an effect promised by this merge.
- Review and explicitly approve publication/main merge; refresh main and rerun
  affected gates if it moves. Do not remove the validated Linux worktree or its
  ignored builds/evidence merely because the tracked source is clean.
