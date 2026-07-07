#!/usr/bin/env bash
set -euo pipefail

script_path="${BASH_SOURCE[0]}"
while [ -L "$script_path" ]; do
    script_dir="$(cd -- "$(dirname -- "$script_path")" && pwd)"
    link_target="$(readlink -- "$script_path")"
    if [[ "$link_target" = /* ]]; then
        script_path="$link_target"
    else
        script_path="$script_dir/$link_target"
    fi
done

bin_dir="$(cd -- "$(dirname -- "$script_path")" && pwd)"
app_root="$(cd -- "$bin_dir/.." && pwd)"
redgui="$bin_dir/redgui"

if [ ! -x "$redgui" ]; then
    echo "Crimson launcher error: missing executable: $redgui" >&2
    exit 1
fi

runtime_paths=()
for candidate in \
    "$app_root/lib" \
    "$app_root/lib64" \
    "$app_root/lib/crimson/private" \
    "$app_root/lib64/crimson/private"; do
    if [ -d "$candidate" ]; then
        runtime_paths+=("$candidate")
    fi
done

if [ -n "${CRIMSON_EXTRA_LD_LIBRARY_PATH:-}" ]; then
    runtime_paths+=("$CRIMSON_EXTRA_LD_LIBRARY_PATH")
fi

if [ "${#runtime_paths[@]}" -gt 0 ]; then
    joined_paths="$(IFS=:; printf '%s' "${runtime_paths[*]}")"
    if [ "${CRIMSON_LINUX_STRICT_RUNTIME:-0}" = "1" ]; then
        export LD_LIBRARY_PATH="$joined_paths"
    elif [ -n "${LD_LIBRARY_PATH:-}" ]; then
        export LD_LIBRARY_PATH="$joined_paths:$LD_LIBRARY_PATH"
    else
        export LD_LIBRARY_PATH="$joined_paths"
    fi
fi

exec "$redgui" "$@"
