#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
test_root="$(mktemp -d)"
trap 'rm -rf -- "$test_root"' EXIT

mkdir -p -- "$test_root/bin" "$test_root/lib/crimson/private"
cp -- "$repo_root/tools/crimson_linux_launcher.sh" "$test_root/bin/crimson"

cat >"$test_root/bin/redgui" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf 'LD_LIBRARY_PATH=%s\n' "${LD_LIBRARY_PATH:-}"
printf 'ARG=%s\n' "$@"
EOF
chmod +x "$test_root/bin/crimson" "$test_root/bin/redgui"

output="$(env CRIMSON_LINUX_STRICT_RUNTIME=1 \
    LD_LIBRARY_PATH=/uncontrolled/host/path \
    "$test_root/bin/crimson" first "second argument")"

expected_path="$test_root/lib:$test_root/lib/crimson/private"
grep -Fxq "LD_LIBRARY_PATH=$expected_path" <<<"$output"
grep -Fxq "ARG=first" <<<"$output"
grep -Fxq "ARG=second argument" <<<"$output"

echo "crimson_linux_launcher_tests: PASS"
