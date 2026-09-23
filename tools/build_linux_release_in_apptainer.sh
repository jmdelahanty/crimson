#!/usr/bin/env bash
set -Eeuo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"
builder="${CRIMSON_LINUX_BUILDER_SIF:-$HOME/crimson-builders/crimson-linux-ubuntu22-cuda12.4-trt10.sif}"
builder_lock="$repo_root/packaging/linux/ubuntu22-cuda12.4-trt10/CrimsonLinuxBuilder.lock.json"
build_dir="${CRIMSON_LINUX_BUILD_DIR:-$repo_root/build/linux-ubuntu22-cuda12.4-trt10-release}"
install_prefix="${CRIMSON_LINUX_INSTALL_PREFIX:-$repo_root/dist/Crimson-linux-x86_64}"
cuda_architectures="${CRIMSON_LINUX_CUDA_ARCHITECTURES:-80;86}"
jobs="${CRIMSON_BUILD_JOBS:-}"
skip_configure=0
skip_build=0
verify_builder_only=0

usage() {
    cat <<'EOF'
Usage: build_linux_release_in_apptainer.sh [options]

Builds a relocatable Crimson Linux app drop in the pinned Ubuntu 22 builder.
The host supplies link-time NVIDIA driver libraries, but they are never copied
into the app drop.

Options:
  --builder PATH             Builder SIF path.
  --builder-lock PATH        Repository lock describing the approved SIF.
  --build-dir PATH           Out-of-source CMake build directory.
  --install-prefix PATH      Staged app-drop directory.
  --cuda-architectures LIST  CMake CUDA architectures. Default: 80;86.
  --jobs N                   Parallel build jobs.
  --skip-configure           Reuse an existing configured build tree.
  --skip-build               Reuse an existing compiled build tree.
  --verify-builder-only      Verify the locked SIF and exit without building.
  -h, --help                 Show this help.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --builder)
            builder="${2:-}"
            shift 2
            ;;
        --builder-lock)
            builder_lock="${2:-}"
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
        --cuda-architectures)
            cuda_architectures="${2:-}"
            shift 2
            ;;
        --jobs)
            jobs="${2:-}"
            shift 2
            ;;
        --skip-configure)
            skip_configure=1
            shift
            ;;
        --skip-build)
            skip_build=1
            shift
            ;;
        --verify-builder-only)
            verify_builder_only=1
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

command -v apptainer >/dev/null 2>&1 || { echo "apptainer is required" >&2; exit 1; }
command -v sha256sum >/dev/null 2>&1 || { echo "sha256sum is required" >&2; exit 1; }
[ -r "$builder" ] || { echo "Builder SIF not found: $builder" >&2; exit 1; }
[ -r "$builder_lock" ] || { echo "Builder lock not found: $builder_lock" >&2; exit 1; }

if [ -r "${builder}.sha256" ]; then
    (cd -- "$(dirname -- "$builder")" && sha256sum -c "$(basename -- "${builder}.sha256")")
else
    echo "warning: builder checksum is missing: ${builder}.sha256" >&2
fi

locked_sha256="$(sed -n 's/.*"builder_sif_sha256": "\([0-9a-f]\{64\}\)".*/\1/p' "$builder_lock")"
[ -n "$locked_sha256" ] || { echo "Builder SHA-256 is missing from: $builder_lock" >&2; exit 1; }
actual_sha256="$(sha256sum "$builder" | awk '{print $1}')"
if [ "$actual_sha256" != "$locked_sha256" ]; then
    echo "Builder SIF does not match the approved repository lock:" >&2
    echo "  expected: $locked_sha256" >&2
    echo "  actual:   $actual_sha256" >&2
    exit 1
fi

apptainer exec --cleanenv "$builder" /bin/bash -lc '
    set -eu
    test "$(getconf GNU_LIBC_VERSION)" = "glibc 2.35"
    test -x /opt/crimson/cmake-3.30.5-linux-x86_64/bin/cmake
    test -r /opt/crimson/opencv-4.10.0-jammy/lib/cmake/opencv4/OpenCVConfig.cmake
    test -r /opt/crimson/tensorrt-10.0.1.6/lib/libnvinfer.so
    test -r /opt/crimson/ffmpeg-jammy/lib/libavcodec.so
'
if [ "$verify_builder_only" -eq 1 ]; then
    echo "Crimson Linux builder verification passed:"
    echo "  $builder"
    echo "  sha256=$actual_sha256"
    exit 0
fi

find_driver_library() {
    local soname="$1"
    ldconfig -p 2>/dev/null \
        | awk -v name="$soname" '$1 == name { print $NF; exit }'
}

cuda_driver="$(find_driver_library libcuda.so.1)"
nvcuvid_driver="$(find_driver_library libnvcuvid.so.1)"
nvml_driver="$(find_driver_library libnvidia-ml.so.1)"
[ -r "$cuda_driver" ] || { echo "Host NVIDIA libcuda.so.1 was not found" >&2; exit 1; }
[ -r "$nvcuvid_driver" ] || { echo "Host NVIDIA libnvcuvid.so.1 was not found" >&2; exit 1; }
[ -r "$nvml_driver" ] || { echo "Host NVIDIA libnvidia-ml.so.1 was not found" >&2; exit 1; }

mkdir -p -- "$build_dir" "$(dirname -- "$install_prefix")"
bind_args=(
    --bind "$repo_root:$repo_root"
    --bind "$build_dir:$build_dir"
    --bind "$(dirname -- "$install_prefix"):$(dirname -- "$install_prefix")"
    --bind "$cuda_driver:/opt/crimson/host-driver/libcuda.so.1:ro"
    --bind "$nvcuvid_driver:/opt/crimson/host-driver/libnvcuvid.so.1:ro"
    --bind "$nvml_driver:/opt/crimson/host-driver/libnvidia-ml.so.1:ro"
)
container_env=(
    PATH=/opt/crimson/cmake-3.30.5-linux-x86_64/bin:/usr/local/cuda/bin:/usr/local/bin:/usr/bin:/bin
    CRIMSON_CUDA_TOOLKIT_ROOT=/usr/local/cuda
    CRIMSON_OPENCV_DIR=/opt/crimson/opencv-4.10.0-jammy/lib/cmake/opencv4
    CRIMSON_FFMPEG_ROOT=/opt/crimson/ffmpeg-jammy
    CRIMSON_TENSORRT_ROOT=/opt/crimson/tensorrt-10.0.1.6
    CRIMSON_VIDEO_CODEC_SDK_ROOT=
)

run_in_builder() {
    apptainer exec --cleanenv "${bind_args[@]}" "$builder" env "${container_env[@]}" "$@"
}

if [ "$skip_configure" -eq 0 ]; then
    run_in_builder cmake \
        --preset linux-trt10-cuda12.4-release \
        -S "$repo_root" \
        -B "$build_dir" \
        -D "CMAKE_CUDA_ARCHITECTURES=$cuda_architectures" \
        -D CRIMSON_PREBUILT_TENSORSTORE_BUILD_DIR=/nonexistent \
        -D CUDA_DRIVER=/opt/crimson/host-driver/libcuda.so.1 \
        -D NVCUVID_LIBRARY=/opt/crimson/host-driver/libnvcuvid.so.1 \
        -D NVML_LIBRARY=/opt/crimson/host-driver/libnvidia-ml.so.1
fi

if [ "$skip_build" -eq 0 ]; then
    build_args=(cmake --build "$build_dir")
    if [ -n "$jobs" ]; then
        build_args+=(--parallel "$jobs")
    fi
    run_in_builder "${build_args[@]}"
fi

package_args=(
    "$repo_root/tools/build_linux_app_drop.sh"
    --build-dir "$build_dir"
    --install-prefix "$install_prefix"
    --skip-submodules
    --skip-configure
    --skip-build
    --skip-runtime-check
    --clean-install
    --bundle-opencv-ffmpeg
    --bundle-nvidia-runtime
    --bundle-runtime-closure
)
run_in_builder "${package_args[@]}"

echo "Crimson Ubuntu 22 app drop ready:"
echo "  $install_prefix"
echo "Run the target machine's check_crimson_runtime.sh before GUI validation."
