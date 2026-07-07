#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

app_root="${CRIMSON_BUNDLED_APP_ROOT:-/tmp/crimson-linux-app-publish-nvidia-bundle/current}"
redgui="${CRIMSON_BUNDLED_REDGUI:-$app_root/bin/redgui}"
zarr_path="${CRIMSON_BUNDLED_PLAYBACK_SMOKE_ZARR:-/groups/johnson/johnsonlab/jeremy/recordings/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop/zarr/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop_analysis.zarr/}"
smoke_range="${CRIMSON_BUNDLED_PLAYBACK_SMOKE_RANGE:-0:300}"
timestamp="$(date +%Y%m%d_%H%M%S)"
log_path="${CRIMSON_BUNDLED_PLAYBACK_SMOKE_LOG:-/tmp/crimson_playback_smoke_bundled_${timestamp}.log}"

usage() {
    cat <<'EOF'
Usage: gui_smoke_bundled_playback.sh [options]

Runs the standard GUI playback smoke against a staged/published Crimson app
drop by exporting the needed CRIMSON_* variables for the child process.

Options:
  --app-root PATH       App drop root. Default:
                        /tmp/crimson-linux-app-publish-nvidia-bundle/current
  --redgui PATH         redgui executable. Default: <app-root>/bin/redgui
  --zarr PATH           Analysis Zarr to open. Default: GoodCopBadCop arena 1.
  --range START:END     Playback smoke frame range. Default: 0:300.
  --log PATH            Smoke log path. Default:
                        /tmp/crimson_playback_smoke_bundled_<timestamp>.log
  -h, --help            Show this help.

Environment overrides:
  CRIMSON_BUNDLED_APP_ROOT
  CRIMSON_BUNDLED_REDGUI
  CRIMSON_BUNDLED_PLAYBACK_SMOKE_ZARR
  CRIMSON_BUNDLED_PLAYBACK_SMOKE_RANGE
  CRIMSON_BUNDLED_PLAYBACK_SMOKE_LOG
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --app-root)
            app_root="${2:-}"
            redgui="$app_root/bin/redgui"
            shift 2
            ;;
        --redgui)
            redgui="${2:-}"
            shift 2
            ;;
        --zarr)
            zarr_path="${2:-}"
            shift 2
            ;;
        --range)
            smoke_range="${2:-}"
            shift 2
            ;;
        --log)
            log_path="${2:-}"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [ -z "$redgui" ] || [ ! -x "$redgui" ]; then
    echo "redgui executable not found: $redgui" >&2
    exit 2
fi

echo "Bundled Crimson playback smoke"
echo "  redgui: $redgui"
echo "  zarr:   $zarr_path"
echo "  range:  $smoke_range"
echo "  log:    $log_path"

CRIMSON_REDGUI="$redgui" \
CRIMSON_PLAYBACK_SMOKE_ZARR="$zarr_path" \
CRIMSON_PLAYBACK_SMOKE_RANGE="$smoke_range" \
CRIMSON_PLAYBACK_SMOKE_LOG="$log_path" \
"$repo_root/scripts/gui_smoke_playback.sh"
