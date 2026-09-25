#!/usr/bin/env bash
set -Eeuo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"

share_root="${CRIMSON_LINUX_SHARE_ROOT:-/groups/ahrens/ahrenslab/crimson/linux-app}"
install_prefix="${CRIMSON_LINUX_INSTALL_PREFIX:-dist/Crimson}"
build_dir="${CRIMSON_LINUX_BUILD_DIR:-build/linux-app-drop-nvidia-runtime-bundle}"
cuda_root="${CRIMSON_CUDA_TOOLKIT_ROOT:-/usr/local/cuda-12.4}"
opencv_dir="${CRIMSON_OPENCV_DIR:-/opt/crimson/lib/opencv-ffmpeg-nvidia/lib/cmake/opencv4}"
ffmpeg_root="${CRIMSON_FFMPEG_ROOT:-/opt/orange/lib/ffmpeg-nvidia}"
tensorrt_root="${CRIMSON_TENSORRT_ROOT:-/usr/local/TensorRT-10.0.1.6}"
release_name=""
jobs="${CRIMSON_BUILD_JOBS:-}"
clean_install=1
archive_existing_current=1
skip_submodules=1
bundle_nvidia_runtime=1
require_nvidia_smi=0
require_gl=0
dry_run=0

usage() {
    cat <<'EOF'
Usage: publish_linux_current_app_drop.sh [options]

Builds the validated Linux app-drop layout, runs the release-mode runtime
check, publishes a versioned release, and refreshes current.

Defaults are the current Crimson Linux bundle stack:
  CUDA Toolkit: /usr/local/cuda-12.4
  TensorRT:     /usr/local/TensorRT-10.0.1.6
  OpenCV:       /opt/crimson/lib/opencv-ffmpeg-nvidia/lib/cmake/opencv4
  FFmpeg:       /opt/orange/lib/ffmpeg-nvidia
  Share root:   /groups/ahrens/ahrenslab/crimson/linux-app

Options:
  --share-root PATH              Publish root. Default: $CRIMSON_LINUX_SHARE_ROOT
                                 or /groups/ahrens/ahrenslab/crimson/linux-app.
  --build-dir PATH               Build directory. Default:
                                 build/linux-app-drop-nvidia-runtime-bundle.
  --install-prefix PATH          Staged install root. Default: dist/Crimson.
  --cuda-root PATH               CUDA Toolkit root. Default: /usr/local/cuda-12.4.
  --opencv-dir PATH              OpenCV CMake package dir.
  --ffmpeg-root PATH             FFmpeg root.
  --tensorrt-root PATH           TensorRT root.
  --release-name NAME            Versioned release name. Default: auto.
  --jobs N                       Parallel build jobs.
  --no-clean-install             Do not remove dist/Crimson before install.
  --no-archive-existing-current  Do not archive the previous current release.
  --update-submodules            Run git submodule update before configuring.
  --managed-nvidia-runtime       Do not bundle TensorRT/NPP; use managed
                                 TensorRT/CUDA roots in runtime_roots.env.
  --require-nvidia-smi           Fail runtime check if nvidia-smi is unavailable.
  --require-gl                   Fail runtime check if no GL/X probe succeeds.
  --dry-run                      Print the delegated command without running it.
  -h, --help                     Show this help.

Examples:
  tools/publish_linux_current_app_drop.sh

  tools/publish_linux_current_app_drop.sh \
    --share-root /groups/johnson/johnsonlab/jeremy/crimson/linux-app

  tools/publish_linux_current_app_drop.sh --dry-run
EOF
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

print_command() {
    printf '+'
    printf ' %q' "$@"
    printf '\n'
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --share-root)
            share_root="${2:-}"
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
        --cuda-root)
            cuda_root="${2:-}"
            shift 2
            ;;
        --opencv-dir)
            opencv_dir="${2:-}"
            shift 2
            ;;
        --ffmpeg-root)
            ffmpeg_root="${2:-}"
            shift 2
            ;;
        --tensorrt-root)
            tensorrt_root="${2:-}"
            shift 2
            ;;
        --release-name)
            release_name="${2:-}"
            shift 2
            ;;
        --jobs)
            jobs="${2:-}"
            shift 2
            ;;
        --no-clean-install)
            clean_install=0
            shift
            ;;
        --no-archive-existing-current)
            archive_existing_current=0
            shift
            ;;
        --update-submodules)
            skip_submodules=0
            shift
            ;;
        --managed-nvidia-runtime)
            bundle_nvidia_runtime=0
            if [ "$build_dir" = "build/linux-app-drop-nvidia-runtime-bundle" ]; then
                build_dir="build/linux-app-drop-opencv-ffmpeg-nvidia-hybrid"
            fi
            shift
            ;;
        --require-nvidia-smi)
            require_nvidia_smi=1
            shift
            ;;
        --require-gl)
            require_gl=1
            shift
            ;;
        --dry-run)
            dry_run=1
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

[ -n "$share_root" ] || { echo "--share-root must be non-empty" >&2; exit 2; }
[ -n "$cuda_root" ] || { echo "--cuda-root must be non-empty" >&2; exit 2; }
[ -n "$opencv_dir" ] || { echo "--opencv-dir must be non-empty" >&2; exit 2; }
[ -n "$ffmpeg_root" ] || { echo "--ffmpeg-root must be non-empty" >&2; exit 2; }
[ -n "$tensorrt_root" ] || { echo "--tensorrt-root must be non-empty" >&2; exit 2; }

env_args=(
    "CRIMSON_CUDA_TOOLKIT_ROOT=$cuda_root"
    "CRIMSON_OPENCV_DIR=$opencv_dir"
    "CRIMSON_FFMPEG_ROOT=$ffmpeg_root"
    "CRIMSON_TENSORRT_ROOT=$tensorrt_root"
)

if [ "$bundle_nvidia_runtime" -eq 0 ]; then
    env_args+=("CRIMSON_ALLOWED_RUNTIME_ROOTS=$tensorrt_root:$cuda_root")
fi

cmd=(
    "$script_dir/build_check_publish_linux_app_drop.sh"
    --build-dir "$build_dir"
    --install-prefix "$install_prefix"
    --bundle-opencv-ffmpeg
    --share-root "$share_root"
    --publish-current
)

append_switch cmd --clean-install "$clean_install"
append_switch cmd --archive-existing-current "$archive_existing_current"
append_switch cmd --skip-submodules "$skip_submodules"
append_switch cmd --bundle-nvidia-runtime "$bundle_nvidia_runtime"
append_switch cmd --require-nvidia-smi "$require_nvidia_smi"
append_switch cmd --require-gl "$require_gl"
append_arg cmd --release-name "$release_name"
append_arg cmd --jobs "$jobs"

cmd+=(
    --
    "-DCUDA_TOOLKIT_ROOT_DIR=$cuda_root"
    "-DCUDAToolkit_ROOT=$cuda_root"
    "-DOpenCV_DIR=$opencv_dir"
    "-DFFMPEG_ROOT=$ffmpeg_root"
    "-DTENSORRT_ROOT=$tensorrt_root"
)

echo "Crimson Linux current app-drop publish preset"
echo "  repo:          $repo_root"
echo "  share root:    $share_root"
echo "  build dir:     $build_dir"
echo "  install root:  $install_prefix"
echo "  OpenCV/FFmpeg: bundled"
if [ "$bundle_nvidia_runtime" -eq 1 ]; then
    echo "  NVIDIA runtime: bundled TensorRT/NPP; driver libs host-resolved"
else
    echo "  NVIDIA runtime: managed TensorRT/CUDA roots"
fi

if [ "$dry_run" -eq 1 ]; then
    print_command env "${env_args[@]}" "${cmd[@]}"
    exit 0
fi

print_command env "${env_args[@]}" "${cmd[@]}"
env "${env_args[@]}" "${cmd[@]}"
