# Subject-Mask Bundle V4 Qualification Evidence

Date: 2026-08-12

Verdict: **PASS**. The receipt-composed physical profile is approved for one
digest-bound, selector-ineligible production-path publication candidate.
Production activation remains unauthorized.

## Identity

- Crimson implementation commit:
  `eef6322db55646f119eb3eb93a05026b65fe5bcf`
- Palette receipt SHA-256:
  `8ccd4b1ce19e1ad632d48b8ef16dbbc2eb963170561a3fb4232dc944a6810a96`
- Bundle manifest digest:
  `1404538de8e53503dbc39449ca9a3d2e60ba1e0cb71d7c54e3da7d062ecccc7f`
- Aggregate SHA-256:
  `c2abb6bbcede12451898049a7a066c2fdfe7588f9fcace33a836397a5b56fad8`
- Platform: Apple Silicon macOS, mounted Johnson Lab network volume
- Cache state: uncontrolled macOS/network-filesystem cache

All ten benchmark processes report the exact Crimson commit above and
`worktree_dirty=false`.

## Correctness

The explicit bundle-v4 probe validated 51 exact array declarations across the
raw, refined, quality, and sampled-contour members. Direct and consolidated
metadata, composable identities, manifests, coordinate bindings, storage
plans, codecs, and complete receipts agreed. The bundle remained
selector-ineligible, activation-deferred, and absent from ordinary selection.
Explicit wrong bundle and wrong digest requests both failed without fallback.

The complete crop join compared all 1,169,010 refined rows. It found zero
out-of-range rows, key mismatches, or placement mismatches. The real workload
included all 22 clip boundaries and empty frame 152,428. This recording has no
multi-row frames; the portable `[2, 0, 1, 3]` repository test supplies that
contract coverage.

Every presentation process read and retained `frame_row_offsets` once, opened
no quality payload, opened no `source_point_count`, published zero stale visible
frames, and had zero post-warmup deadline misses. Full ragged contours were
correctly absent.

## Five-Repetition Results

| Metric | Sampled contours | Dense authority |
| --- | ---: | ---: |
| First readiness, median | 2,538.4 ms | 1,055.2 ms |
| Warm random p95, median | 70.4 ms | 80.3 ms |
| Warm random p95, maximum | 72.3 ms | 127.9 ms |
| Forward 70-frame page p95, median | 42.9 ms | 537.7 ms |
| Reverse 70-frame page p95, median | 69.6 ms | 448.6 ms |
| Current-frame queue maximum | 110.7 ms | 140.0 ms |
| Rapid-seek final readiness maximum | 151.2 ms | 312.4 ms |
| Process file bytes, median | 135,686,874 | 39,332,434 |
| Process file reads, median | 1,781 | 4,999 |
| Peak RSS, maximum | 370,638,848 | 446,545,920 |

The contour path wins sustained presentation latency and memory. Dense masks
transfer fewer physical bytes in this fixture because the binary mask content
compresses strongly, but they require substantially more file reads and decode
work. Dense remains the scientific/edit authority and the explicit fallback;
sampled contours remain the ordinary presentation surface.

## Evidence Files

- `aggregate.json`: complete structured verdict, summaries, probes, and trials
- `aggregate.sha256`: aggregate digest
- `bundle_probe.json`: exact four-member bundle validation
- `crop_join_probe.json`: complete key and placement join
- `negative_wrong_bundle.json`: fail-closed explicit selection evidence
- `negative_wrong_digest.json`: fail-closed digest evidence
- `environment.json`: platform and cache-state classification
- `trials/`: five fresh-process results for each presentation mode

No fixture, selector, registry, production archive, or Palette state was
modified. A source-matched Metal view through the same 22-clip media provider
was accepted at the preceding sampled-contour integration checkpoint; this
qualification reran the storage and presentation paths headlessly and did not
claim a new GUI visual comparison.
