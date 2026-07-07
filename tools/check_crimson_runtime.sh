#!/usr/bin/env bash
set -u -o pipefail

app_root=""
require_nvidia_smi=0
require_gl=0
has_fail=0
ok_count=0
warn_count=0
fail_count=0

usage() {
    cat <<'EOF'
Usage: check_crimson_runtime.sh [options]

Options:
  --app-root PATH       Crimson app root. Defaults to the script directory.
  --require-nvidia-smi  Fail instead of warn when nvidia-smi is unavailable.
  --require-gl          Fail instead of warn when no X/OpenGL probe succeeds.
  -h, --help            Show this help.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --app-root)
            app_root="${2:-}"
            shift 2
            ;;
        --require-nvidia-smi)
            require_nvidia_smi=1
            shift
            ;;
        --require-gl)
            require_gl=1
            shift
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

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [ -z "$app_root" ]; then
    app_root="$script_dir"
fi
if [ -d "$app_root" ]; then
    app_root="$(cd -- "$app_root" && pwd)"
fi

add_result() {
    local category="$1"
    local name="$2"
    local status="$3"
    local details="$4"

    case "$status" in
        OK) ok_count=$((ok_count + 1)) ;;
        WARN) warn_count=$((warn_count + 1)) ;;
        FAIL)
            fail_count=$((fail_count + 1))
            has_fail=1
            ;;
    esac

    printf '%-10s %-30s %-5s %s\n' "$category" "$name" "$status" "$details"
}

add_ok() { add_result "$1" "$2" "OK" "$3"; }
add_warn() { add_result "$1" "$2" "WARN" "$3"; }
add_fail() { add_result "$1" "$2" "FAIL" "$3"; }

require_path() {
    local category="$1"
    local name="$2"
    local path_value="$3"
    if [ -e "$path_value" ]; then
        add_ok "$category" "$name" "$path_value"
        return 0
    fi
    add_fail "$category" "$name" "missing: $path_value"
    return 1
}

check_optional_path() {
    local category="$1"
    local name="$2"
    local path_value="$3"
    if [ -e "$path_value" ]; then
        add_ok "$category" "$name" "$path_value"
    else
        add_warn "$category" "$name" "missing: $path_value"
    fi
}

echo ""
echo "Crimson Linux runtime check:"
echo "  App root: $app_root"
echo ""
printf '%-10s %-30s %-5s %s\n' "Category" "Name" "Stat" "Details"
printf '%-10s %-30s %-5s %s\n' "--------" "----" "----" "-------"

bin_dir="$app_root/bin"
share_root="$app_root/share/crimson"
redgui="$bin_dir/redgui"
launcher="$bin_dir/crimson"
fonts_dir="$share_root/fonts"
config_dir="$share_root/config"
release_json="$app_root/release.json"
readme="$app_root/README.txt"

require_path "app" "App root" "$app_root" || true
require_path "app" "Crimson executable" "$redgui" || true
if [ -e "$redgui" ] && [ ! -x "$redgui" ]; then
    add_fail "app" "Executable bit" "not executable: $redgui"
elif [ -x "$redgui" ]; then
    add_ok "app" "Executable bit" "$redgui"
fi
check_optional_path "app" "Launcher" "$launcher"
check_optional_path "app" "README" "$readme"
require_path "app" "Fonts directory" "$fonts_dir" || true
require_path "app" "Config directory" "$config_dir" || true

if [ -r "$release_json" ]; then
    if command -v python3 >/dev/null 2>&1; then
        release_summary="$(python3 - "$release_json" <<'PY' 2>/dev/null
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    data = json.load(f)

parts = []
for key in ("release_name", "preset", "commit_short", "built_at_utc"):
    value = data.get(key)
    if value:
        parts.append(f"{key}={value}")
print(" | ".join(parts) if parts else "present")
PY
)"
        if [ -n "$release_summary" ]; then
            add_ok "metadata" "Release metadata" "$release_summary"
        else
            add_warn "metadata" "Release metadata" "unreadable summary: $release_json"
        fi
    else
        add_ok "metadata" "Release metadata" "$release_json"
    fi
else
    add_warn "metadata" "Release metadata" "missing or unreadable: $release_json"
fi

if [ -x "$redgui" ]; then
    if command -v readelf >/dev/null 2>&1; then
        rpath_summary="$(readelf -d "$redgui" 2>/dev/null | grep -E 'RPATH|RUNPATH' | sed 's/^[[:space:]]*//' | paste -sd ';' -)"
        if [ -n "$rpath_summary" ]; then
            add_ok "linking" "RPATH/RUNPATH" "$rpath_summary"
            absolute_rpath="$(printf '%s\n' "$rpath_summary" | grep -Eo '(/opt|/usr/local|/home)[^]:; ]*' | sort -u | paste -sd ';' -)"
            if [ -n "$absolute_rpath" ]; then
                add_warn "linking" "Absolute RUNPATH" "$absolute_rpath"
            fi
        else
            add_warn "linking" "RPATH/RUNPATH" "no RPATH/RUNPATH recorded"
        fi
    else
        add_warn "linking" "readelf" "not found"
    fi

    if command -v ldd >/dev/null 2>&1; then
        ldd_output="$(ldd "$redgui" 2>&1)"
        if printf '%s\n' "$ldd_output" | grep -q "not found"; then
            missing="$(printf '%s\n' "$ldd_output" | grep "not found" | paste -sd ';' -)"
            add_fail "linking" "ldd unresolved" "$missing"
        else
            add_ok "linking" "ldd unresolved" "none"
        fi

        key_libs="$(printf '%s\n' "$ldd_output" | grep -E 'lib(nvinfer|opencv|avcodec|avformat|avutil|swscale|swresample|cudart|npp|cuda|GLEW|glfw|hdf5)' | sed 's/^[[:space:]]*//' | head -n 20 | paste -sd ';' -)"
        if [ -n "$key_libs" ]; then
            add_ok "linking" "Key libraries" "$key_libs"
        else
            add_warn "linking" "Key libraries" "no expected GPU/video/data libraries found in ldd output"
        fi
    else
        add_warn "linking" "ldd" "not found"
    fi
fi

if command -v nvidia-smi >/dev/null 2>&1; then
    gpu_summary="$(nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>&1)"
    nvidia_status=$?
    gpu_summary="$(printf '%s\n' "$gpu_summary" | paste -sd ';' -)"
    if [ "$nvidia_status" -eq 0 ] && [ -n "$gpu_summary" ]; then
        add_ok "nvidia" "nvidia-smi" "$gpu_summary"
    else
        if [ -z "$gpu_summary" ]; then
            gpu_summary="query failed"
        fi
        if [ "$require_nvidia_smi" -eq 1 ]; then
            add_fail "nvidia" "nvidia-smi" "$gpu_summary"
        else
            add_warn "nvidia" "nvidia-smi" "$gpu_summary"
        fi
    fi
else
    if [ "$require_nvidia_smi" -eq 1 ]; then
        add_fail "nvidia" "nvidia-smi" "not found in PATH"
    else
        add_warn "nvidia" "nvidia-smi" "not found in PATH"
    fi
fi

if [ -n "${DISPLAY:-}" ]; then
    if command -v glxinfo >/dev/null 2>&1 && glxinfo -B >/dev/null 2>&1; then
        renderer="$(glxinfo -B 2>/dev/null | awk -F: '/OpenGL renderer string/ { sub(/^[ \t]+/, "", $2); print $2; exit }')"
        add_ok "display" "OpenGL probe" "DISPLAY=$DISPLAY renderer=${renderer:-unknown}"
    elif command -v xdpyinfo >/dev/null 2>&1 && xdpyinfo >/dev/null 2>&1; then
        add_warn "display" "OpenGL probe" "xdpyinfo works for DISPLAY=$DISPLAY, but glxinfo is unavailable or failed"
    else
        if [ "$require_gl" -eq 1 ]; then
            add_fail "display" "OpenGL probe" "DISPLAY=$DISPLAY but no GL/X probe succeeded"
        else
            add_warn "display" "OpenGL probe" "DISPLAY=$DISPLAY but no GL/X probe succeeded"
        fi
    fi
else
    if [ "$require_gl" -eq 1 ]; then
        add_fail "display" "DISPLAY" "not set"
    else
        add_warn "display" "DISPLAY" "not set"
    fi
fi

echo ""
echo "Summary: OK=$ok_count WARN=$warn_count FAIL=$fail_count"

if [ "$has_fail" -ne 0 ]; then
    exit 1
fi
