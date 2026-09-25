# August contours and shape controls — 2026-09-23

Implemented on `codex/main-integration-20260922`, following `658c0e0`.
This change does not modify recordings, merge main, or update Ginny's package.

## Scope

- Camera View and Frame Inspect's **Masks / Shape** controls now expose body,
  eye and swim-bladder outlines independently of filled mask visibility.
- All existing shape controls reach canonical rendering: landmarks, body axes,
  centerline, sampled B-spline, individual spline markers, control polygon and
  points, tail samples and normals. Invalid B-splines do not produce control
  geometry. Observation keys are retained on emitted primitives.
- The source is the uniquely matching same-archive sampled-contour cache,
  bound to the selected mask run and manifest digest. Discovery does not pick
  an unrelated latest run or promote a selector. The existing strict reader
  validates the manifest, source binding and physical metadata before use.
- These are the published **largest external sampled contours**: 128 body,
  64 left-eye, 64 right-eye and 32 swim-bladder points when valid. They are not
  every pixel boundary, hole, or disconnected mask component. Dense masks
  remain the scientific pixel authority; no contours are recomputed or written.
- ROI-local contour coordinates now receive the same width/height scaling and
  translation as mask fills. Shape arrays already in camera pixels are not
  transformed a second time.

## Loading and identity

Fills and contours have separate asynchronous buffers and scheduler source
identities. Disabled outlines submit no contour payload work. Slow or failed
contour payload reads do not directly gate publication of ready fills. Both
still share the bounded scheduler, storage connection and TensorStore cache;
this is not a guarantee of complete resource isolation or storage deadlines.

Contour composition checks the presented frame, bound source digest, unique
observation keys, crop row, ROI placement and component identities against the
dense frame. Consequently, **outline-only display still requests dense frames
for identity matching**. It is not a zero-dense-I/O presentation mode.

Source and cache discovery/opening remain on the lifecycle worker. The contour
reader currently retains a second bounded source mapping (about 224 MiB for
camera 2010093), rather than sharing the dense reader's mapping. Contour payload
and frame caches have separate byte/count bounds. This increases opening work
and memory; mapping deduplication is not part of this increment. Repository
opening is still serial on that worker, so contour metadata/mapping opening
delays initial session publication even when outlines are hidden. The runtime
payload independence described above does not remove that startup cost.

Unavailable or ambiguous caches have visible diagnostics. Invalid/empty
inference remains distinct from a read failure. Stale contour frames are never
reused on a new video frame.

## Local visual check

Use the newly built executable, not an older `dist` or public-package binary:

```bash
crimson_repo=/home/delahantyj@hhmi.org/gitrepos/crimson-main-integration-20260922
crimson_stage="$crimson_repo/dist/Crimson-linux-integration-20260922"
crimson_test_dir="$(mktemp -d /tmp/crimson-contours-user-check.XXXXXX)"
cd "$crimson_test_dir"
LD_LIBRARY_PATH="$crimson_stage/lib:$crimson_stage/lib64:$crimson_stage/lib/crimson/private:$crimson_stage/lib64/crimson/private" \
XDG_CONFIG_HOME="$crimson_test_dir/config" \
XDG_CACHE_HOME="$crimson_test_dir/cache" \
"$crimson_repo/release/redgui" \
  --zarr /misc/public/forGinny/recordings/2026_08_06_19_13_35_cam2010093/zarr/2026_08_06_19_13_35_cam2010093_analysis.zarr \
  --show-subject-masks --swap-interval 0 --frame-cap-fps 60
```

Supply an authenticated `DISPLAY`/`XAUTHORITY` pair in tmux; see `AGENTS.md`.
In **Frame Inspect → Masks / Shape**, enable **Show shape / contours**, then
the desired outlines, **Spline debug points**, **Control points**, **Tail
samples** or **Tail normals**. Turning off **Show subject masks** hides fills
without disabling the selected outlines. These additional overlays start off
by default.

## Validation and limits

Validation passed on the local NVIDIA workstation using the pinned Ubuntu 22 /
CUDA 12.4 builder. The production `release/redgui` SHA256 is
`bf991435c2c6f3ec7c6f7be2dc26f357617e0fea0003f24dd44bffb609abab2b`.

- Full build and **107/107 CTests** passed. The mask repository test passed
  20 consecutive runs; session and presentation tests each passed 10 runs.
  All eight Python playback-checker tests, shell syntax checks and
  `git diff --check` passed.
- All four August archives (cameras 2010093–2010096) passed strict repository
  probes at frames 0, 53990, 54000, 54010, 2565015, 2862000 and 2937603,
  requiring positive contour and shape-sample coverage across each workload.
  Genuine invalid/empty inference frames remained absent rather than becoming
  fabricated geometry. This is sampled coverage, not a full-recording audit.
- Authenticated GPU playback at a 60 Hz UI cap crossed the 54,000-frame clip
  boundary (53990–54320). Outlines were ready on the first draw for **329/329
  presented frames**, with zero contour/frame-identity errors. A separate
  fills-only run also passed **329/329**, with **zero contour payload reads**.
  Each run reported two unpresented video-clock frames separately; neither is
  a claim of zero video skips or of 60 unique source frames per second.
- A paused GPU capture at frame 54010 showed four outlines, no fills and 199
  shape primitives, including spline/control markers and tail samples/normals.
  The capture was visually inspected. Required legacy June playback (0–300)
  also passed with the final production binary.

An initial combined fill/contour payload path missed 11 first draws during
concurrent archive reads. That dependency was removed before acceptance;
the final split-buffer outline run passed while an archive probe was also
running. These results do not establish arbitrary cold-storage latency bounds.
The shared scheduler retains its existing one-active-speculative-job limit
and reserved current-frame capacity; asynchronous lookahead remains enabled.

Final four-archive probe opening times were 7.57–8.25 seconds, with peak process
RSS of 1,036,028–1,122,328 KiB. Those are measurements of this workload, not
hard process-memory caps or storage-cold guarantees.

Raw evidence is retained locally in
`/tmp/crimson-overlay-completion-20260923.Edzj7j`; final GPU runs are in
`/tmp/crimson-mask-playback-smoke.SGJo0V` (outlines),
`/tmp/crimson-mask-playback-smoke.Y5eDLn` (fills), and
`/tmp/crimson-canonical-overlay-smoke.fyWOEV` (paused capture).
These temporary directories are not committed artifacts.

The canonical ROI inset renderer, eye-geometry overlays and swim-bout/core
shading on the speed graph remain outside this increment. Native macOS and
Windows were not tested; shared scene support is not native-backend acceptance.
