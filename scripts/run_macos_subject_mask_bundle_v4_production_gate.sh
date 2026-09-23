#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
recording_root="${CRIMSON_SUBJECT_MASK_PRODUCTION_RECORDING_ROOT:-/Volumes/johnsonlab/jeremy/recordings/sleepyfish_2026_05_05_17_45_30_cam2010095}"
store="$recording_root/zarr/sleepyfish_2026_05_05_17_45_30_cam2010095_analysis.zarr"
receipt_root="$recording_root/.processing_logs/analysis_workflows/subject_mask_bundle_v4_production_candidate_20260812"

bundle_import_sha256="51f63bbbd5fbe90aa6c494e45771b5ccde5e8f10661d10b56b2a0d340ca96db9"
bundle_validation_sha256="10c1a501bf8d3d9478741bc0fbee64d43115317f743331442d36bdaccddc517f"
lineage_import_sha256="ffebf86c6b47661d96311e880c2f008d870b2f5f43df3bf2db484c7433dde349"

verify_receipt() {
    receipt="$1"
    expected="$2"
    path="$receipt_root/$receipt"
    if [[ ! -r "$path" ]]; then
        echo "Palette production receipt is not readable: $path" >&2
        exit 2
    fi
    actual="$(/usr/bin/shasum -a 256 "$path" | awk '{print $1}')"
    if [[ "$actual" != "$expected" ]]; then
        echo "Palette production receipt digest mismatch: $receipt" >&2
        exit 1
    fi
}

verify_receipt bundle_import.json "$bundle_import_sha256"
verify_receipt bundle_validation.json "$bundle_validation_sha256"
verify_receipt lineage_import.json "$lineage_import_sha256"

export CRIMSON_SUBJECT_MASK_BUNDLE_V4_STORE="$store"
export CRIMSON_SUBJECT_MASK_BUNDLE_V4_FIXTURE_ROOT="$recording_root"
export CRIMSON_SUBJECT_MASK_BUNDLE_V4_RECEIPT="$receipt_root/bundle_validation.json"
export CRIMSON_SUBJECT_MASK_BUNDLE_V4_RECEIPT_SHA256="$bundle_validation_sha256"
export CRIMSON_SUBJECT_MASK_BUNDLE_V4_BUNDLE="subject_mask_bundle_sleepyfish_composable_modern_receipts_20260811_e523c816_v1"
export CRIMSON_SUBJECT_MASK_BUNDLE_V4_BUNDLE_DIGEST="582652a82631ceb41004c9612cfb6ad5426506a9c6c31de8fa007ab9b70f3f79"
export CRIMSON_SUBJECT_MASK_BUNDLE_V4_REFINED_RUN="refined_subject_masks_sleepyfish_composable_modern_receipts_20260811_e523c816_v1"
export CRIMSON_SUBJECT_MASK_BUNDLE_V4_REFINED_DIGEST="8fb8bd4ddb3ee586813d51439f17a26cd03c6f2845455ce63cda6d67bbdb34c9"
export CRIMSON_SUBJECT_MASK_BUNDLE_V4_CACHE_RUN="subject_mask_sampled_contours_sleepyfish_composable_modern_receipts_20260811_e523c816_v1"
export CRIMSON_SUBJECT_MASK_BUNDLE_V4_CACHE_DIGEST="38a77478755e5442c8004d7d53a075fde15d12c5e9cff87b8ef2b6f28b4ddf0f"
export CRIMSON_SUBJECT_MASK_BUNDLE_V4_CROP_RUN="crop_sleepyfish_cam2010095_full_v8_20260730"
export CRIMSON_SUBJECT_MASK_BUNDLE_V4_OUTPUT_DIR="${CRIMSON_SUBJECT_MASK_BUNDLE_V4_OUTPUT_DIR:-$repo_root/docs/diagnostics/subject_mask_bundle_v4_production_location_2026-08-12}"

exec "$repo_root/scripts/run_macos_subject_mask_bundle_v4_qualification.sh"
