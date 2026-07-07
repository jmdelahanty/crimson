#!/usr/bin/env bash
set -Eeuo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"

preset="linux-trt10-cuda12.4-release"
install_prefix="dist/Crimson"
build_dir=""
jobs="${CRIMSON_BUILD_JOBS:-}"
skip_submodules=0
skip_configure=0
skip_build=0
skip_install=0
skip_runtime_check=0
runtime_check_mode="dev"
dependency_manifest=""
require_nvidia_smi=0
require_gl=0
clean_install=0
launch=0
bundle_opencv_ffmpeg=0
clean_runpath=1
extra_cmake_args=()
bundled_runtime_roots=()

usage() {
    cat <<'EOF'
Usage: build_linux_app_drop.sh [options] [-- extra-cmake-configure-args...]

Options:
  --preset NAME             CMake configure preset.
                            Default: linux-trt10-cuda12.4-release
  --build-dir PATH          Build directory. Default: build/<preset>
  --install-prefix PATH     Staged install root. Default: dist/Crimson
  --jobs N                  Parallel build jobs. Default: CMake/ninja default.
  --clean-install           Remove the install prefix before cmake --install.
  --skip-submodules         Do not update git submodules.
  --skip-configure          Do not run cmake configure.
  --skip-build              Do not run cmake build.
  --skip-install            Do not run cmake install.
  --skip-runtime-check      Do not run check_crimson_runtime.sh.
  --runtime-check-mode MODE Runtime check policy: dev or release. Default: dev.
  --dependency-manifest PATH
                            Write a JSON dependency manifest during runtime check.
  --require-nvidia-smi      Runtime check fails if nvidia-smi is unavailable.
  --require-gl              Runtime check fails if no GL/X probe succeeds.
  --bundle-opencv-ffmpeg    Copy OpenCV and FFmpeg shared libraries into the
                            app drop. CUDA/TensorRT remain managed roots.
  --skip-runpath-cleanup    Do not patch the installed Linux executable RUNPATH.
  --launch                  Launch the staged app through bin/crimson.
  -h, --help                Show this help.

Examples:
  tools/build_linux_app_drop.sh
  tools/build_linux_app_drop.sh --clean-install --require-nvidia-smi
  tools/build_linux_app_drop.sh -- --debug-find
EOF
}

absolute_path() {
    local path_value="$1"
    if [[ "$path_value" = /* ]]; then
        printf '%s\n' "$path_value"
    else
        printf '%s\n' "$repo_root/$path_value"
    fi
}

run_step() {
    echo ""
    echo "== $1 =="
}

run_command() {
    printf '+'
    printf ' %q' "$@"
    printf '\n'
    "$@"
}

json_string() {
    local value="$1"
    value="${value//\\/\\\\}"
    value="${value//\"/\\\"}"
    value="${value//$'\n'/ }"
    printf '"%s"' "$value"
}

cache_value() {
    local key="$1"
    local cache_file="$2"
    if [ ! -r "$cache_file" ]; then
        return 0
    fi
    grep -E "^${key}(:[A-Za-z]+)?=" "$cache_file" 2>/dev/null | head -n 1 | sed 's/^[^=]*=//' || true
}

canonical_existing_path() {
    local path_value="$1"
    if [ -d "$path_value" ]; then
        cd -- "$path_value" && pwd
    elif [ -e "$path_value" ]; then
        local parent
        local name
        parent="$(dirname -- "$path_value")"
        name="$(basename -- "$path_value")"
        printf '%s/%s\n' "$(cd -- "$parent" && pwd)" "$name"
    else
        printf '%s\n' "$path_value"
    fi
}

append_bundled_runtime_root() {
    local root="$1"
    local existing

    [ -n "$root" ] || return
    root="$(canonical_existing_path "$root")"
    for existing in "${bundled_runtime_roots[@]}"; do
        if [ "$existing" = "$root" ]; then
            return
        fi
    done
    bundled_runtime_roots+=("$root")
}

path_contains_or_equals() {
    local parent="$1"
    local child="$2"

    parent="$(canonical_existing_path "$parent")"
    child="$(canonical_existing_path "$child")"
    [ "$child" = "$parent" ] || [[ "$child" = "$parent"/* ]]
}

is_bundled_runtime_root() {
    local candidate="$1"
    local bundled_root

    [ -n "$candidate" ] || return 1
    for bundled_root in "${bundled_runtime_roots[@]}"; do
        if path_contains_or_equals "$candidate" "$bundled_root" \
            || path_contains_or_equals "$bundled_root" "$candidate"; then
            return 0
        fi
    done
    return 1
}

write_release_metadata() {
    local install_root="$1"
    local build_root="$2"
    local release_path="$install_root/release.json"
    local cache_file="$build_root/CMakeCache.txt"
    local commit="unknown"
    local commit_short="unknown"
    local branch="unknown"
    local dirty="unknown"
    local built_at_utc
    local host_name

    built_at_utc="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
    host_name="$(hostname 2>/dev/null || printf 'unknown')"

    if git -C "$repo_root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        commit="$(git -C "$repo_root" rev-parse HEAD 2>/dev/null || printf 'unknown')"
        commit_short="$(git -C "$repo_root" rev-parse --short HEAD 2>/dev/null || printf 'unknown')"
        branch="$(git -C "$repo_root" rev-parse --abbrev-ref HEAD 2>/dev/null || printf 'unknown')"
        if git -C "$repo_root" diff --quiet --ignore-submodules -- 2>/dev/null \
            && git -C "$repo_root" diff --cached --quiet --ignore-submodules -- 2>/dev/null; then
            dirty="false"
        else
            dirty="true"
        fi
    fi

    mkdir -p -- "$install_root"
    {
        printf '{\n'
        printf '  "release_name": %s,\n' "$(json_string "crimson-${preset}-${commit_short}")"
        printf '  "platform": "linux",\n'
        printf '  "preset": %s,\n' "$(json_string "$preset")"
        printf '  "commit": %s,\n' "$(json_string "$commit")"
        printf '  "commit_short": %s,\n' "$(json_string "$commit_short")"
        printf '  "branch": %s,\n' "$(json_string "$branch")"
        printf '  "dirty": %s,\n' "$dirty"
        printf '  "built_at_utc": %s,\n' "$(json_string "$built_at_utc")"
        printf '  "build_host": %s,\n' "$(json_string "$host_name")"
        printf '  "build_dir": %s,\n' "$(json_string "$build_root")"
        printf '  "install_root": %s,\n' "$(json_string "$install_root")"
        printf '  "cuda_toolkit_root": %s,\n' "$(json_string "$(cache_value "CUDA_TOOLKIT_ROOT_DIR" "$cache_file")")"
        printf '  "cudatoolkit_root": %s,\n' "$(json_string "$(cache_value "CUDAToolkit_ROOT" "$cache_file")")"
        printf '  "opencv_dir": %s,\n' "$(json_string "$(cache_value "OpenCV_DIR" "$cache_file")")"
        printf '  "ffmpeg_root": %s,\n' "$(json_string "$(cache_value "FFMPEG_ROOT" "$cache_file")")"
        printf '  "tensorrt_root": %s,\n' "$(json_string "$(cache_value "TENSORRT_ROOT" "$cache_file")")"
        printf '  "expected_cuda_version": %s,\n' "$(json_string "$(cache_value "CRIMSON_EXPECT_CUDA_VERSION" "$cache_file")")"
        printf '  "expected_opencv_version": %s,\n' "$(json_string "$(cache_value "CRIMSON_EXPECT_OPENCV_VERSION" "$cache_file")")"
        printf '  "expected_tensorrt_version": %s\n' "$(json_string "$(cache_value "CRIMSON_EXPECT_TENSORRT_VERSION" "$cache_file")")"
        printf '}\n'
    } > "$release_path"
}

write_runtime_roots_config() {
    local install_root="$1"
    local config_dir="$install_root/etc/crimson"
    local config_file="$config_dir/runtime_roots.env"
    local runtime_root
    local old_ifs

    if [ -z "${CRIMSON_ALLOWED_RUNTIME_ROOTS:-}" ]; then
        rm -f -- "$config_file"
        return
    fi

    mkdir -p -- "$config_dir"
    : > "$config_file"

    old_ifs="$IFS"
    IFS=:
    for runtime_root in ${CRIMSON_ALLOWED_RUNTIME_ROOTS:-}; do
        if [ -n "$runtime_root" ] && ! is_bundled_runtime_root "$runtime_root"; then
            printf '%s\n' "$runtime_root" >> "$config_file"
        fi
    done
    IFS="$old_ifs"
}

copy_library_family() {
    local source_dir="$1"
    local destination_dir="$2"
    local pattern="$3"
    local required="${4:-1}"
    local matched=0
    local lib_path

    shopt -s nullglob
    for lib_path in "$source_dir"/$pattern; do
        matched=1
        cp -a -- "$lib_path" "$destination_dir/"
    done
    shopt -u nullglob

    if [ "$matched" -eq 0 ]; then
        if [ "$required" -eq 1 ]; then
            echo "warning: no libraries matched $source_dir/$pattern" >&2
        fi
    fi
}

bundle_opencv_ffmpeg_runtime() {
    local install_root="$1"
    local build_root="$2"
    local cache_file="$build_root/CMakeCache.txt"
    local opencv_dir
    local ffmpeg_root
    local opencv_lib_dir
    local ffmpeg_lib_dir
    local bundle_dir="$install_root/lib/crimson/private"

    opencv_dir="$(cache_value "OpenCV_DIR" "$cache_file")"
    ffmpeg_root="$(cache_value "FFMPEG_ROOT" "$cache_file")"
    [ -n "$opencv_dir" ] || { echo "OpenCV_DIR not found in $cache_file" >&2; exit 1; }
    [ -n "$ffmpeg_root" ] || { echo "FFMPEG_ROOT not found in $cache_file" >&2; exit 1; }

    opencv_lib_dir="$(dirname -- "$(dirname -- "$opencv_dir")")"
    ffmpeg_lib_dir="$ffmpeg_root/lib"

    [ -d "$opencv_lib_dir" ] || { echo "OpenCV lib dir not found: $opencv_lib_dir" >&2; exit 1; }
    [ -d "$ffmpeg_lib_dir" ] || { echo "FFmpeg lib dir not found: $ffmpeg_lib_dir" >&2; exit 1; }

    mkdir -p -- "$bundle_dir"

    copy_library_family "$opencv_lib_dir" "$bundle_dir" "libopencv*.so*" 1
    copy_library_family "$ffmpeg_lib_dir" "$bundle_dir" "libav*.so*" 1
    copy_library_family "$ffmpeg_lib_dir" "$bundle_dir" "libsw*.so*" 1
    copy_library_family "$ffmpeg_lib_dir" "$bundle_dir" "libpostproc*.so*" 0

    append_bundled_runtime_root "$opencv_lib_dir"
    append_bundled_runtime_root "$(dirname -- "$opencv_lib_dir")"
    append_bundled_runtime_root "$ffmpeg_lib_dir"
    append_bundled_runtime_root "$ffmpeg_root"

    echo "Bundled OpenCV/FFmpeg runtime libraries into:"
    echo "  $bundle_dir"
}

read_elf_runpath() {
    local executable="$1"

    readelf -d "$executable" 2>/dev/null \
        | sed -n 's/^.*Library r.*path: \[\(.*\)\].*$/\1/p' \
        | head -n 1
}

clean_installed_runpath() {
    local install_root="$1"
    local executable="$install_root/bin/redgui"
    local desired_runpath='$ORIGIN:$ORIGIN/../lib:$ORIGIN/../lib/crimson/private'
    local current_runpath
    local updated_runpath

    [ -x "$executable" ] || { echo "installed executable not found: $executable" >&2; exit 1; }
    command -v readelf >/dev/null 2>&1 || { echo "readelf is required for RUNPATH cleanup" >&2; exit 1; }

    current_runpath="$(read_elf_runpath "$executable")"
    if [ "$current_runpath" = "$desired_runpath" ]; then
        echo "Installed RUNPATH already clean:"
        echo "  $desired_runpath"
        return
    fi

    if command -v patchelf >/dev/null 2>&1; then
        run_command patchelf --set-rpath "$desired_runpath" "$executable"
    else
        run_command cmake \
            -D "CRIMSON_RPATH_FILE=$executable" \
            -D "CRIMSON_OLD_RPATH=$current_runpath" \
            -D "CRIMSON_NEW_RPATH=$desired_runpath" \
            -P "$repo_root/tools/patch_linux_runpath.cmake"
    fi

    updated_runpath="$(read_elf_runpath "$executable")"
    if [ "$updated_runpath" != "$desired_runpath" ]; then
        echo "RUNPATH cleanup failed:" >&2
        echo "  expected: $desired_runpath" >&2
        echo "  actual:   ${updated_runpath:-<none>}" >&2
        exit 1
    fi

    echo "Cleaned installed RUNPATH:"
    echo "  $updated_runpath"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --preset)
            preset="${2:-}"
            shift 2
            ;;
        --build-dir)
            build_dir="${2:-}"
            shift 2
            ;;
        --install-prefix)
            install_prefix="${2:-}"
            shift 2
            ;;
        --jobs)
            jobs="${2:-}"
            shift 2
            ;;
        --clean-install)
            clean_install=1
            shift
            ;;
        --skip-submodules)
            skip_submodules=1
            shift
            ;;
        --skip-configure)
            skip_configure=1
            shift
            ;;
        --skip-build)
            skip_build=1
            shift
            ;;
        --skip-install)
            skip_install=1
            shift
            ;;
        --skip-runtime-check)
            skip_runtime_check=1
            shift
            ;;
        --runtime-check-mode)
            runtime_check_mode="${2:-}"
            shift 2
            ;;
        --dependency-manifest)
            dependency_manifest="${2:-}"
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
        --bundle-opencv-ffmpeg)
            bundle_opencv_ffmpeg=1
            shift
            ;;
        --skip-runpath-cleanup)
            clean_runpath=0
            shift
            ;;
        --launch)
            launch=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            extra_cmake_args+=("$@")
            break
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [ -z "$preset" ]; then
    echo "--preset requires a non-empty value" >&2
    exit 2
fi

case "$runtime_check_mode" in
    dev|release) ;;
    *)
        echo "--runtime-check-mode must be 'dev' or 'release', got: $runtime_check_mode" >&2
        exit 2
        ;;
esac

if [ -z "$build_dir" ]; then
    build_dir="build/$preset"
fi

build_dir_abs="$(absolute_path "$build_dir")"
install_prefix_abs="$(absolute_path "$install_prefix")"

echo "Crimson Linux app-drop build"
echo "  repo:          $repo_root"
echo "  preset:        $preset"
echo "  build dir:     $build_dir_abs"
echo "  install root:  $install_prefix_abs"

run_step "Tool check"
run_command cmake --version
if command -v ninja >/dev/null 2>&1; then
    run_command ninja --version
else
    echo "ninja not found in PATH; configure may still succeed if the preset uses another generator"
fi

if [ "$skip_submodules" -eq 0 ]; then
    run_step "Submodules"
    run_command git -C "$repo_root" submodule update --init --recursive
fi

if [ "$skip_configure" -eq 0 ]; then
    run_step "Configure"
    if [ "${#extra_cmake_args[@]}" -gt 0 ]; then
        run_command cmake --preset "$preset" -S "$repo_root" -B "$build_dir_abs" "${extra_cmake_args[@]}"
    else
        run_command cmake --preset "$preset" -S "$repo_root" -B "$build_dir_abs"
    fi
fi

if [ "$skip_build" -eq 0 ]; then
    run_step "Build"
    build_args=(cmake --build "$build_dir_abs")
    if [ -n "$jobs" ]; then
        build_args+=(--parallel "$jobs")
    fi
    run_command "${build_args[@]}"
fi

if [ "$skip_install" -eq 0 ]; then
    run_step "Install app drop"
    if [ "$clean_install" -eq 1 ]; then
        case "$install_prefix_abs" in
            ""|"/"|"$repo_root"|"$repo_root/")
                echo "Refusing to clean unsafe install prefix: $install_prefix_abs" >&2
                exit 1
                ;;
        esac
        rm -rf -- "$install_prefix_abs"
    fi
    run_command cmake --install "$build_dir_abs" --prefix "$install_prefix_abs"
    if [ "$clean_runpath" -eq 1 ]; then
        run_step "Clean installed RUNPATH"
        clean_installed_runpath "$install_prefix_abs"
    fi
    if [ "$bundle_opencv_ffmpeg" -eq 1 ]; then
        run_step "Bundle OpenCV/FFmpeg runtime"
        bundle_opencv_ffmpeg_runtime "$install_prefix_abs" "$build_dir_abs"
    fi
    write_release_metadata "$install_prefix_abs" "$build_dir_abs"
    write_runtime_roots_config "$install_prefix_abs"
fi

if [ "$skip_runtime_check" -eq 0 ]; then
    run_step "Runtime check"
    check_script="$install_prefix_abs/check_crimson_runtime.sh"
    if [ ! -x "$check_script" ]; then
        echo "Runtime check script not found or not executable: $check_script" >&2
        exit 1
    fi
    check_args=("$check_script" --app-root "$install_prefix_abs" --mode "$runtime_check_mode")
    if [ -n "$dependency_manifest" ]; then
        check_args+=(--write-dependency-manifest "$(absolute_path "$dependency_manifest")")
    fi
    if [ "$require_nvidia_smi" -eq 1 ]; then
        check_args+=(--require-nvidia-smi)
    fi
    if [ "$require_gl" -eq 1 ]; then
        check_args+=(--require-gl)
    fi
    if [ "$bundle_opencv_ffmpeg" -eq 1 ]; then
        run_command env -u CRIMSON_ALLOWED_RUNTIME_ROOTS "${check_args[@]}"
    else
        run_command "${check_args[@]}"
    fi
fi

if [ "$launch" -eq 1 ]; then
    run_step "Launch"
    run_command "$install_prefix_abs/bin/crimson"
fi

echo ""
echo "Linux app drop ready:"
echo "  $install_prefix_abs"
