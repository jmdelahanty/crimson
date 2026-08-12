#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${CRIMSON_BUILD_DIR:-$repo_root/build/macos-arm64-release}"
benchmark="$build_dir/subject_mask_v1_long_duration_benchmark"
bundle_probe="$build_dir/subject_mask_bundle_v4_probe"
crop_join_probe="$build_dir/subject_mask_crop_join_probe"

fixture_root="${CRIMSON_SUBJECT_MASK_BUNDLE_V4_FIXTURE_ROOT:-/Volumes/johnsonlab/jeremy/recordings/.palette_benchmarks/subject_mask_storage/full_duration/sleepyfish_receipt_composed_20260812_80e55b64_v1}"
store="$fixture_root/analysis.zarr"
receipt="$fixture_root/result.json"
receipt_sha256="8ccd4b1ce19e1ad632d48b8ef16dbbc2eb963170561a3fb4232dc944a6810a96"
bundle="subject_mask_bundle_sleepyfish_composable_modern_receipts_20260811_e523c816_v1"
bundle_digest="1404538de8e53503dbc39449ca9a3d2e60ba1e0cb71d7c54e3da7d062ecccc7f"
refined_run="refined_subject_masks_sleepyfish_composable_modern_receipts_20260811_e523c816_v1"
refined_digest="97ae241d3860031a03f02fb7b541060cb03b0bb8c7e3c50eeadfeb039ce1fa45"
cache_run="subject_mask_sampled_contours_sleepyfish_composable_modern_receipts_20260811_e523c816_v1"
cache_digest="844cd75cf13d6a3d1ab6ba43ebf7f885acb2c93181916b4acbb3c80fb3251fe6"
crop_run="crop_sleepyfish_cam2010095_full_v8_20260730"
workload="${CRIMSON_SUBJECT_MASK_BUNDLE_V4_WORKLOAD:-$repo_root/tools/fixtures/subject_mask_bundle_v4_qualification_workload_v1.json}"
output_dir="${CRIMSON_SUBJECT_MASK_BUNDLE_V4_OUTPUT_DIR:-/private/tmp/crimson-subject-mask-bundle-v4-$(date -u +%Y%m%dT%H%M%SZ)}"

for executable in "$benchmark" "$bundle_probe" "$crop_join_probe"; do
    if [[ ! -x "$executable" ]]; then
        echo "Required executable not found: $executable" >&2
        echo "Build the subject-mask qualification targets first." >&2
        exit 2
    fi
done
for input in "$store/zarr.json" "$receipt" "$workload"; do
    if [[ ! -r "$input" ]]; then
        echo "Required input is not readable: $input" >&2
        exit 2
    fi
done
for command in jq shasum sw_vers; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "$command is required for this qualification." >&2
        exit 2
    fi
done

actual_receipt_sha256="$(shasum -a 256 "$receipt" | awk '{print $1}')"
if [[ "$actual_receipt_sha256" != "$receipt_sha256" ]]; then
    echo "Palette result receipt SHA-256 mismatch." >&2
    exit 1
fi

repetitions="$(jq -er '.repetition_count | select(type == "number" and . > 0 and floor == .)' "$workload")"
if [[ "$repetitions" -ne 5 ]]; then
    echo "The frozen bundle-v4 workload must contain exactly five repetitions." >&2
    exit 1
fi
mkdir -p "$output_dir/trials"
if [[ -e "$output_dir/aggregate.json" ]]; then
    echo "Refusing to overwrite an existing qualification: $output_dir" >&2
    exit 2
fi

echo "Validating the exact selector-ineligible bundle"
"$bundle_probe" \
    --store "$store" \
    --bundle "$bundle" \
    --manifest-payload-digest "$bundle_digest" \
    > "$output_dir/bundle_probe.json"

# These failures prove that an explicit bad request does not silently select or
# fall back to another bundle.
if "$bundle_probe" \
    --store "$store" \
    --bundle "${bundle}_does_not_exist" \
    --manifest-payload-digest "$bundle_digest" \
    > "$output_dir/negative_wrong_bundle.json"; then
    echo "A nonexistent explicit bundle unexpectedly opened." >&2
    exit 1
fi
if "$bundle_probe" \
    --store "$store" \
    --bundle "$bundle" \
    --manifest-payload-digest "0000000000000000000000000000000000000000000000000000000000000000" \
    > "$output_dir/negative_wrong_digest.json"; then
    echo "An incorrect explicit bundle digest unexpectedly opened." >&2
    exit 1
fi

echo "Validating every refined-mask row against crop geometry"
"$crop_join_probe" \
    --store "$store" \
    --mask-run "$refined_run" \
    --crop-run "$crop_run" \
    > "$output_dir/crop_join_probe.json"

jq -n \
    --arg generated_at_utc "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    --arg architecture "$(uname -m)" \
    --arg kernel "$(uname -srv)" \
    --arg macos_version "$(sw_vers -productVersion)" \
    --arg fixture_root "$fixture_root" \
    --arg workload "$workload" \
    '{
      schema_id: "crimson.subject_mask.bundle_v4_environment",
      schema_version: 1,
      generated_at_utc: $generated_at_utc,
      architecture: $architecture,
      kernel: $kernel,
      macos_version: $macos_version,
      fixture_root: $fixture_root,
      workload: $workload,
      mount_cache_state: "uncontrolled_macos_and_network_filesystem_cache",
      tensorstore_cache_policy: "benchmark_workload_declared"
    }' > "$output_dir/environment.json"

trials=()
for ((repetition = 0; repetition < repetitions; ++repetition)); do
    if ((repetition % 2 == 0)); then
        modes=(sampled_contours dense_masks)
    else
        modes=(dense_masks sampled_contours)
    fi
    for mode in "${modes[@]}"; do
        output="$output_dir/trials/${mode}-$(printf '%03d' "$repetition").json"
        echo "Running repetition $((repetition + 1))/$repetitions mode=$mode"
        command=(
            "$benchmark"
            --store "$store"
            --run "$refined_run"
            --manifest-payload-digest "$refined_digest"
            --frame-size 4512x4512
            --workload "$workload"
            --repetition "$repetition"
            --output "$output"
        )
        if [[ "$mode" == "sampled_contours" ]]; then
            command+=(
                --presentation-cache-store "$store"
                --presentation-cache-run "$cache_run"
                --presentation-cache-manifest-payload-digest "$cache_digest"
            )
        fi
        trial_status=0
        "${command[@]}" || trial_status=$?
        if [[ "$trial_status" -ne 0 && "$trial_status" -ne 2 ]]; then
            echo "Benchmark process failed before producing a gate result." >&2
            exit "$trial_status"
        fi
        trials+=("$output")
    done
done

aggregate_partial="$output_dir/aggregate.partial.json"
jq -s \
    --slurpfile bundle_probe_result "$output_dir/bundle_probe.json" \
    --slurpfile crop_join_result "$output_dir/crop_join_probe.json" \
    --slurpfile environment "$output_dir/environment.json" \
    --arg receipt "$receipt" \
    --arg receipt_sha256 "$receipt_sha256" \
    --arg bundle_digest "$bundle_digest" '
  def median: sort | .[length / 2 | floor];
  def mode($name): map(select(.presentation_mode == $name));
  def mode_summary($name):
    mode($name) as $trials |
    {
      trial_count: ($trials | length),
      first_presentation_readiness_ms_median:
        ($trials | map(.first_presentation_readiness_ms) | median),
      warm_random_p95_ms_median:
        ($trials | map(.random_warm.p95_ms) | median),
      warm_random_p95_ms_max: ($trials | map(.random_warm.p95_ms) | max),
      forward_page_p95_ms_median:
        ($trials | map(.forward_traversal.page_p95_ms) | median),
      reverse_page_p95_ms_median:
        ($trials | map(.reverse_traversal.page_p95_ms) | median),
      deadline_miss_ratio_max:
        ($trials | map(.effective_deadline_miss_ratio) | max),
      current_frame_queue_max_ms:
        ($trials | map(.scheduler_metrics.timing_by_priority.current_frame.queue_maximum_ms) | max),
      rapid_seek_final_readiness_ms_max:
        ($trials | map(.rapid_seeks.final_readiness_ms) | max),
      stale_visible_frames_total:
        ($trials | map(.rapid_seeks.stale_visible_frames) | add),
      cancelled_requests_total:
        ($trials | map(.rapid_seeks.cancelled_requests) | add),
      process_file_reads_median:
        ($trials | map(.process_physical.file_reads) | median),
      process_file_bytes_median:
        ($trials | map(.process_physical.file_bytes) | median),
      peak_rss_bytes_max: ($trials | map(.peak_rss_bytes) | max),
      frame_offset_reads: ($trials | map(.repository_metrics.frame_offset_reads) | unique),
      dense_mask_payload_reads:
        ($trials | map(.repository_metrics.dense_mask_payload_reads) | unique),
      contour_payload_reads:
        ($trials | map(.repository_metrics.contour_payload_reads) | unique),
      source_point_count_open_attempts:
        ($trials | map(.repository_metrics.source_point_count_open_attempts) | unique),
      source_point_count_payload_reads:
        ($trials | map(.repository_metrics.source_point_count_payload_reads) | unique),
      derived_metric_payload_reads:
        ($trials | map(.repository_metrics.derived_metric_payload_reads) | unique),
      fixed_frame_statuses: ($trials | map(.fixed_frames.status) | unique)
    };
  (all(.[];
      .status == "pass" and
      .random_warm.p95_ms <= 150.0 and
      .effective_deadline_miss_ratio == 0.0 and
      .repository_metrics.frame_offset_reads == 1 and
      .rapid_seeks.stale_visible_frames == 0 and
      .repository_metrics.derived_metric_payload_reads == 0 and
      .peak_rss_bytes <= 2147483648 and
      .fixed_frames.status == "pass")) as $trials_pass |
  (($bundle_probe_result[0].status == "pass") and
   ($bundle_probe_result[0].manifest_payload_digest == $bundle_digest) and
   ($bundle_probe_result[0].selector_eligible == false) and
   ($bundle_probe_result[0].ordinary_selection_absent == true) and
   ($bundle_probe_result[0].quality_payload_opened == false) and
   ($bundle_probe_result[0].full_ragged_contours_present == false)) as $bundle_pass |
  (($crop_join_result[0].status == "pass") and
   ($crop_join_result[0].row_count == 1169010) and
   ($crop_join_result[0].instance_key_mismatches == 0) and
   ($crop_join_result[0].placement_mismatches == 0)) as $join_pass |
  {
    schema_id: "crimson.subject_mask.bundle_v4_qualification",
    schema_version: 1,
    status: (if $bundle_pass and $join_pass and $trials_pass then "pass" else "fail" end),
    classification: "selector_ineligible_full_duration_read_only_consumer_gate",
    profile_approved_for_production_path_candidate:
      ($bundle_pass and $join_pass and $trials_pass),
    production_activation_authorized: false,
    receipt: {path: $receipt, sha256: $receipt_sha256},
    environment: $environment[0],
    bundle_probe: $bundle_probe_result[0],
    crop_join_probe: $crop_join_result[0],
    fail_closed_checks: {
      wrong_bundle_rejected: true,
      wrong_digest_rejected: true
    },
    trial_count: length,
    repetitions_per_mode: 5,
    execution_order: map({repetition, presentation_mode}),
    crimson_commits: (map(.crimson_commit) | unique),
    worktree_dirty_values: (map(.worktree_dirty) | unique),
    workload_sha256_values: (map(.workload_sha256) | unique),
    quality_payload_reads_during_presentation: 0,
    summaries: {
      sampled_contours: mode_summary("sampled_contours"),
      dense_masks: mode_summary("dense_masks")
    },
    trials: .
  }
' "${trials[@]}" > "$aggregate_partial"
mv "$aggregate_partial" "$output_dir/aggregate.json"

(
    cd "$output_dir"
    shasum -a 256 aggregate.json > aggregate.sha256
)
status="$(jq -er '.status' "$output_dir/aggregate.json")"
echo "Aggregate: $output_dir/aggregate.json"
echo "SHA-256:  $(awk '{print $1}' "$output_dir/aggregate.sha256")"
[[ "$status" == "pass" ]]
