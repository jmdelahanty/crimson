# Crimson Frame Pacing Performance TODO

Date anchored: 2026-05-03.

Lifecycle: **deferred active performance work**.

## Purpose

Capture the current `120 FPS` render-cap investigation so it does not block
mask-overlay and UI modularization work.

This is a deferred performance task. The current evidence does not point to
subject-mask fills or contours as the primary bottleneck.

## Current Observation

When Crimson is launched with:

```bash
./release/redgui \
  --zarr /nvme1/recordings/2026-01-28T23-15-10Z_arena_2_Feeding/zarr/2026-01-28T23-15-10Z_arena_2_Feeding_analysis.zarr/ \
  --swap-interval 0 \
  --frame-cap-fps 120
```

the UI often reports about `117-119 FPS` instead of exactly `120 FPS`.

The current perf JSONL is written by default to:

```text
~/.cache/crimson/buffer_dumps/mask_perf_latest.jsonl
```

and can be summarized with:

```bash
python3 scripts/summarize_mask_perf_jsonl.py --top 12 \
  ~/.cache/crimson/buffer_dumps/mask_perf_latest.jsonl
```

## Most Recent Measurement

The latest mask-enabled run showed:

- frame cap: `120 FPS` (`8.333 ms` budget)
- playback loop rate: about `119 FPS`
- paused loop rate: about `117.6-118.9 FPS`
- playback frame loop mean: about `8.40 ms`
- paused frame loop mean: about `8.50 ms`

Mask overlay costs were measurable but small:

- playback `mask_total_draw_ms`: mean about `0.22 ms`, p95 about `0.45 ms`
- playback `texture_upload_ms`: mean about `0.16 ms`, p95 about `0.36 ms`
- playback `contour_draw_ms`: mean about `0.014 ms`
- playback `fill_draw_ms`: mean about `0.003 ms`

The larger per-frame contributors were:

- UI build: about `3.8 ms`
- GL draw: about `0.8 ms`
- swap: about `0.9 ms`
- frame-cap sleep: about `2.7-3.0 ms`

## Interpretation

The app is not obviously rendering-bound in this scenario because it still
sleeps for several milliseconds per frame. The small gap from `120 FPS` is most
likely frame pacing overhead:

- `std::this_thread::sleep_until` wakeup jitter
- OS scheduler granularity
- swap timing variance
- measurement window noise
- occasional UI or GL spikes

Mask fills and contours should still be optimized when they become a real
constraint, but this run does not justify interrupting current overlay/UI
refactoring for mask-specific performance work.

## Deferred Work

When we return to this, prefer a focused frame-pacing patch before changing
overlay rendering:

1. Add a frame-pacing mode that sleeps most of the remaining frame budget, then
   yields or busy-waits for the final short interval.
2. Add a small early-wakeup bias to compensate for scheduler overshoot.
3. Record both requested sleep duration and actual sleep duration in the perf
   JSONL.
4. Split frame-loop timing into:
   - work time before cap sleep
   - requested cap sleep
   - actual cap sleep
   - post-sleep overshoot
5. Compare `--frame-cap-fps 120` with:
   - current `sleep_until`
   - early-wakeup sleep
   - hybrid sleep/spin
   - uncapped rendering, for headroom only
6. Keep `--swap-interval 0` as the main test path when evaluating the app's own
   frame pacing.

## Acceptance For Future Patch

A future frame-pacing patch should show:

- steady observed render rate closer to the requested cap
- no increase in visible playback jitter
- no large CPU burn when the app is idle or paused
- perf JSONL clearly explaining whether missed frames are work-bound,
  swap-bound, or sleep-overshoot-bound

## Non-Goals

Do not use this task to:

- change Palette Zarr contracts
- rewrite mask fill rendering
- remove contour rendering
- move timeline/event plotting
- add new UI controls unrelated to frame pacing
