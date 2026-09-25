#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
redgui="${1:-$repo_root/release/redgui}"
zarr="${2:-/groups/johnson/johnsonlab/jeremy/recordings/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop/zarr/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop_analysis.zarr}"
output_dir="${3:-$repo_root/docs/reference/phase5m/linux}"
width="${4:-1920}"
height="${5:-1080}"
layout="${CRIMSON_PHASE5M_REFERENCE_IMGUI_INI:-$repo_root/docs/reference/phase5l/linux/arranged_reference_1920x1080.imgui.ini}"

if [[ ! -f "$layout" ]]; then
    echo "Maintained reference layout not found: $layout" >&2
    exit 2
fi

CRIMSON_REFERENCE_CAPTURE_CLASS=deterministic \
CRIMSON_REFERENCE_UI_STATE=polar \
CRIMSON_REFERENCE_FRAME=1024 \
CRIMSON_REFERENCE_UI_TIMEOUT="${CRIMSON_PHASE5M_REFERENCE_TIMEOUT:-180}" \
CRIMSON_REFERENCE_IMGUI_INI="$layout" \
"$repo_root/scripts/capture_redgui_workspace_reference.sh" \
    "$redgui" "$zarr" "$output_dir" \
    polar_exact_f001024_1920x1080 "$width" "$height"

echo "Linux polar reference captured in $output_dir"
