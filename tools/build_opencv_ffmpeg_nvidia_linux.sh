#!/usr/bin/env bash
set -Eeuo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"

opencv_version="4.10.0"
ffmpeg_root="${CRIMSON_FFMPEG_ROOT:-/opt/orange/lib/ffmpeg-nvidia}"
source_root="${CRIMSON_OPENCV_SOURCE_ROOT:-$HOME/src/crimson-opencv-$opencv_version}"
build_dir="${CRIMSON_OPENCV_BUILD_DIR:-$HOME/build/crimson-opencv-ffmpeg-nvidia-$opencv_version}"
install_prefix="${CRIMSON_OPENCV_INSTALL_PREFIX:-/opt/crimson/lib/opencv-ffmpeg-nvidia}"
jobs="${CRIMSON_BUILD_JOBS:-$(nproc 2>/dev/null || printf '4')}"
cuda_arch="${CRIMSON_OPENCV_CUDA_ARCH_BIN:-8.0,8.6,8.9}"
enable_cuda=0
enable_sfm=1
skip_clone=0
configure_only=0
skip_install=0
generator=""

usage() {
    cat <<'EOF'
Usage: build_opencv_ffmpeg_nvidia_linux.sh [options] [-- extra-cmake-args...]

Build OpenCV for Crimson while forcing OpenCV videoio to use the same custom
FFmpeg/NVIDIA stack as Crimson's direct decoder path.

Defaults:
  OpenCV version:  4.10.0
  FFmpeg root:     $CRIMSON_FFMPEG_ROOT or /opt/orange/lib/ffmpeg-nvidia
  source root:     $CRIMSON_OPENCV_SOURCE_ROOT or ~/src/crimson-opencv-4.10.0
  build dir:       $CRIMSON_OPENCV_BUILD_DIR or ~/build/crimson-opencv-ffmpeg-nvidia-4.10.0
  install prefix:  $CRIMSON_OPENCV_INSTALL_PREFIX or /opt/crimson/lib/opencv-ffmpeg-nvidia

Options:
  --opencv-version VERSION       OpenCV and opencv_contrib tag to build.
  --ffmpeg-root PATH             FFmpeg install root with include, lib, and lib/pkgconfig.
  --source-root PATH             Parent containing opencv/ and opencv_contrib/.
  --build-dir PATH               OpenCV build directory.
  --install-prefix PATH          OpenCV install prefix.
  --jobs N                       Parallel build jobs.
  --cuda                         Build OpenCV CUDA modules. Off by default.
  --cuda-arch LIST               CUDA_ARCH_BIN list. Default: 8.0,8.6,8.9.
  --no-sfm                       Disable opencv_sfm.
  --skip-clone                   Require sources to already exist.
  --configure-only               Stop after CMake configure.
  --skip-install                 Build but do not install or validate installed libraries.
  -h, --help                     Show this help.

After validation, point Crimson at the new build with:

  export CRIMSON_OPENCV_DIR=<install-prefix>/lib/cmake/opencv4
  export CRIMSON_FFMPEG_ROOT=<ffmpeg-root>

EOF
}

run() {
    printf '+'
    printf ' %q' "$@"
    printf '\n'
    "$@"
}

die() {
    echo "error: $*" >&2
    exit 1
}

require_file() {
    local path_value="$1"
    [ -e "$path_value" ] || die "missing required path: $path_value"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --opencv-version)
            opencv_version="${2:-}"
            shift 2
            ;;
        --ffmpeg-root)
            ffmpeg_root="${2:-}"
            shift 2
            ;;
        --source-root)
            source_root="${2:-}"
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
        --cuda)
            enable_cuda=1
            shift
            ;;
        --cuda-arch)
            cuda_arch="${2:-}"
            shift 2
            ;;
        --no-sfm)
            enable_sfm=0
            shift
            ;;
        --skip-clone)
            skip_clone=1
            shift
            ;;
        --configure-only)
            configure_only=1
            shift
            ;;
        --skip-install)
            skip_install=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            break
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

extra_cmake_args=("$@")

[ -n "$opencv_version" ] || die "--opencv-version requires a value"
[ -n "$ffmpeg_root" ] || die "--ffmpeg-root requires a value"
[ -n "$source_root" ] || die "--source-root requires a value"
[ -n "$build_dir" ] || die "--build-dir requires a value"
[ -n "$install_prefix" ] || die "--install-prefix requires a value"

ffmpeg_root="$(cd -- "$ffmpeg_root" && pwd)"
source_root="$(mkdir -p -- "$source_root" && cd -- "$source_root" && pwd)"
build_dir="$(mkdir -p -- "$build_dir" && cd -- "$build_dir" && pwd)"

opencv_src="$source_root/opencv"
opencv_contrib_src="$source_root/opencv_contrib"
ffmpeg_pc_dir="$ffmpeg_root/lib/pkgconfig"
ffmpeg_lib_dir="$ffmpeg_root/lib"
ffmpeg_include_dir="$ffmpeg_root/include"

command -v cmake >/dev/null 2>&1 || die "cmake not found"
command -v git >/dev/null 2>&1 || die "git not found"
command -v pkg-config >/dev/null 2>&1 || die "pkg-config not found"
command -v readelf >/dev/null 2>&1 || die "readelf not found"
command -v ldd >/dev/null 2>&1 || die "ldd not found"

if command -v ninja >/dev/null 2>&1; then
    generator="Ninja"
else
    generator="Unix Makefiles"
fi

require_file "$ffmpeg_include_dir/libavformat/avformat.h"
require_file "$ffmpeg_lib_dir/libavformat.so"
require_file "$ffmpeg_lib_dir/libavcodec.so"
require_file "$ffmpeg_lib_dir/libavutil.so"
require_file "$ffmpeg_lib_dir/libswscale.so"
require_file "$ffmpeg_lib_dir/libswresample.so"
require_file "$ffmpeg_pc_dir/libavformat.pc"
require_file "$ffmpeg_pc_dir/libavcodec.pc"
require_file "$ffmpeg_pc_dir/libavutil.pc"
require_file "$ffmpeg_pc_dir/libswscale.pc"
require_file "$ffmpeg_pc_dir/libswresample.pc"

export PKG_CONFIG_PATH="$ffmpeg_pc_dir"
export LD_LIBRARY_PATH="$ffmpeg_lib_dir"

echo "OpenCV FFmpeg/NVIDIA build"
echo "  repo:           $repo_root"
echo "  OpenCV version: $opencv_version"
echo "  FFmpeg root:    $ffmpeg_root"
echo "  source root:    $source_root"
echo "  build dir:      $build_dir"
echo "  install prefix: $install_prefix"
echo "  generator:      $generator"
echo "  jobs:           $jobs"
echo "  SFM:            $enable_sfm"
echo "  OpenCV CUDA:    $enable_cuda"
if [ "$enable_cuda" -eq 1 ]; then
    echo "  CUDA_ARCH_BIN:  $cuda_arch"
fi
echo ""
echo "FFmpeg pkg-config versions:"
run pkg-config --modversion libavcodec libavformat libavutil libswscale libswresample

if [ "$skip_clone" -eq 0 ]; then
    if [ ! -d "$opencv_src/.git" ]; then
        run git clone --depth 1 --branch "$opencv_version" https://github.com/opencv/opencv.git "$opencv_src"
    fi
    if [ ! -d "$opencv_contrib_src/.git" ]; then
        run git clone --depth 1 --branch "$opencv_version" https://github.com/opencv/opencv_contrib.git "$opencv_contrib_src"
    fi
fi

require_file "$opencv_src/CMakeLists.txt"
require_file "$opencv_contrib_src/modules"

cmake_args=(
    cmake
    -S "$opencv_src"
    -B "$build_dir"
    -G "$generator"
    -D "CMAKE_BUILD_TYPE=Release"
    -D "CMAKE_INSTALL_PREFIX=$install_prefix"
    -D "CMAKE_PREFIX_PATH=$ffmpeg_root"
    -D "CMAKE_INCLUDE_PATH=$ffmpeg_include_dir"
    -D "CMAKE_LIBRARY_PATH=$ffmpeg_lib_dir"
    -D "CMAKE_BUILD_RPATH=$ffmpeg_lib_dir"
    -D "CMAKE_INSTALL_RPATH=$ffmpeg_lib_dir"
    -D "CMAKE_INSTALL_RPATH_USE_LINK_PATH=ON"
    -D "BUILD_SHARED_LIBS=ON"
    -D "BUILD_EXAMPLES=OFF"
    -D "BUILD_TESTS=OFF"
    -D "BUILD_PERF_TESTS=OFF"
    -D "BUILD_DOCS=OFF"
    -D "BUILD_JAVA=OFF"
    -D "BUILD_opencv_python2=OFF"
    -D "WITH_FFMPEG=ON"
    -D "WITH_GSTREAMER=ON"
    -D "WITH_TBB=ON"
    -D "WITH_OPENGL=ON"
    -D "WITH_QT=ON"
    -D "OPENCV_ENABLE_NONFREE=ON"
    -D "OPENCV_GENERATE_PKGCONFIG=ON"
    -D "OPENCV_PC_FILE_NAME=opencv4.pc"
    -D "OPENCV_EXTRA_MODULES_PATH=$opencv_contrib_src/modules"
)

if [ "$enable_sfm" -eq 1 ]; then
    cmake_args+=(-D "BUILD_opencv_sfm=ON")
else
    cmake_args+=(-D "BUILD_opencv_sfm=OFF")
fi

if [ "$enable_cuda" -eq 1 ]; then
    cmake_args+=(
        -D "WITH_CUDA=ON"
        -D "CUDA_FAST_MATH=ON"
        -D "WITH_CUBLAS=ON"
        -D "CUDA_ARCH_BIN=$cuda_arch"
        -D "BUILD_opencv_cudacodec=ON"
    )
else
    cmake_args+=(
        -D "WITH_CUDA=OFF"
        -D "OPENCV_DNN_CUDA=OFF"
        -D "BUILD_opencv_cudacodec=OFF"
    )
fi

if [ "${#extra_cmake_args[@]}" -gt 0 ]; then
    cmake_args+=("${extra_cmake_args[@]}")
fi

run "${cmake_args[@]}"

if [ "$configure_only" -eq 1 ]; then
    echo "Configure complete."
    exit 0
fi

run cmake --build "$build_dir" --parallel "$jobs"

if [ "$skip_install" -eq 1 ]; then
    echo "Build complete. Install skipped."
    exit 0
fi

install_cmd=(cmake --install "$build_dir" --prefix "$install_prefix")
install_parent="$(dirname -- "$install_prefix")"
if [ -w "$install_parent" ] || { [ -e "$install_prefix" ] && [ -w "$install_prefix" ]; }; then
    run "${install_cmd[@]}"
else
    command -v sudo >/dev/null 2>&1 || die "install prefix is not writable and sudo is not available: $install_prefix"
    run sudo "${install_cmd[@]}"
fi

opencv_version_bin="$install_prefix/bin/opencv_version"
opencv_videoio_lib="$install_prefix/lib/libopencv_videoio.so.410"
require_file "$opencv_version_bin"
require_file "$opencv_videoio_lib"

build_info_file="$build_dir/crimson-opencv-build-info.txt"
link_info_file="$build_dir/crimson-opencv-videoio-linkage.txt"

run env LD_LIBRARY_PATH="$install_prefix/lib:$ffmpeg_lib_dir" "$opencv_version_bin" --verbose
env LD_LIBRARY_PATH="$install_prefix/lib:$ffmpeg_lib_dir" "$opencv_version_bin" --verbose > "$build_info_file"

readelf -d "$opencv_videoio_lib" > "$link_info_file"
env LD_LIBRARY_PATH="$install_prefix/lib:$ffmpeg_lib_dir" ldd "$opencv_videoio_lib" >> "$link_info_file"

grep -Eq "avcodec:[[:space:]]+YES \\(58\\." "$build_info_file" \
    || die "OpenCV build info does not show avcodec 58 from the FFmpeg/NVIDIA stack"
grep -Eq "avformat:[[:space:]]+YES \\(58\\." "$build_info_file" \
    || die "OpenCV build info does not show avformat 58 from the FFmpeg/NVIDIA stack"
grep -Eq "avutil:[[:space:]]+YES \\(56\\." "$build_info_file" \
    || die "OpenCV build info does not show avutil 56 from the FFmpeg/NVIDIA stack"
grep -q "Shared library: \\[libavcodec.so.58\\]" "$link_info_file" \
    || die "libopencv_videoio does not need libavcodec.so.58"
grep -q "Shared library: \\[libavformat.so.58\\]" "$link_info_file" \
    || die "libopencv_videoio does not need libavformat.so.58"

echo ""
echo "OpenCV FFmpeg/NVIDIA build validated."
echo "  Build info: $build_info_file"
echo "  Link info:  $link_info_file"
echo ""
echo "Use with Crimson:"
echo "  export CRIMSON_OPENCV_DIR=$install_prefix/lib/cmake/opencv4"
echo "  export CRIMSON_FFMPEG_ROOT=$ffmpeg_root"
