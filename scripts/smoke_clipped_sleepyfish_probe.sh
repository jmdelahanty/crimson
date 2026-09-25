#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

zarr_path="${1:-/groups/johnson/johnsonlab/jeremy/palette_smoke/sleepyfish_2026_05_05_17_45_30_cam2010093/zarr/sleepyfish_2026_05_05_17_45_30_cam2010093_analysis.zarr}"
collection_id="${CRIMSON_CLIPPED_COLLECTION_ID:-sleepyfish_cam2010093_allclips_pynvvc_fixed_20260518_01}"
build_jobs="${CRIMSON_BUILD_JOBS:-8}"
probe="${repo_root}/release/palette_clipped_loader_probe"

cmake --build "${repo_root}/build" --target palette_clipped_loader_probe -j "${build_jobs}"

"${probe}" "${zarr_path}" \
  --expect-collection "${collection_id}" \
  --expect-selected-runs 22 \
  --expect-mapped-frames 1188000 \
  --expect-total-parent-frames 1188000 \
  --expect-unselected-frame-pairs 0 \
  --expect-coordinates-normalized false \
  --expect-frame 0:clip_000000:0:1:1 \
  --expect-frame 53999:clip_000000:53999:54000:1 \
  --expect-frame 54000:clip_000001:0:54001:1 \
  --expect-frame 107999:clip_000001:53999:108000:1 \
  --expect-frame 108000:clip_000002:0:108001:1 \
  --expect-frame 1187999:clip_000021:53999:1188000:1
