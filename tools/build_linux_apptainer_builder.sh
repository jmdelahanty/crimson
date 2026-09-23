#!/usr/bin/env bash
set -Eeuo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"
definition="$repo_root/packaging/linux/ubuntu22-cuda12.4-trt10/CrimsonLinuxBuilder.def"
output="${CRIMSON_LINUX_BUILDER_SIF:-$HOME/crimson-builders/crimson-linux-ubuntu22-cuda12.4-trt10.sif}"
sandbox=""
use_sudo=0

usage() {
    cat <<'EOF'
Usage: build_linux_apptainer_builder.sh [options]

Builds or seals Crimson's pinned Ubuntu 22 Linux builder and writes a sibling
SHA-256 file. Normal release builds reuse the resulting SIF.

Options:
  --definition PATH  Apptainer definition to build. Defaults to the repository
                     Ubuntu 22/CUDA 12.4/TensorRT 10 definition.
  --output PATH      Destination SIF. Default:
                     ~/crimson-builders/crimson-linux-ubuntu22-cuda12.4-trt10.sif
  --from-sandbox PATH
                     Seal an already validated, user-owned sandbox instead of
                     running the definition. This does not require fakeroot.
  --sudo             Build the definition with sudo instead of --fakeroot.
  -h, --help         Show this help.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --definition)
            definition="${2:-}"
            shift 2
            ;;
        --output)
            output="${2:-}"
            shift 2
            ;;
        --from-sandbox)
            sandbox="${2:-}"
            shift 2
            ;;
        --sudo)
            use_sudo=1
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

command -v apptainer >/dev/null 2>&1 || {
    echo "apptainer is required" >&2
    exit 1
}
command -v sha256sum >/dev/null 2>&1 || {
    echo "sha256sum is required" >&2
    exit 1
}

mkdir -p -- "$(dirname -- "$output")"
temporary_output="${output}.partial.$$"
trap 'rm -f -- "$temporary_output"' EXIT

if [ -n "$sandbox" ]; then
    [ -d "$sandbox" ] || { echo "Sandbox not found: $sandbox" >&2; exit 1; }
    apptainer build "$temporary_output" "$sandbox"
else
    [ -r "$definition" ] || { echo "Definition not found: $definition" >&2; exit 1; }
    if [ "$use_sudo" -eq 1 ]; then
        sudo apptainer build "$temporary_output" "$definition"
    else
        apptainer build --fakeroot "$temporary_output" "$definition"
    fi
fi

apptainer exec --cleanenv "$temporary_output" /bin/bash -lc '
    set -eu
    test "$(getconf GNU_LIBC_VERSION)" = "glibc 2.35"
    test -x /opt/crimson/cmake-3.30.5-linux-x86_64/bin/cmake
    test -r /opt/crimson/opencv-4.10.0-jammy/lib/cmake/opencv4/OpenCVConfig.cmake
    test -r /opt/crimson/tensorrt-10.0.1.6/lib/libnvinfer.so
    test -r /opt/crimson/ffmpeg-jammy/lib/libavcodec.so
'
mv -- "$temporary_output" "$output"
sha256sum "$output" > "${output}.sha256"
trap - EXIT

echo "Crimson Linux builder ready:"
echo "  $output"
cat "${output}.sha256"
