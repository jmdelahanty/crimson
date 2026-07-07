#!/usr/bin/env bash
set -u -o pipefail

app_root=""
mode="dev"
write_dependency_manifest=""
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
  --mode dev|release    Check policy. Dev warns on absolute private roots;
                        release fails. Default: dev.
  --write-dependency-manifest PATH
                        Write a JSON dependency audit manifest.
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
        --mode)
            mode="${2:-}"
            shift 2
            ;;
        --write-dependency-manifest)
            write_dependency_manifest="${2:-}"
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

case "$mode" in
    dev|release) ;;
    *)
        echo "--mode must be 'dev' or 'release', got: $mode" >&2
        exit 2
        ;;
esac

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

runtime_search_path=""

append_runtime_search_path() {
    local path_value="$1"
    if [ -z "$path_value" ]; then
        return
    fi
    if [ -d "$path_value" ]; then
        path_value="$(cd -- "$path_value" && pwd)"
    fi
    case ":$runtime_search_path:" in
        *":$path_value:"*) return ;;
    esac
    if [ -n "$runtime_search_path" ]; then
        runtime_search_path="$runtime_search_path:$path_value"
    else
        runtime_search_path="$path_value"
    fi
}

build_release_ld_library_path() {
    local allowed_root
    local old_ifs

    runtime_search_path=""
    append_runtime_search_path "$app_root/lib"
    append_runtime_search_path "$app_root/lib64"
    append_runtime_search_path "$app_root/lib/crimson/private"
    append_runtime_search_path "$app_root/lib64/crimson/private"

    old_ifs="$IFS"
    IFS=:
    for allowed_root in ${CRIMSON_ALLOWED_RUNTIME_ROOTS:-}; do
        append_runtime_search_path "$allowed_root"
    done
    IFS="$old_ifs"

    printf '%s\n' "$runtime_search_path"
}

run_dependency_audit() {
    local ldd_file="$1"
    local runtime_search_policy="$2"
    local runtime_ld_library_path="$3"

    if ! command -v python3 >/dev/null 2>&1; then
        if [ -n "$write_dependency_manifest" ]; then
            add_fail "audit" "Dependency manifest" "python3 is required to write $write_dependency_manifest"
        else
            add_warn "audit" "Dependency audit" "python3 not found; structured audit skipped"
        fi
        return
    fi

    local audit_output
    audit_output="$(python3 - "$mode" "$write_dependency_manifest" "$app_root" "$release_json" "$ldd_file" "$runtime_search_policy" "$runtime_ld_library_path" <<'PY'
import collections
import datetime as _datetime
import json
import os
import re
import sys

mode, manifest_path, app_root, release_json, ldd_file, runtime_search_policy, runtime_ld_library_path = sys.argv[1:8]
app_root = os.path.realpath(app_root)

def clean(value):
    return str(value).replace("\t", " ").replace("\n", " ").strip()

def emit(status, name, details):
    print(f"{status}\t{clean(name)}\t{clean(details)}")

def split_env_paths(name):
    value = os.environ.get(name, "")
    return [os.path.realpath(p) for p in value.split(":") if p]

def is_under(path, roots):
    if not path:
        return False
    real = os.path.realpath(path)
    for root in roots:
        root = os.path.realpath(root)
        try:
            if os.path.commonpath([real, root]) == root:
                return True
        except ValueError:
            pass
    return False

def path_dir(path):
    if path and path.startswith("/"):
        return os.path.dirname(os.path.realpath(path))
    return ""

def parse_ldd(text):
    entries = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        soname = ""
        resolved = ""
        missing = False
        if "=>" in line:
            left, right = line.split("=>", 1)
            soname = left.strip()
            right = right.strip()
            if right.startswith("not found"):
                missing = True
                resolved = "not found"
            else:
                match = re.match(r"(\S+)", right)
                if match:
                    resolved = match.group(1)
        else:
            first = line.split()[0]
            if first.startswith("/"):
                resolved = first
                soname = os.path.basename(first)
            else:
                soname = first
        if not soname:
            soname = os.path.basename(resolved) if resolved else line
        entries.append({
            "soname": soname,
            "resolved_path": resolved,
            "missing": missing,
            "raw": line,
        })
    return entries

driver_prefixes = (
    "libcuda.so",
    "libnvidia",
    "libnvcuvid",
    "libnvoptix",
    "libnvencodeapi",
)
bundle_prefixes = (
    "libnvinfer",
    "libnvonnxparser",
    "libopencv",
    "libavcodec",
    "libavformat",
    "libavutil",
    "libswscale",
    "libswresample",
    "libcudart",
    "libnpp",
    "libcublas",
    "libcufft",
    "libcurand",
    "libcusolver",
    "libcusparse",
    "libnvrtc",
    "libOpenCL",
)
system_prefixes = (
    "ld-linux",
    "linux-vdso",
    "libanl",
    "libblkid",
    "libbrotli",
    "libbsd",
    "libbz2",
    "libc.so",
    "libcom_err",
    "libcrypto",
    "libcurl",
    "libdbus",
    "libdl",
    "libdrm",
    "libexpat",
    "libffi",
    "libfontconfig",
    "libfreetype",
    "libgcc_s",
    "libgcrypt",
    "libgfortran",
    "libgmp",
    "libgnutls",
    "libgpg-error",
    "libgssapi",
    "libharfbuzz",
    "libhdf5_serial",
    "libhogweed",
    "libidn",
    "libjpeg",
    "libkeyutils",
    "libkrb5",
    "liblber",
    "libldap",
    "liblz4",
    "liblzma",
    "libm.so",
    "libmd",
    "libmount",
    "libnettle",
    "libnghttp",
    "libp11-kit",
    "libpcre",
    "libpng",
    "libpsl",
    "libpthread",
    "libquadmath",
    "libresolv",
    "librtmp",
    "librt",
    "libsasl",
    "libselinux",
    "libssh",
    "libssl",
    "libstdc++",
    "libsystemd",
    "libtasn1",
    "libtiff",
    "libunistring",
    "libuuid",
    "libwebp",
    "libX",
    "libxcb",
    "libz.",
    "libzstd",
    "libGL",
    "libEGL",
    "libOpenGL",
    "libGLdispatch",
)
system_roots = tuple(os.path.realpath(p) for p in (
    "/lib",
    "/lib64",
    "/usr/lib",
    "/usr/lib64",
    "/usr/lib/x86_64-linux-gnu",
))
allowed_roots = [app_root] + split_env_paths("CRIMSON_ALLOWED_RUNTIME_ROOTS")

def classify(soname):
    if soname.startswith(driver_prefixes):
        return "driver"
    if soname.startswith(bundle_prefixes):
        return "bundle-candidate"
    if soname.startswith(system_prefixes):
        return "system"
    return "unexpected"

def group_for(soname):
    if soname.startswith("libopencv"):
        return "OpenCV"
    if soname.startswith(("libavcodec", "libavformat", "libavutil", "libswscale", "libswresample")):
        return "FFmpeg"
    if soname.startswith(("libnvinfer", "libnvonnxparser")):
        return "TensorRT"
    if soname.startswith(("libcudart", "libnpp", "libcublas", "libcufft", "libcurand", "libcusolver", "libcusparse", "libnvrtc")):
        return "CUDA runtime"
    return ""

def release_allowed(entry):
    path = entry["resolved_path"]
    classification = entry["classification"]
    if entry["missing"]:
        return False
    if classification == "driver":
        return True
    if classification == "system":
        return not path or is_under(path, system_roots)
    if classification == "bundle-candidate":
        return is_under(path, allowed_roots)
    return False

def expected_prefixes(metadata):
    prefixes = collections.defaultdict(list)
    def add(group, path):
        if path:
            prefixes[group].append(os.path.realpath(path))
    opencv_dir = metadata.get("opencv_dir", "")
    if opencv_dir:
        add("OpenCV", opencv_dir)
        marker = "/lib/cmake/opencv4"
        if opencv_dir.endswith(marker):
            root = opencv_dir[:-len(marker)]
            add("OpenCV", root)
            add("OpenCV", os.path.join(root, "lib"))
    ffmpeg_root = metadata.get("ffmpeg_root", "")
    if ffmpeg_root:
        add("FFmpeg", ffmpeg_root)
        add("FFmpeg", os.path.join(ffmpeg_root, "lib"))
    tensorrt_root = metadata.get("tensorrt_root", "")
    if tensorrt_root:
        add("TensorRT", tensorrt_root)
        add("TensorRT", os.path.join(tensorrt_root, "lib"))
    cuda_root = metadata.get("cuda_toolkit_root", "") or metadata.get("cudatoolkit_root", "")
    if cuda_root:
        add("CUDA runtime", cuda_root)
        add("CUDA runtime", os.path.join(cuda_root, "lib64"))
    return prefixes

with open(ldd_file, "r", encoding="utf-8", errors="replace") as f:
    entries = parse_ldd(f.read())

metadata = {}
if release_json and os.path.exists(release_json):
    try:
        with open(release_json, "r", encoding="utf-8") as f:
            metadata = json.load(f)
    except Exception:
        metadata = {}

for entry in entries:
    entry["classification"] = classify(entry["soname"])
    if entry["classification"] == "unexpected" and is_under(entry["resolved_path"], system_roots):
        entry["classification"] = "system"
    entry["group"] = group_for(entry["soname"])
    entry["owning_root"] = path_dir(entry["resolved_path"])
    entry["allowed_in_release"] = release_allowed(entry)

counts = collections.Counter(e["classification"] for e in entries)
emit("OK", "Dependency classes", ", ".join(f"{key}={counts[key]}" for key in sorted(counts)))

missing = [e for e in entries if e["missing"]]
if missing:
    emit("FAIL", "Missing dependencies", "; ".join(e["soname"] for e in missing[:12]))

external_bundle = [
    e for e in entries
    if e["classification"] == "bundle-candidate" and not e["allowed_in_release"]
]
if external_bundle:
    status = "FAIL" if mode == "release" else "WARN"
    detail = "; ".join(f"{e['soname']} -> {e['resolved_path']}" for e in external_bundle[:10])
    if len(external_bundle) > 10:
        detail += f"; +{len(external_bundle) - 10} more"
    emit(status, "External private deps", detail)

unexpected = [
    e for e in entries
    if e["classification"] == "unexpected" and e["resolved_path"] and e["resolved_path"] != "not found"
]
if unexpected:
    status = "FAIL" if mode == "release" else "WARN"
    detail = "; ".join(f"{e['soname']} -> {e['resolved_path']}" for e in unexpected[:10])
    if len(unexpected) > 10:
        detail += f"; +{len(unexpected) - 10} more"
    emit(status, "Unexpected deps", detail)

roots_by_group = collections.defaultdict(set)
for entry in entries:
    if entry["group"] and entry["owning_root"]:
        roots_by_group[entry["group"]].add(entry["owning_root"])

for group in ("OpenCV", "FFmpeg", "TensorRT", "CUDA runtime"):
    roots = sorted(roots_by_group.get(group, ()))
    if not roots:
        continue
    if len(roots) == 1:
        emit("OK", f"{group} root", roots[0])
    else:
        status = "FAIL" if mode == "release" else "WARN"
        emit(status, f"{group} mixed roots", "; ".join(roots))

prefixes = expected_prefixes(metadata)
for group, expected in sorted(prefixes.items()):
    group_entries = [e for e in entries if e["group"] == group and e["resolved_path"] and e["resolved_path"] != "not found"]
    mismatches = [
        e for e in group_entries
        if not is_under(e["resolved_path"], expected) and not is_under(e["resolved_path"], allowed_roots)
    ]
    if mismatches:
        status = "FAIL" if mode == "release" else "WARN"
        detail = "; ".join(f"{e['soname']} -> {e['resolved_path']}" for e in mismatches[:8])
        emit(status, f"{group} expected root", detail)

if manifest_path:
    manifest_dir = os.path.dirname(os.path.abspath(manifest_path))
    if manifest_dir:
        os.makedirs(manifest_dir, exist_ok=True)
    manifest = {
        "schema_id": "crimson_linux_dependency_manifest_v1",
        "generated_at_utc": _datetime.datetime.now(_datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "mode": mode,
        "app_root": app_root,
        "runtime_search_policy": runtime_search_policy,
        "runtime_ld_library_path": runtime_ld_library_path,
        "release_metadata": metadata,
        "allowed_runtime_roots": allowed_roots,
        "dependencies": entries,
    }
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
        f.write("\n")
    emit("OK", "Dependency manifest", manifest_path)
PY
)"
    local audit_status=$?
    if [ "$audit_status" -ne 0 ]; then
        add_fail "audit" "Dependency audit" "python3 audit failed with exit code $audit_status"
        return
    fi

    while IFS=$'\t' read -r status name details; do
        [ -n "$status" ] || continue
        case "$status" in
            OK) add_ok "audit" "$name" "$details" ;;
            WARN) add_warn "audit" "$name" "$details" ;;
            FAIL) add_fail "audit" "$name" "$details" ;;
            *) add_warn "audit" "Unknown audit result" "$status $name $details" ;;
        esac
    done <<< "$audit_output"
}

echo ""
echo "Crimson Linux runtime check:"
echo "  App root: $app_root"
echo "  Mode:     $mode"
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
        runtime_search_policy=""
        runtime_ld_library_path=""
        if [ "$mode" = "release" ]; then
            runtime_search_policy="release sanitized LD_LIBRARY_PATH"
            runtime_ld_library_path="$(build_release_ld_library_path)"
            if [ -n "$runtime_ld_library_path" ]; then
                add_ok "linking" "Runtime search policy" "$runtime_search_policy=$runtime_ld_library_path"
                ldd_output="$(env LD_LIBRARY_PATH="$runtime_ld_library_path" ldd "$redgui" 2>&1)"
            else
                add_ok "linking" "Runtime search policy" "$runtime_search_policy=<unset>"
                ldd_output="$(env -u LD_LIBRARY_PATH ldd "$redgui" 2>&1)"
            fi
        else
            runtime_search_policy="dev inherited LD_LIBRARY_PATH"
            runtime_ld_library_path="${LD_LIBRARY_PATH:-}"
            if [ -n "$runtime_ld_library_path" ]; then
                add_ok "linking" "Runtime search policy" "$runtime_search_policy=$runtime_ld_library_path"
                if printf '%s\n' "$runtime_ld_library_path" | grep -Eq '(^|:)(/opt|/usr/local|/home)'; then
                    add_warn "linking" "Inherited LD_LIBRARY_PATH" "developer paths can mask release packaging issues"
                fi
            else
                add_ok "linking" "Runtime search policy" "$runtime_search_policy=<unset>"
            fi
            ldd_output="$(ldd "$redgui" 2>&1)"
        fi
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

        ldd_file="$(mktemp)"
        printf '%s\n' "$ldd_output" > "$ldd_file"
        run_dependency_audit "$ldd_file" "$runtime_search_policy" "$runtime_ld_library_path"
        rm -f "$ldd_file"
    else
        add_warn "linking" "ldd" "not found"
        if [ -n "$write_dependency_manifest" ]; then
            add_fail "audit" "Dependency manifest" "ldd is required to write $write_dependency_manifest"
        fi
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
