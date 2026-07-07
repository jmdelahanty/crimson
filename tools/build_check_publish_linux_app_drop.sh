#!/usr/bin/env bash
set -Eeuo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"

preset="linux-trt10-cuda12.4-release"
install_prefix="dist/Crimson"
build_dir=""
jobs="${CRIMSON_BUILD_JOBS:-}"
clean_install=0
skip_submodules=0
skip_configure=0
skip_build=0
skip_install=0
bundle_opencv_ffmpeg=0
bundle_nvidia_runtime=0
skip_runpath_cleanup=0
runtime_check_mode="release"
dependency_manifest=""
require_nvidia_smi=0
require_gl=0
share_root=""
release_name=""
publish_current=0
archive_existing_current=0
current_name="current"
releases_dir_name="releases"
extra_cmake_args=()

usage() {
    cat <<'EOF'
Usage: build_check_publish_linux_app_drop.sh [options] [-- extra-cmake-configure-args...]

Builds a Linux app drop, runs the release-mode runtime check, and publishes it.

Build options:
  --preset NAME             CMake configure preset.
                            Default: linux-trt10-cuda12.4-release
  --build-dir PATH          Build directory. Default: build/<preset>.
  --install-prefix PATH     Staged install root. Default: dist/Crimson.
  --jobs N                  Parallel build jobs. Default: CMake/ninja default.
  --clean-install           Remove the install prefix before cmake --install.
  --skip-submodules         Do not update git submodules.
  --skip-configure          Do not run cmake configure.
  --skip-build              Do not run cmake build.
  --skip-install            Do not run cmake install.
  --bundle-opencv-ffmpeg    Bundle OpenCV and FFmpeg into the app drop.
  --bundle-nvidia-runtime   Experimental: bundle observed non-driver NVIDIA
                            runtime libs into the app drop.
  --skip-runpath-cleanup    Do not patch the installed Linux executable RUNPATH.

Publish/check options:
  --share-root PATH         Publish root, for example /groups/.../crimson/linux-app.
  --release-name NAME       Versioned release name. Default when needed:
                            YYYY-MM-DD_HHMMSS_<short-commit>.
  --publish-current         Publish a versioned release and refresh current.
  --archive-existing-current
                            Archive current before refreshing it.
  --current-name NAME       Current drop directory name. Default: current.
  --releases-dir-name NAME  Versioned release directory name. Default: releases.
  --runtime-check-mode MODE Runtime check mode: dev or release. Default: release.
  --dependency-manifest PATH
                            Write dependency manifest during runtime check.
                            Default: <install-prefix>/dependency_manifest.json.
  --require-nvidia-smi      Runtime check fails if nvidia-smi is unavailable.
  --require-gl              Runtime check fails if no GL/X probe succeeds.
  -h, --help                Show this help.

Examples:
  CRIMSON_ALLOWED_RUNTIME_ROOTS=/usr/local/TensorRT-10.0.1.6:/usr/local/cuda-12.4 \
  tools/build_check_publish_linux_app_drop.sh \
    --clean-install \
    --bundle-opencv-ffmpeg \
    --share-root /groups/ahrens/ahrenslab/crimson/linux-app \
    --publish-current \
    --archive-existing-current
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

append_arg() {
    local -n array_ref="$1"
    local name="$2"
    local value="$3"

    if [ -n "$value" ]; then
        array_ref+=("$name" "$value")
    fi
}

append_switch() {
    local -n array_ref="$1"
    local name="$2"
    local enabled="$3"

    if [ "$enabled" -eq 1 ]; then
        array_ref+=("$name")
    fi
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
        --bundle-opencv-ffmpeg)
            bundle_opencv_ffmpeg=1
            shift
            ;;
        --bundle-nvidia-runtime)
            bundle_nvidia_runtime=1
            shift
            ;;
        --skip-runpath-cleanup)
            skip_runpath_cleanup=1
            shift
            ;;
        --share-root)
            share_root="${2:-}"
            shift 2
            ;;
        --release-name)
            release_name="${2:-}"
            shift 2
            ;;
        --publish-current)
            publish_current=1
            shift
            ;;
        --archive-existing-current)
            archive_existing_current=1
            shift
            ;;
        --current-name)
            current_name="${2:-}"
            shift 2
            ;;
        --releases-dir-name)
            releases_dir_name="${2:-}"
            shift 2
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

case "$runtime_check_mode" in
    dev|release) ;;
    *)
        echo "--runtime-check-mode must be 'dev' or 'release', got: $runtime_check_mode" >&2
        exit 2
        ;;
esac

if [ -z "$share_root" ]; then
    echo "Pass --share-root with the Linux-visible publish root, for example /groups/.../crimson/linux-app" >&2
    exit 2
fi
if [ "$publish_current" -eq 0 ] && [ -z "$release_name" ]; then
    echo "Pass --publish-current to refresh current, or pass --release-name for a versioned-only publish." >&2
    exit 2
fi
if [ "$archive_existing_current" -eq 1 ] && [ "$publish_current" -eq 0 ]; then
    echo "--archive-existing-current requires --publish-current" >&2
    exit 2
fi

install_root_abs="$(absolute_path "$install_prefix")"
if [ -z "$dependency_manifest" ]; then
    dependency_manifest="$install_root_abs/dependency_manifest.json"
else
    dependency_manifest="$(absolute_path "$dependency_manifest")"
fi

build_args=(--preset "$preset" --install-prefix "$install_prefix" --skip-runtime-check)
append_arg build_args --build-dir "$build_dir"
append_arg build_args --jobs "$jobs"
append_switch build_args --clean-install "$clean_install"
append_switch build_args --skip-submodules "$skip_submodules"
append_switch build_args --skip-configure "$skip_configure"
append_switch build_args --skip-build "$skip_build"
append_switch build_args --skip-install "$skip_install"
append_switch build_args --bundle-opencv-ffmpeg "$bundle_opencv_ffmpeg"
append_switch build_args --bundle-nvidia-runtime "$bundle_nvidia_runtime"
append_switch build_args --skip-runpath-cleanup "$skip_runpath_cleanup"
if [ "${#extra_cmake_args[@]}" -gt 0 ]; then
    build_args+=(-- "${extra_cmake_args[@]}")
fi

publish_args=(
    --stage-root "$install_root_abs"
    --share-root "$share_root"
    --current-name "$current_name"
    --releases-dir-name "$releases_dir_name"
    --runtime-check-mode "$runtime_check_mode"
    --dependency-manifest "$dependency_manifest"
)
append_arg publish_args --release-name "$release_name"
append_switch publish_args --publish-current "$publish_current"
append_switch publish_args --archive-existing-current "$archive_existing_current"
append_switch publish_args --require-nvidia-smi "$require_nvidia_smi"
append_switch publish_args --require-gl "$require_gl"

echo "Crimson Linux build/check/publish"
echo "  repo:         $repo_root"
echo "  preset:       $preset"
echo "  install root: $install_root_abs"
echo "  share root:   $share_root"
if [ -n "$release_name" ]; then
    echo "  release name: $release_name"
else
    echo "  release name: auto"
fi
echo "  publish current: $publish_current"
echo "  archive current: $archive_existing_current"

run_step "Build and stage app drop"
run_command "$script_dir/build_linux_app_drop.sh" "${build_args[@]}"

run_step "Publish app drop"
run_command "$script_dir/publish_linux_app_drop.sh" "${publish_args[@]}"

echo ""
echo "Build/check/publish complete."
