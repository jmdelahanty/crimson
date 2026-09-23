# August Linux mask playback read-ahead

Validated integration on `codex/linux-priority-20260916`, based on `b9da51b`.
This change is separate from the accompanying camera-window identity fix.
No recording data or published colleague package is modified.

## Why the August masks flickered

The initial canonical overlay integration deliberately used a four-frame mask
cache with no speculative reads. During drawing it requested the displayed
frame and immediately took a nonblocking snapshot. A newly requested frame
therefore commonly had no mask yet, even when the source data and mapping were
valid. At 60 Hz a later redraw could catch the result; at 30 Hz the next draw
could already present another video frame.

The previous Linux legacy route preloaded upcoming masks. Sharing its priority
scheduler did not automatically carry that read-ahead policy into the canonical
session. The existing 60 FPS canonical overlay smoke checked a settled, paused
frame; it did not prove continuous masks on advancing video.

The selected camera 93 dense mask array uses outer shards
`[2872,1,384,384]` containing indexed, separately compressed inner chunks
`[8,1,384,384]`. Eight observation rows are not universally eight video frames.
The outer logical payload is 403.875 MiB per component; the inner read chunk is
1.125 MiB uncompressed. These are not compressed file sizes or physical-I/O
measurements. There is no storage rewrite or video re-encoding in this fix.

## Integration and invariants

- Canonical mask requests explicitly supply playback direction, source FPS and
  playback rate. Paused demand also seeds a bounded future window for Play.
- Reads and mask decoding run through the existing shared scheduler, preserving
  current-frame priority. The repository's separate private prefetch remains
  disabled; decoded-frame demand reuses its existing inner-chunk cache.
- The buffer chooses bounded read-ahead using storage-row density, playback rate
  and observed resolution latency. Retained decoded-frame bytes have an explicit
  budget, in addition to frame-count limits and the repository's payload budget.
- Source and seek discontinuities invalidate obsolete work. Presentation still
  requires the exact displayed frame and validated bound source/observation keys.
  No old-frame mask is substituted to conceal a late read.
- Cache limits apply to retained buffer payloads, not total process RSS: active
  reads, caller-held snapshots, source mappings, TensorStore and GPU resources
  have separate ownership and bounds.

## Diagnostics and acceptance

`--playback-trace-log PATH` now emits `canonical_overlay_present` for each camera
draw, including playing and paused draws. It records the query/presented/mask
frames, mask state/error/outcome, expected and actual fill counts, texture costs,
decoded cache bytes/budget/lookahead/coverage, and repository payload counters.

Outcomes distinguish drawn masks, valid absent components, pending/opening data,
unavailable/failed products, rejected scenes, disabled masks and drawing failures.
These counters do not replace an independent source-data oracle: expected fill
counts come from the validated repository/presentation path.

`--playback-smoke-warmup-seconds N` optionally leaves the smoke paused while
loading/read-ahead settles, then starts ordinary playback. Default zero preserves
existing smoke behavior; the total smoke timeout includes warmup.

Run advancing mask coverage checks with a current authenticated display:

```bash
CRIMSON_REDGUI=/path/to/test-package/bin/crimson \
CRIMSON_PLAYBACK_SMOKE_DISPLAY=:1 \
CRIMSON_PLAYBACK_SMOKE_XAUTHORITY=/path/to/current/Xauthority \
  bash scripts/gui_smoke_canonical_mask_playback.sh /path/to/analysis.zarr \
  1677990:1678320 60
```

Repeat with render FPS `30`, and with a range crossing a clip boundary such as
`53990:54320`. The helper uses eight seconds of paused warmup, then checks all
draws during advancement, including the first draw of each displayed frame.
No grace period is excluded by default. It requires positive mask drawing,
exact frame identity, consistent counts and cache bounds. Genuine absent
components are allowed. Video frames skipped by the playback clock are reported
separately rather than counted as missing mask draws. No desktop input is sent;
the temporary process and its configuration are isolated.

Headless coverage-checker tests run in CTest and directly with:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 tools/check_canonical_mask_playback_tests.py
```

## Verification checkpoint

The Ubuntu 22 / CUDA 12.4 container build succeeded. The complete CTest suite
passed (101/101), including launch-option parsing and the coverage checker.
Delayed-reader tests cover 30 FPS video under both 30 Hz and 60 Hz rendering,
2 ms reads with 40 ms outliers, paused prewarm, chunk boundaries, exact identity,
seek/reversal, missing and failed results, byte limits, and scheduler eviction
recovery. The installed runtime audit passed with 19 OK, no warnings or failures.

The required June control passed frames 0:300 on the local RTX A6000 using the
installed candidate. Build/test evidence:

- `/tmp/crimson-mask-prefetch-build-verified.log`
- `/tmp/crimson-mask-prefetch-ctest.log`
- `/tmp/crimson-mask-prefetch-runtime-final.log`
- `/tmp/crimson-mask-prefetch-june-control.log`

Final-candidate advancing camera 93 checks all passed, with no excluded grace
frames and no late, mismatched or failed mask draws:

| Render cap | Parent frames | First draws with masks | Video frames skipped by clock | Evidence directory |
| --- | --- | --- | --- | --- |
| 60 Hz | 1677990:1678320 | 331/331 | 0 | `/tmp/crimson-mask-playback-smoke.baH6eZ` |
| 30 Hz | 1677990:1678320 | 330/330 | 1 | `/tmp/crimson-mask-playback-smoke.YZQHlY` |
| 60 Hz, clip boundary | 53990:54320 | 329/329 | 2 | `/tmp/crimson-mask-playback-smoke.JPgzhl` |

Each evidence directory contains `app.log`, `playback.jsonl` and `coverage.json`.
The retained decoded-mask cache peaked at 5,905,630 bytes (5.63 MiB), below its
96 MiB budget; the observed read-ahead was nine video frames. The clip-boundary
run logged both loading and presentation of clip 1 at parent frame 54000.

Local package (not published):
`dist/Crimson-linux-x86_64-mask-prefetch-test-20260921`.
It also contains the independently tested camera-window identity fix. The user
visually accepted this candidate on September 21 and requested a commit, push,
and new colleague release. Release artifacts and their source revision are
recorded separately in the published package metadata and START_HERE guide.

These tests establish local playback behavior, not cold-storage latency guarantees
or qualification on Ginny's Ubuntu 22 / RTX A4000 workstation. The automated
coverage checks use eight seconds of paused prewarm and do not flush filesystem
or remote-storage caches. Immediate playback after an arbitrary cold seek can
still briefly wait for exact-frame data; the renderer never substitutes an old
mask. Read-ahead does not repair missing or failed inference results in the source.
