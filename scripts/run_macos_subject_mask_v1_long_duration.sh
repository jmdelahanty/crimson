#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${CRIMSON_SUBJECT_MASK_V1_BENCHMARK:-$repo_root/build/macos-arm64-release/subject_mask_v1_long_duration_benchmark}"
store="${CRIMSON_SUBJECT_MASK_V1_STORE:-/Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/subject_mask_storage/integration/20260128_cropv2_subject_mask_cache_pipeline_20260731_v2/refined.zarr}"
run="${CRIMSON_SUBJECT_MASK_V1_RUN:-refined_subject_masks_cache_canary_v1}"
digest="${CRIMSON_SUBJECT_MASK_V1_MANIFEST_PAYLOAD_DIGEST:-56cdc38699dff17c2331e161f73be50da372bedf49f3a707159d78ce81093d03}"
frame_size="${CRIMSON_SUBJECT_MASK_V1_FRAME_SIZE:-4512x4512}"
workload="${CRIMSON_SUBJECT_MASK_V1_WORKLOAD:-$repo_root/tools/fixtures/subject_mask_v1_long_duration_workload_v1.json}"
output_dir="${CRIMSON_SUBJECT_MASK_V1_OUTPUT_DIR:-/private/tmp/crimson-subject-mask-v1-long-duration}"

if [[ ! -x "$binary" ]]; then
    echo "Benchmark executable not found: $binary" >&2
    echo "Build it with: cmake --build build/macos-arm64-release --target subject_mask_v1_long_duration_benchmark -j6" >&2
    exit 2
fi
if [[ ! -r "$store/zarr.json" ]]; then
    echo "Subject-mask store is not readable: $store" >&2
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

repetitions="$(jq -er '.repetition_count | select(type == "number" and . > 0 and floor == .)' "$workload")"
mkdir -p "$output_dir"
results=()
for ((repetition = 0; repetition < repetitions; ++repetition)); do
    output="$output_dir/repetition-$(printf '%03d' "$repetition").json"
    echo "Running fresh process $((repetition + 1))/$repetitions"
    "$binary" \
        --store "$store" \
        --run "$run" \
        --manifest-payload-digest "$digest" \
        --frame-size "$frame_size" \
        --workload "$workload" \
        --repetition "$repetition" \
        --output "$output"
    results+=("$output")
done

aggregate_partial="$output_dir/aggregate.partial.json"
jq -s '
  def median: sort | .[length / 2 | floor];
  {
    schema_id: "crimson.subject_mask_v1.long_duration_aggregate",
    schema_version: 1,
    status: (if all(.[]; .status == "pass") then "pass" else "fail" end),
    trial_count: length,
    crimson_commits: (map(.crimson_commit) | unique),
    worktree_dirty_values: (map(.worktree_dirty) | unique),
    workload_sha256_values: (map(.workload_sha256) | unique),
    manifest_payload_digest_values: (map(.manifest_payload_digest) | unique),
    summary: {
      first_presentation_readiness_ms_median:
        (map(.first_presentation_readiness_ms) | median),
      warm_random_p95_ms_median: (map(.random_warm.p95_ms) | median),
      forward_page_p95_ms_median:
        (map(.forward_traversal.page_p95_ms) | median),
      reverse_page_p95_ms_median:
        (map(.reverse_traversal.page_p95_ms) | median),
      deadline_miss_ratio_max:
        (map(.effective_deadline_miss_ratio) | max),
      rapid_seek_final_readiness_ms_median:
        (map(.rapid_seeks.final_readiness_ms) | median),
      stale_visible_frames_total:
        (map(.rapid_seeks.stale_visible_frames) | add),
      process_file_bytes_median: (map(.process_physical.file_bytes) | median),
      logical_chunk_source_bytes_median:
        (map(.repository_metrics.logical_chunk_source_bytes) | median),
      sparse_retained_bytes_produced_median:
        (map(.repository_metrics.sparse_retained_bytes_produced) | median),
      peak_rss_bytes_median: (map(.peak_rss_bytes) | median),
      frame_offset_reads: (map(.repository_metrics.frame_offset_reads) | unique),
      derived_metric_payload_reads:
        (map(.repository_metrics.derived_metric_payload_reads) | unique),
      roi_image_open_attempts:
        (map(.repository_metrics.roi_image_open_attempts) | unique)
    },
    trials: .
  }
' "${results[@]}" > "$aggregate_partial"
mv "$aggregate_partial" "$output_dir/aggregate.json"

status="$(jq -er '.status' "$output_dir/aggregate.json")"
echo "Aggregate: $output_dir/aggregate.json"
[[ "$status" == "pass" ]]
