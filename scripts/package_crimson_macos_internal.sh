#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
app_path="${1:-${repo_root}/build/macos-arm64-release/Crimson.app}"
output_dir="${2:-${repo_root}/dist}"
executable="${app_path}/Contents/MacOS/Crimson"

if [[ ! -x "${executable}" ]]; then
  printf 'Crimson executable not found: %s\n' "${executable}" >&2
  printf 'Build it with: cmake --build --preset build-macos-arm64-release\n' >&2
  exit 1
fi

architectures="$(lipo -archs "${executable}")"
if [[ "${architectures}" != "arm64" ]]; then
  printf 'Expected an arm64-only app, found: %s\n' "${architectures}" >&2
  exit 1
fi

glfw_path="$(otool -L "${executable}" | awk '$1 ~ /libglfw.*[.]dylib$/ { print $1; exit }')"
if [[ -z "${glfw_path}" ]]; then
  printf 'Could not identify Crimson runtime GLFW dependency.\n' >&2
  exit 1
fi
if [[ "${glfw_path}" != /opt/homebrew/* ]]; then
  printf 'Unexpected GLFW runtime path: %s\n' "${glfw_path}" >&2
  printf 'Update the internal testing instructions before packaging.\n' >&2
  exit 1
fi

version="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
  "${app_path}/Contents/Info.plist")"
commit="$(git -C "${repo_root}" rev-parse --short HEAD 2>/dev/null || printf 'unknown')"
timestamp="$(date -u +%Y%m%dT%H%M%SZ)"
package_name="Crimson-${version}-macos-arm64-internal-${commit}-${timestamp}"
staging_root="$(mktemp -d "${TMPDIR:-/tmp}/crimson-internal-package.XXXXXX")"
package_root="${staging_root}/${package_name}"
staged_app="${package_root}/Crimson.app"
archive_name="${package_name}.zip"
archive_path="${output_dir}/${archive_name}"
checksum_path="${archive_path}.sha256"

cleanup() {
  rm -rf "${staging_root}"
}
trap cleanup EXIT

mkdir -p "${package_root}" "${output_dir}"
ditto "${app_path}" "${staged_app}"
cp "${repo_root}/docs/crimson_macos_internal_testing.md" \
  "${package_root}/README.md"

# Seal the bundle resources for trusted internal transfer. This is not a
# Developer ID signature and does not replace Apple notarization.
codesign --force --deep --sign - --timestamp=none "${staged_app}"
codesign --verify --deep --strict --verbose=2 "${staged_app}"

ditto -c -k --sequesterRsrc --keepParent "${package_root}" "${archive_path}"
(
  cd "${output_dir}"
  shasum -a 256 "${archive_name}" > "${archive_name}.sha256"
)

printf 'Created internal Crimson package:\n  %s\n  %s\n' \
  "${archive_path}" "${checksum_path}"
printf 'Runtime GLFW dependency:\n  %s\n' "${glfw_path}"
