# Crimson Mask Overlay Async Buffering Notes

Date anchored: 2026-07-07.

## Summary

Double buffering the ROI inset can help only for the GPU-facing handoff. It can
avoid updating an OpenGL texture while ImGui is drawing the previous inset, but
it does not reduce Zarr reads, compact RLE decode, dense mask allocation, or CPU
mask/contour preparation.

The desired behavior is:

- video playback never waits for mask decode or upload,
- geometry/keypoint ROI inset remains cheap and synchronous,
- mask overlays decode asynchronously when possible,
- the GUI displays the latest ready mask result and skips or lags masks if the
  worker is behind.

## Practical Direction

Use a worker-side mask preparation path plus a small publish buffer:

1. GUI thread identifies the visible frame, ROI, and enabled components.
2. Worker reads and decodes the required mask rows/components.
3. Worker publishes an immutable decoded mask payload.
4. GUI thread uploads or reuses GPU textures from the latest ready payload.
5. If no payload is ready, playback continues without blocking on masks.

Double buffering is still useful after this split. The front texture can be
drawn while the back texture is prepared, then atomically swapped on the GUI
thread. This smooths texture ownership and upload stalls, but it is not a
replacement for moving RLE reads/decode off the render cadence.

## Telemetry

Mask perf JSONL samples include a `mask_work` object:

- `estimated_total_ms`: `data_load_ms + overlay_draw_ms`
- `data_load_ms`: synchronous mask load/decode time measured around
  `getRawDetections(... include_eye_masks=true ...)`
- `overlay_draw_ms`: total mask overlay render time
- `texture_total_ms`: texture lookup plus texture upload time
- `contour_total_ms`: contour build plus contour draw time
- `cpu_overlay_detail_ms`: fill, contour, axis, and pick sub-costs
- `dominant_stage`: largest measured bucket for that sample

Summarize logs with:

```bash
python3 scripts/summarize_mask_perf_jsonl.py --top 12 \
  ~/.cache/crimson/buffer_dumps/mask_perf_latest.jsonl
```

For RLE-backed archives, high `data_load_ms` points to read/decode pressure.
High `texture_total_ms` points to upload/cache churn. High `overlay_draw_ms`
with low texture time points to draw or geometry work.

## GoodCopBadCop RLE Smoke

Test archive:

```text
/groups/johnson/johnsonlab/jeremy/recordings/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop/zarr/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop_analysis.zarr
```

Test run:

```text
refined_subject_masks_smart_finalizer_rle_chunk512_20260620_02
```

This is the useful modern Crimson smoke target because it has `masks_roi`,
`mask_rle`, `frame_indices`, `source_crop_row_ids`, component contours, and
modern subject mask labels. The archive-level `latest` currently points at a
bitpacked canary, so use the explicit run above for Crimson RLE playback tests.

On 2026-07-07, the RLE playback smoke passed but showed large synchronous mask
load stalls:

- `mask_work_overlay_draw_ms` p95: about `0.63 ms`
- `mask_work_texture_total_ms` p95: about `0.59 ms`
- `mask_work.data_load_ms` p95: about `377 ms`
- worst `mask_work.data_load_ms`: about `1783 ms`

The slowest frame loop was about `1787 ms` and was dominated by mask data load.
The drawing path itself was sub-ms. This means the first implementation slice
should prevent playback from blocking on mask chunk reads/decode; draw
optimization is not the limiting factor yet.

## Implementation Slice

Playback should use a cache-first mask policy:

1. Queue mask chunks for the current and upcoming frames on the existing
   prefetch worker.
2. While playback is running, render only mask chunks that are already cached.
3. If a chunk is missing, skip the mask for that frame and keep playback moving.
4. While paused, seeking manually, or editing, keep the blocking path so the UI
   can show exact masks once the user asks for a specific frame.

This is intentionally smaller than a full decoded-mask publish buffer. It uses
the existing chunk cache and background worker, removes the worst UI-thread
stalls during playback, and keeps the later worker-side decoded payload design
available for texture double-buffering and tighter frame/component scheduling.

## Slice Verification

After adding playback cache-only mask reads plus 1024-frame prefetch lookahead,
the same RLE playback smoke passed without UI-thread mask-load stalls:

```text
[PlaybackSmoke] PASS start_frame=0 end_frame=300 presented_frame=300 presented_count=339 elapsed_s=2.9929
```

Telemetry summary:

- `frame_loop_ms` p95: about `8.41 ms`
- `frame_loop_ms` max: about `10.42 ms`
- `mask_work.data_load_ms` p95: about `0.022 ms`
- `mask_work.data_load_ms` max: about `0.062 ms`
- `mask_work.overlay_draw_ms` p95: about `0.43 ms`
- `mask_work.texture_total_ms` p95: about `0.40 ms`

The old multi-hundred-ms and multi-second UI-thread mask data-load spikes were
not present in this smoke. This confirms that cache-only playback protects the
render loop from compact RLE read/decode misses.

However, this is not yet continuous RLE mask playback. With
`kRleEyeMaskChunkRows = 32`, chunk perf logs showed background chunk reads such
as:

- chunk 1: about `369 ms`
- chunk 2: about `1081 ms`
- chunk 3: about `956 ms`
- chunk 4: about `1294 ms`

At 100 FPS playback, a 32-frame chunk is consumed in about `320 ms`, so the
single background prefetch worker cannot always decode/read compact RLE chunks
before playback reaches them. In cache-only mode this means masks may lag or be
skipped for frames whose chunks are not ready, while video playback remains
smooth. The perf log now includes `mask_work.invalid_roi_count` to make these
cache-only drops explicit. Dense `masks_roi` remains the better realtime path
when available; true compact-only RLE realtime playback needs another slice.

The next RLE-specific slice should measure and choose between:

- a worker-side decoded-frame or decoded-chunk ring published to the GUI thread,
- larger logical RLE chunk groups or adaptive chunk groups,
- more than one RLE prefetch worker if TensorStore/network I/O benefits,
- optional blocking prewarm for a configurable playback window,
- Palette-side RLE/read layout changes if count-slice reads remain the limit.

## Dense vs RLE vs Bitpacked Design Pass

Date anchored: 2026-07-08.

The GoodCopBadCop run above is useful because it contains both dense
`masks_roi` and compact `mask_rle` for the same refined subject-mask run. On
this archive, the physical mask payloads are:

| format | path | physical size | file count | chunk/read shape |
| --- | --- | ---: | ---: | --- |
| dense uint8 | `masks_roi` | `76M` | `941` | Zarr shape `[120221, 4, 512, 512]`, chunk shape `[512, 1, 512, 512]` |
| component RLE | `mask_rle` | `29M` | `3828` | per-component count arrays; `counts` chunk shape `[1048576]` |
| bitpacked canary | `mask_bitpacked/masks_packed` | `35M` | `1875` | Zarr shape `[120221, 4, 512, 64]`, chunk shape `[256, 1, 512, 64]` |

Dense is about `2.6x` larger than RLE on disk, but it is much more predictable
for playback. One dense chunk covers 512 mask rows for one component and can be
prefetched by row window. RLE is smaller but requires component-separated
variable-length `counts` reads, and those reads are the expensive part.

The RLE chunk telemetry shows the difference clearly:

- RLE chunk 0 prewarm: `total_ms=140.924`,
  `rle_counts_read_ms=132.775`, `rle_decode_ms=0.967`.
- RLE chunk 1 background prefetch: `total_ms=787.575`,
  `rle_counts_read_ms=778.982`, `rle_decode_ms=1.324`.
- Dense chunk 0 prewarm: `total_ms=404.547`, split roughly between
  `dense_read_ms=186.233` and `dense_collect_ms=187.130`.
- Dense chunk 1 background prefetch: `total_ms=399.556`,
  split roughly between `dense_read_ms=188.885` and
  `dense_collect_ms=177.077`.

So compact RLE is not CPU-decode bound in this smoke. It is mostly
count-slice-read bound. A 32-row RLE chunk is consumed in about 320 ms at
100 FPS, but a background RLE chunk read can take hundreds of ms on this store.
Cache-only playback keeps the GUI responsive by skipping masks that are not
ready, but it does not make RLE masks continuous.

### Playback Smoke Results

All positive smokes used the same archive, run, and playback range. The only
request difference was the storage mode: omitted/auto, or an added
`--refined-subject-mask-storage dense` / `rle` flag.

```bash
release/redgui \
  --zarr /groups/johnson/johnsonlab/jeremy/recordings/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop/zarr/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop_analysis.zarr \
  --refined-subject-mask-run refined_subject_masks_smart_finalizer_rle_chunk512_20260620_02 \
  --show-subject-masks \
  --playback-smoke 0:800 \
  --swap-interval 0 \
  --frame-cap-fps 120 \
  --mask-perf-sample-every 1
```

Runtime summaries:

| request | actual source | visible masks | invalid masks | mask data load p95 | overlay draw p95 | frame loop p95 | result |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| auto | `masks_roi` | `100%` | `0%` | `0.0282 ms` | `0.4740 ms` | `8.4071 ms` | pass |
| dense | `masks_roi` | `100%` | `0%` | `0.0335 ms` | `0.5653 ms` | `8.4163 ms` | pass |
| RLE forced | `mask_rle` | `46.4%` | `53.6%` | `0.0259 ms` | `0.4831 ms` | `8.4091 ms` | pass, but skipped masks |

The forced-RLE numbers look smooth because playback now uses cache-only mask
reads. That is the intended UI-protection behavior, but it is not an acceptable
definition of "realtime mask playback" if users expect continuous masks. The
dense and auto paths displayed masks continuously over the smoke range.

After adding the Crimson bitpacked reader, the bitpacked-only canary run
`refined_subject_masks_smart_finalizer_subject_masks_bitpacked_only_canary_20260621_01`
was smoke-tested both explicitly and through auto storage selection.

```bash
release/redgui \
  --zarr /groups/johnson/johnsonlab/jeremy/recordings/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop/zarr/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop_analysis.zarr \
  --refined-subject-mask-run refined_subject_masks_smart_finalizer_subject_masks_bitpacked_only_canary_20260621_01 \
  --refined-subject-mask-storage bitpacked \
  --show-subject-masks \
  --playback-smoke 0:800 \
  --swap-interval 0 \
  --frame-cap-fps 120 \
  --mask-perf-sample-every 1
```

Positive logs:

- explicit bitpacked: `/tmp/crimson_mask_bitpacked_20260708_005709.log`,
  `/tmp/crimson_mask_bitpacked_20260708_005709.jsonl`
- auto-selected bitpacked when dense was absent:
  `/tmp/crimson_mask_bitpacked_auto_20260708_005747.log`,
  `/tmp/crimson_mask_bitpacked_auto_20260708_005747.jsonl`

Both runs reported `path=mask_bitpacked`,
`row_position_fallback=no`, and `crop_frame_match=yes`. The explicit 0:800
smoke produced 920 samples with `overlay_enabled=true`, `visible masks=100%`,
`invalid masks=0%`, `mask_work_data_load_ms p95=0.0307 ms`,
`mask_work_overlay_draw_ms p95=0.4943 ms`, and `frame_loop_ms p95=8.4131 ms`.
The auto-selection 0:300 smoke produced 347 samples with
`overlay_enabled=true`, `visible masks=100%`, `invalid masks=0%`,
`mask_work_data_load_ms p95=0.0258 ms`, `mask_work_overlay_draw_ms p95=0.4358 ms`,
and `frame_loop_ms p95=8.4088 ms`.

Chunk telemetry is still important. The 0:800 smoke saw bitpacked chunk reads
ranging from about `18-666 ms` and bit unpack work around `22-44 ms` per
256-row chunk. Once a chunk was cached, render-loop mask data lookup was tiny,
but cold bitpacked chunks still need background prefetch/headroom for continuous
playback.

### Network Read Design Pass

For the current Crimson reader over `/groups` storage, dense still has the best
measured network read behavior for playback, even though it is larger on disk.
The reason is that dense uses larger, fixed row chunks and avoids the
variable-length count-slice access pattern that makes RLE expensive.

Cold chunk telemetry from the current smokes:

| format | row chunk | mean read | read range | mean read per row | mean total per row |
| --- | ---: | ---: | ---: | ---: | ---: |
| dense `masks_roi` | `512` | `208.6 ms` | `179.1-336.1 ms` | `0.407 ms` | `0.899 ms` |
| bitpacked `mask_bitpacked` | `256` | `196.6 ms` | `18.6-666.0 ms` | `0.768 ms` | `0.984 ms` |
| RLE `mask_rle` | `32` | `185.5 ms` | `77.3-1288.0 ms` | `5.796 ms` | `6.256 ms` |

The important comparison is per row and chunk duration, not only physical size:

- dense `512` rows lasts about `5.12 s` at 100 FPS;
- bitpacked `256` rows lasts about `2.56 s` at 100 FPS;
- RLE `32` rows lasts about `0.32 s` at 100 FPS.

Dense has enough chunk duration for the background prefetch worker to hide the
network read. Bitpacked also kept up in the 0:800 canary, but it has less
prefetch headroom and showed larger read variance. RLE chunks are too small for
the observed network/count-read variance; a slow RLE count read can take longer
than several chunks of playback.

The current bitpacked canary uses:

```text
shape       [120221, 4, 512, 64]
chunk_shape [256, 1, 512, 64]
codecs      bytes + zstd(level=0)
```

A Crimson row-window read asks for all channels, so the current layout touches
four component chunks for each row chunk. That is useful if a reader only needs
one component, but Crimson's review overlay usually wants all visible subject
components. For this reader, component chunking may be as important as row
chunking.

Recommended Palette canary matrix:

| candidate | expected benefit | expected cost |
| --- | --- | --- |
| `[512, 1, 512, 64]` | halves row chunk count and doubles prefetch time window to `5.12 s` | doubles bit-unpack work per chunk; still touches four component chunks |
| `[1024, 1, 512, 64]` | quarters row chunk count and gives `10.24 s` prefetch window | larger cold seek over-read; larger per-chunk unpack/cache memory |
| `[512, 4, 512, 64]` | one chunk object covers all components for Crimson's common read | less selective for component-only readers |
| `[1024, 4, 512, 64]` | fewest file/object opens for playback windows | largest cold seek over-read and largest cache entries |

The most useful first test is `[512, 4, 512, 64]`, followed by
`[1024, 4, 512, 64]` if random seek latency stays acceptable. Both match
Crimson's current all-component display path better than `[*, 1, ...]`.

### Prefetch Hardening

Date anchored: 2026-07-08.

Cold bitpacked chunks can still arrive slowly over `/groups`, even when the
chunk files are physically small. In one `[512, 4, 512, 64]` smoke, the first
three chunks were around `125-137 KB` each, but chunk read times ranged from
about `108 ms` to `380 ms`. That variance is enough to show as masks falling
behind if Crimson waits until the current playback frame is close to a chunk
boundary before requesting the next chunk.

The prefetch path now explicitly queues the current chunk plus two future mask
chunks when playback asks for a frame cache window. This is in addition to the
existing frame-window scan and adjacent-chunk prefetch from synchronous chunk
loads. The intended steady state is that the current chunk and two ahead chunks
are either cached, queued, or in-flight before playback reaches them.

Enable prefetch diagnostics with:

```bash
CRIMSON_SUBJECT_MASK_PREFETCH_TRACE=1
```

The trace logs:

- queued chunks and dropped queue entries,
- worker completion time and remaining queue depth,
- per-frame current chunks, attempted chunks, queued chunks, and
  `covered_ahead_min`.

In the `[512, 4, 512, 64]` canary smoke, Crimson queued chunks `1` and `2`
immediately after loading chunk `0`. Chunk `1` finished in about `150 ms`;
chunk `2` finished in about `416 ms`. Playback was still on chunk `0`, so the
render loop did not block or skip masks:

- playback smoke: pass, frames `0:300`
- `frame_loop_ms` p95: about `8.41 ms`
- `mask_work.data_load_ms` p95: about `0.028 ms`
- `mask_work.overlay_draw_ms` p95: about `0.44 ms`
- `visible masks`: `100%`
- `invalid masks`: `0%`

This does not remove network variance. It gives the worker more time to absorb
that variance before playback reaches the next chunk.

Promotion criteria for bitpacked as a compact review cache:

1. `visible masks=100%` and `invalid masks=0%` on cold 0:800 and longer
   playback smokes.
2. Cold random seeks across chunk boundaries do not visibly stall the GUI.
3. Chunk `read + unpack + optional contour` p99 stays comfortably below the
   amount of playback covered by one chunk.
4. Cache memory remains bounded; if 1024-row chunks are used, compact-mask cache
   capacity may need to drop from 8 chunks or become byte-budgeted.

Until those canaries pass, Crimson should remain dense-first for active review
archives. For compact-only archives, bitpacked is now the best direction to
optimize; RLE should remain the archival fallback rather than the preferred
network playback surface.

### Storage Policy Recommendation

For active Crimson review, prefer dense-first publication:

1. If `masks_roi` exists, Crimson should use it by default.
2. If dense is absent and `mask_bitpacked` exists, Crimson should prefer
   bitpacked over RLE. It has fixed row/component addressing and was continuous
   in the 0:800 canary smoke.
3. Keep `mask_rle` as a compact fallback when dense and bitpacked are absent or
   when the user explicitly forces RLE.
4. For user-facing review archives where realtime mask playback matters,
   Palette should publish dense plus compact, not compact-only.
5. For archival or transfer-oriented outputs, compact-only RLE is acceptable,
   but Crimson should make it visible that playback may skip masks until chunks
   are cached.

Bitpacked is now the most promising compact realtime display path. It preserves
fixed row/component addressing like dense, is close to RLE in physical size on
this canary (`35M` vs `29M`), and avoids variable-length count-slice reads.
The remaining work is prefetch/caching policy, not basic read support.

### Follow-Up Experiments

The next storage/performance slice should:

1. Re-run the same `0:800` smoke for dense, RLE, and bitpacked using
   `mask_work.visible_roi_count`, `mask_work.invalid_roi_count`,
   `mask_work.data_load_ms`, `mask_work.texture_total_ms`, and chunk perf logs.
2. Test a longer bitpacked playback window after cold cache and after warmed
   cache, especially random seeks across chunk boundaries.
3. Tune the bitpacked prefetch window and cache size. Cold chunk reads can still
   exceed the time needed to play through a 256-frame chunk at 100 FPS.
4. If compact-only RLE must support continuous realtime playback, redesign the
   RLE storage/read path around row-window chunking or build a decoded display
   cache ahead of playback. More render-loop tuning will not fix the current
   count-read bottleneck.
