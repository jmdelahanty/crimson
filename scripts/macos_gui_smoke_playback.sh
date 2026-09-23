#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
app="${CRIMSON_MACOS_APP:-$repo_root/build/macos-arm64-release/Crimson.app/Contents/MacOS/Crimson}"
video="${1:-${CRIMSON_MACOS_PLAYBACK_SMOKE_VIDEO:-}}"
smoke_range="${2:-${CRIMSON_MACOS_PLAYBACK_SMOKE_RANGE:-0:300}}"
timestamp="$(date +%Y%m%d_%H%M%S)"
log_path="${CRIMSON_MACOS_PLAYBACK_SMOKE_LOG:-/tmp/crimson_macos_playback_smoke_${timestamp}.log}"

if [[ ! -x "$app" ]]; then
    echo "Crimson macOS app executable not found: $app" >&2
    echo "Build it with: cmake --build --preset build-macos-arm64-release" >&2
    exit 2
fi

if [[ -z "$video" ]]; then
    echo "Pass a video path or set CRIMSON_MACOS_PLAYBACK_SMOKE_VIDEO." >&2
    exit 2
fi

if [[ ! -f "$video" ]]; then
    echo "Playback smoke video not found: $video" >&2
    exit 2
fi

set +e
"$app" --video "$video" --video-smoke "$smoke_range" \
    >"$log_path" 2>&1
status=$?
set -e

if [[ "$status" -ne 0 ]]; then
    echo "macOS playback smoke failed with status $status" >&2
    echo "log: $log_path" >&2
    tail -n 120 "$log_path" >&2 || true
    exit "$status"
fi

if ! grep -Fq "[AppleVideoSmoke] PASS" "$log_path"; then
    echo "macOS playback smoke did not emit its PASS record" >&2
    echo "log: $log_path" >&2
    tail -n 120 "$log_path" >&2 || true
    exit 1
fi

tail -n 3 "$log_path"
echo "macOS playback smoke passed"
echo "log: $log_path"
