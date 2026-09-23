# Crimson Phase 4E Composite Stimulus Presentation

Date: 2026-07-12

Phase 4E connects the Phase 4D camera-aligned stimulus decoder to the native
macOS Metal application. The camera and stimulus are encoded into distinct
viewports in one drawable, render encoder, and command buffer. There is still
one `LogicalPlaybackClock`, owned by the main camera transport.

## Presentation Contract

The camera frame actually selected for presentation is authoritative. It may
trail the logical request while network decode catches up, so the stimulus is
never resolved from the clock target alone.

For every committed mapped composite:

```text
displayed stimulus frame
  == repository.resolveCameraFrame(displayed camera frame).stimulus_frame
  == decoded stimulus frame
```

`StimulusPresentationCoordinator` gives this rule a portable state machine and
generation identity:

- `Present`: commit a newly decoded exact pair;
- `Hold`: commit a new camera frame only when it maps to the stimulus surface
  already visible;
- `Clear`: commit the camera with an empty stimulus viewport for an explicit
  missing or out-of-range mapping; and
- `Wait`: defer the entire candidate composite when its mapped stimulus frame
  is not exact yet or any identity is inconsistent.

The initial implementation cleared the stimulus viewport on `Wait`. Production
playback exposed that as visible flashing. The corrected policy retains the
previous complete matched pair until the next pair is ready. It may briefly
repeat a camera frame under network latency, but it never flashes black for a
mapped frame and never pairs a newer camera frame with stale stimulus content.

## macOS Integration

The native app accepts an analysis archive and optional stimulus run alongside
the main camera video:

```bash
build/macos-arm64-release/Crimson.app/Contents/MacOS/Crimson \
  --video /path/to/camera.mp4 \
  --zarr /path/to/analysis.zarr \
  [--stimulus-run RUN]
```

The app opens `ArchiveContext`, `StimulusRepository`, and
`AppleStimulusPlaybackSession` once at startup. Session control remains on the
GUI thread; only the bounded playback buffers decode in the background.

Pause, slider settlement, and frame steps propagate an explicit discontinuity
to both streams. Normal progress stays target-driven and never enables an
autonomous stimulus clock. Reaching the camera end pauses the logical clock and
settles the final exact pair.

The composite layout reserves a right-side stimulus viewport above the shared
transport. One `AppleVideoMetalRenderer` encodes the camera and stimulus
`CVPixelBuffer` surfaces into the same command buffer, which also retains both
surfaces until GPU completion.

## Metrics

The production smoke records:

- requested and presented camera identities;
- mapped, decoded, and presented stimulus identities;
- composite presentation generation;
- exact, repeated-map hold, interpolated, deferred, missing, and out-of-range
  counts;
- mapping and decoded-identity mismatches;
- current and maximum camera-frame mapping skew;
- decoder seek/follow/hold counts and peak buffer depth; and
- camera repeats, source skips, lag, memory, and thermal state.

Only committed pairs contribute presentation skew. A correct mapped pair has
zero camera-frame skew because the repository resolution carries the same
camera identity as the camera surface submitted beside it. Deferred candidates
are reported separately instead of being hidden as mismatched presentation.

## Automated Coverage

`stimulus_presentation_coordinator_tests` is portable C++ coverage for exact
presentation, repeated mappings, paused holds, deferred unavailable frames,
mapping-generation mismatch, decoded-identity mismatch, missing mappings,
backward steps, and out-of-range clearing.

`apple_stimulus_composite_metal_tests` uses independent camera and stimulus
decoders on deterministic media. It renders both surfaces into nonoverlapping
Metal viewports in one command buffer and checks camera pixels, stimulus pixels,
the untouched gutter, missing-map clearing, repeated-map hold, forward and
backward exact seeks, the final frame, bounded buffers, and zero skew.

The existing logical-clock, repository, decoder, color-conversion, surface
lifetime, and shell tests remain part of the same CTest preset. The native
macOS suite passed 11/11.

## Production Validation

The production smoke is:

```bash
scripts/macos_gui_smoke_stimulus.sh [VIDEO] [ANALYSIS_ZARR] [START:END]
```

It requires both `[AppleVideoSmoke] PASS` and `[AppleStimulusSmoke] PASS`.
The end camera frame must be mapped; an explicit negative run ending at missing
camera frame `1023` produced `[AppleStimulusSmoke] FAIL` rather than accepting
an empty stimulus viewport as parity.

GoodCopBadCop camera range `1024:1324` passed directly from the network mount:

- final camera `1324` mapped to, decoded, and presented stimulus `360`;
- zero mapping mismatches and zero maximum camera-frame skew;
- two stimulus seeks, six-frame peak stimulus buffer depth; and
- 64 deferred candidates with a maximum run of six refreshes, presented as
  retained matched pairs rather than black flashes.

Mid-recording range `70000:70300` also passed:

- final camera `70300` mapped to, decoded, and presented stimulus `83125`;
- zero mapping mismatches and zero maximum camera-frame skew;
- two stimulus seeks, six-frame peak stimulus buffer depth; and
- 62 deferred candidates with a maximum run of four refreshes.

The maintained NVIDIA configuration rebuilt with CUDA 12.4, TensorRT 10.0.1.6,
OpenCV 4.10, NVIDIA FFmpeg, and the prebuilt TensorStore stack. Its three
portable CTests passed, `redgui` linked, and the authenticated `0:300` GUI
playback smoke passed. The known OpenCV/FFmpeg version-family linker warnings
remain unchanged.

## Phase Boundary

Phase 4E completes camera-aligned stimulus decode and presentation on macOS.
The next Phase 4 work should bring acquisition crop streams under the same
logical-camera and atomic-presentation contract, then validate multistream
handoff boundaries before broader overlay/UI parity work.
