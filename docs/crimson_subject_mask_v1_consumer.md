# Subject-mask v1 consumer checkpoint

Date: 2026-07-31

Status: selector-ineligible correctness and visual-demo gate passed; physical
profile promotion and production selection remain out of scope.

## Scope

Crimson now has a backend-neutral exact-schema reader for Palette's refined
subject-mask dense-core contract. The reader is separate from the legacy mask
compatibility adapter and is enabled only by an explicit benchmark request.
It:

- validates the canonical run-manifest envelope, payload digest, logical
  schema, component registry, storage declaration, and logical-content digest;
- compares direct and root-consolidated metadata for the run group and all 13
  required arrays;
- opens every array with one exact compile-time dtype and rank, without dtype
  probing or aliases;
- reads and retains the authoritative `int64[F+1]` frame offsets exactly once;
- validates the complete offset/frame relationship and unique `uint64`
  `instance_key` values;
- resolves every row in `[offsets[f], offsets[f+1])`, preserving empty and
  multi-observation frames;
- preserves each observation's crop placement and identity through the shared
  presentation repository; and
- leaves the seven derived metric payload arrays unread during ordinary
  playback. The dense mask chunk is converted to exact sparse foreground-pixel
  indices and the temporary dense decode is released.

The contract contains no `roi_images` surface, so the strict reader cannot open
or synthesize one. Legacy stores continue through the existing compatibility
reader.

## Immutable fixture

Archive:

```text
/Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/subject_mask_storage/integration/20260128_cropv2_subject_mask_cache_pipeline_20260731_v2/refined.zarr
```

Explicit run:

```text
refined_subject_masks_runs/refined_subject_masks_cache_canary_v1
```

Handoff payload digest:

```text
dbc958d2be32f530b1deffafb99e84d2150d8948171d05a2e4992fa89d07c6eb
```

Run-manifest payload digest:

```text
56cdc38699dff17c2331e161f73be50da372bedf49f3a707159d78ce81093d03
```

The fixture has 23,287 camera frames, 22,926 observation rows, four ordered
components, and 512x512 ROI masks. The persistent artifact was not modified.

## Mounted evidence

The mounted headless probe validated the complete schema and presented camera
frame 1000 with one observation, four present components, and 4,256 exact
foreground pixels. Repository open took 1.86 seconds, first demand took 174 ms,
and a warm repeat took 0.17 ms in that trial. One four-row mask chunk decoded
to 4 MiB and retained about 81 KiB in sparse form. Offsets were read once.

The Metal/AVFoundation GUI smoke presented frames 1000 through 1020 with the
mask overlay enabled. It recorded 19 mask presentations, 32 resolved frames,
zero missing or failed frames, no stale presentation, and 190.6 MiB peak process
memory. The exact end frame settled successfully after correcting transport
stop reconciliation so an explicitly committed smoke/end-of-stream target is
not replaced by the preceding presented frame.

The matching NRS video is HEVC with an `hev1` sample-entry tag, which this
machine's AVFoundation decoder rejects. A stream-copy-only, frame-zero-preserving
25-second `hvc1` compatibility remux was used for the visual gate; pixels were
not re-encoded. This is a media compatibility boundary, not a mask contract or
storage failure.

## Usage

Run the guarded local demo from the repository root:

```bash
scripts/launch_macos_subject_mask_v1_demo.sh
```

The script verifies both immutable digests, requires the PRFS and NRS mounts,
creates or reuses the temporary `hvc1` stream-copy demo when required, and
opens Crimson paused at frame 1000 with subject masks visible.

## Remaining work

- Run a representative long-duration subject-mask physical-layout and cache
  gate before any storage-profile promotion.
- Add production refined-mask selection only after Palette freezes and
  activates the corresponding selector/lifecycle contract.
- Validate native Windows execution. Linux compilation and portable tests are
  part of this checkpoint, but the Metal visual gate is intentionally macOS
  specific.
