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
append_runtime_path() {
    local candidate="$1"
    local existing

    if [ -d "$candidate" ]; then
        candidate="$(cd -- "$candidate" && pwd)"
    else
        return
    fi

    for existing in "${runtime_paths[@]}"; do
        if [ "$existing" = "$candidate" ]; then
            return
        fi
    done

    runtime_paths+=("$candidate")
}

for candidate in \
    "$app_root/lib" \
    "$app_root/lib64" \
    "$app_root/lib/crimson/private" \
    "$app_root/lib64/crimson/private"; do
    append_runtime_path "$candidate"
done

runtime_roots_file="$app_root/etc/crimson/runtime_roots.env"
if [ -r "$runtime_roots_file" ]; then
    while IFS= read -r candidate || [ -n "$candidate" ]; do
        case "$candidate" in
            ""|\#*) continue ;;
        esac
        append_runtime_path "$candidate"
    done < "$runtime_roots_file"
fi

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
