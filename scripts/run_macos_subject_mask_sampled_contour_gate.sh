#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${CRIMSON_SUBJECT_MASK_CONTOUR_BENCHMARK:-$repo_root/build/macos-arm64-release/subject_mask_v1_long_duration_benchmark}"
source_store="${CRIMSON_SUBJECT_MASK_SOURCE_STORE:-/Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/subject_mask_storage/full_duration/sleepyfish_cam2010095_20260731_73f7bb5e/analysis.zarr}"
source_run="${CRIMSON_SUBJECT_MASK_SOURCE_RUN:-refined_subject_masks_sleepyfish_subject_mask_full_duration_20260731_73f7bb5e}"
source_digest="${CRIMSON_SUBJECT_MASK_SOURCE_DIGEST:-9efe2d3e5865d495e40779ecb1be3fcc55953b02bd82ca20125f953c0c2aa78c}"
cache_store="${CRIMSON_SUBJECT_MASK_CONTOUR_STORE:-/Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/subject_mask_storage/sampled_contours/sleepyfish_cam2010095_sampled_contours_20260801_90829491/cache.zarr}"
cache_run="${CRIMSON_SUBJECT_MASK_CONTOUR_RUN:-subject_mask_sampled_contours_sleepyfish_20260801_90829491}"
cache_digest="${CRIMSON_SUBJECT_MASK_CONTOUR_DIGEST:-c04a3f9283da0bd9bd16497b7ecc9ca9871c816ad517b1e5d40b092bde8c6861}"
workload="${CRIMSON_SUBJECT_MASK_CONTOUR_WORKLOAD:-$repo_root/tools/fixtures/subject_mask_v1_long_duration_workload_v1.json}"
output_dir="${CRIMSON_SUBJECT_MASK_CONTOUR_OUTPUT_DIR:-/private/tmp/crimson-subject-mask-sampled-contour}"
repetitions="${CRIMSON_SUBJECT_MASK_CONTOUR_REPETITIONS:-}"

if [[ ! -x "$binary" ]]; then
    echo "Benchmark executable not found: $binary" >&2
    echo "Build it with: cmake --build build/macos-arm64-release --target subject_mask_v1_long_duration_benchmark -j6" >&2
    exit 2
fi
if [[ ! -r "$source_store/zarr.json" || ! -r "$cache_store/zarr.json" ]]; then
    echo "The source mask or sampled-contour store is not readable." >&2
    exit 2
fi
if [[ ! -r "$workload" ]]; then
    echo "Workload is not readable: $workload" >&2
    exit 2
fi
if ! command -v jq >/dev/null 2>&1; then
    echo "jq is required to validate and aggregate benchmark results." >&2
    exit 2
fi

actual_source_digest="$(jq -er '.attributes.run_manifest.payload_digest' \
    "$source_store/refined_subject_masks_runs/$source_run/zarr.json")"
actual_cache_digest="$(jq -er '.attributes.run_manifest.payload_digest' \
    "$cache_store/subject_mask_cache_runs/$cache_run/zarr.json")"
if [[ "$actual_source_digest" != "$source_digest" ]]; then
    echo "Source subject-mask manifest digest mismatch." >&2
    exit 1
fi
if [[ "$actual_cache_digest" != "$cache_digest" ]]; then
    echo "Sampled-contour manifest digest mismatch." >&2
    exit 1
fi

if [[ -z "$repetitions" ]]; then
    repetitions="$(jq -er '.repetition_count | select(type == "number" and . > 0 and floor == .)' "$workload")"
fi
if ! [[ "$repetitions" =~ ^[1-9][0-9]*$ ]]; then
    echo "CRIMSON_SUBJECT_MASK_CONTOUR_REPETITIONS must be a positive integer." >&2
    exit 2
fi

mkdir -p "$output_dir"
results=()
for ((repetition = 0; repetition < repetitions; ++repetition)); do
    output="$output_dir/repetition-$(printf '%03d' "$repetition").json"
    echo "Running sampled-contour process $((repetition + 1))/$repetitions"
    "$binary" \
        --store "$source_store" \
        --run "$source_run" \
        --manifest-payload-digest "$source_digest" \
        --presentation-cache-store "$cache_store" \
        --presentation-cache-run "$cache_run" \
        --presentation-cache-manifest-payload-digest "$cache_digest" \
        --frame-size 4512x4512 \
        --workload "$workload" \
        --repetition "$repetition" \
        --output "$output"
    results+=("$output")
done

aggregate_partial="$output_dir/aggregate.partial.json"
jq -s '
  def median: sort | .[length / 2 | floor];
  {
    schema_id: "crimson.subject_mask_sampled_contour.aggregate",
    schema_version: 1,
    status: (if all(.[]; .status == "pass") then "pass" else "fail" end),
    classification: "selector_ineligible_full_duration_consumer_gate",
    profile_promotion_verdict: "not_evaluated",
    trial_count: length,
    crimson_commits: (map(.crimson_commit) | unique),
    worktree_dirty_values: (map(.worktree_dirty) | unique),
    workload_sha256_values: (map(.workload_sha256) | unique),
    source_manifest_digest_values: (map(.manifest_payload_digest) | unique),
    cache_manifest_digest_values:
      (map(.presentation_cache_manifest_payload_digest) | unique),
    summary: {
      first_presentation_readiness_ms_median:
        (map(.first_presentation_readiness_ms) | median),
      warm_random_p95_ms_median: (map(.random_warm.p95_ms) | median),
      forward_page_p95_ms_median:
        (map(.forward_traversal.page_p95_ms) | median),
      reverse_page_p95_ms_median:
        (map(.reverse_traversal.page_p95_ms) | median),
      deadline_miss_ratio_max: (map(.effective_deadline_miss_ratio) | max),
      rapid_seek_final_readiness_ms_median:
        (map(.rapid_seeks.final_readiness_ms) | median),
      stale_visible_frames_total:
        (map(.rapid_seeks.stale_visible_frames) | add),
      process_file_bytes_median: (map(.process_physical.file_bytes) | median),
      peak_rss_bytes_median: (map(.peak_rss_bytes) | median),
      frame_offset_reads: (map(.repository_metrics.frame_offset_reads) | unique),
      dense_mask_payload_reads:
        (map(.repository_metrics.dense_mask_payload_reads) | unique),
      source_point_count_open_attempts:
        (map(.repository_metrics.source_point_count_open_attempts) | unique),
      source_point_count_payload_reads:
        (map(.repository_metrics.source_point_count_payload_reads) | unique)
    },
    trials: .
  }
' "${results[@]}" > "$aggregate_partial"
mv "$aggregate_partial" "$output_dir/aggregate.json"

status="$(jq -er '.status' "$output_dir/aggregate.json")"
echo "Aggregate: $output_dir/aggregate.json"
[[ "$status" == "pass" ]]
