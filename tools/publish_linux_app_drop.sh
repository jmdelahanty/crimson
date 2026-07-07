#!/usr/bin/env bash
set -Eeuo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
default_repo_root="$(cd -- "$script_dir/.." && pwd)"

repo_root="$default_repo_root"
stage_root=""
share_root=""
drop_name="current"
clean_destination=0
release_name=""
release_name_provided=0
publish_current=0
archive_existing_current=0
current_name="current"
releases_dir_name="releases"
skip_runtime_check=0
runtime_check_mode="release"
dependency_manifest=""
require_nvidia_smi=0
require_gl=0

usage() {
    cat <<'EOF'
Usage: publish_linux_app_drop.sh [options]

Options:
  --repo-root PATH            Crimson repo root. Default: parent of tools/.
  --stage-root PATH           Staged app root. Default: <repo>/dist/Crimson.
  --share-root PATH           Publish root, for example /groups/.../crimson/linux-app.
  --drop-name NAME            Direct-publish child name. Default: current.
  --clean-destination         Remove direct-publish destination before copying.
  --release-name NAME         Versioned release name. Default when needed:
                              YYYY-MM-DD_HHMMSS_<short-commit>.
  --publish-current           Publish a versioned release and refresh current.
  --archive-existing-current  Copy existing current to releases/current-before-*
                              before refreshing current.
  --current-name NAME         Current drop directory name. Default: current.
  --releases-dir-name NAME    Versioned release directory name. Default: releases.
  --skip-runtime-check        Do not run the staged release-mode runtime check.
  --runtime-check-mode MODE   Runtime check mode: dev or release. Default: release.
  --dependency-manifest PATH  Write dependency manifest during runtime check.
                              Default for versioned/current publish:
                              <stage-root>/dependency_manifest.json.
  --require-nvidia-smi        Runtime check fails if nvidia-smi is unavailable.
  --require-gl                Runtime check fails if no GL/X probe succeeds.
  -h, --help                  Show this help.

Examples:
  tools/publish_linux_app_drop.sh \
    --share-root /groups/ahrens/ahrenslab/crimson/linux-app \
    --publish-current

  tools/publish_linux_app_drop.sh \
    --stage-root dist/Crimson \
    --share-root /groups/ahrens/ahrenslab/crimson/linux-app \
    --release-name 2026-07-07_manual \
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

json_string() {
    local value="$1"
    value="${value//\\/\\\\}"
    value="${value//\"/\\\"}"
    value="${value//$'\n'/ }"
    printf '"%s"' "$value"
}

require_path() {
    local path_value="$1"
    local label="$2"

    if [ ! -e "$path_value" ]; then
        echo "$label not found: $path_value" >&2
        exit 1
    fi
}

require_executable() {
    local path_value="$1"
    local label="$2"

    if [ ! -x "$path_value" ]; then
        echo "$label not found or not executable: $path_value" >&2
        exit 1
    fi
}

assert_missing_path() {
    local path_value="$1"
    local label="$2"

    if [ -e "$path_value" ]; then
        echo "$label should not exist: $path_value" >&2
        echo "Clean and reinstall the staged app so the Linux layout is consistent." >&2
        exit 1
    fi
}

git_string() {
    git -C "$repo_root" "$@" 2>/dev/null || true
}

utc_timestamp() {
    date -u +"%Y-%m-%dT%H:%M:%SZ"
}

default_release_name() {
    local commit_short="$1"
    local timestamp
    timestamp="$(date +"%Y-%m-%d_%H%M%S")"
    if [ -n "$commit_short" ]; then
        printf '%s_%s\n' "$timestamp" "$commit_short"
    else
        printf '%s\n' "$timestamp"
    fi
}

safe_remove_tree() {
    local path_value="$1"

    case "$path_value" in
        ""|"/"|"$repo_root"|"$repo_root/"|"$stage_root"|"$stage_root/")
            echo "Refusing to remove unsafe path: $path_value" >&2
            exit 1
            ;;
    esac
    rm -rf -- "$path_value"
}

copy_app_tree() {
    local source_root="$1"
    local target_root="$2"
    local label="$3"
    local clean_target="${4:-0}"

    if [ "$clean_target" -eq 1 ] && [ -e "$target_root" ]; then
        echo "Removing existing $label:"
        echo "  $target_root"
        safe_remove_tree "$target_root"
    fi

    mkdir -p -- "$target_root"
    echo "Copying $label:"
    echo "  from: $source_root"
    echo "  to:   $target_root"
    cp -a -- "$source_root"/. "$target_root"/
}

test_app_drop_layout() {
    local root="$1"

    require_executable "$root/bin/redgui" "redgui"
    require_executable "$root/bin/crimson" "crimson launcher"
    require_executable "$root/check_crimson_runtime.sh" "check_crimson_runtime.sh"
    require_path "$root/README.txt" "README.txt"
    require_path "$root/release.json" "release.json"
    require_path "$root/share/crimson/fonts" "Fonts directory"
    require_path "$root/share/crimson/config" "Config directory"
    assert_missing_path "$root/redgui" "Legacy root redgui"
}

run_runtime_check() {
    local root="$1"
    local manifest_path="$2"
    local check_script="$root/check_crimson_runtime.sh"
    local check_args

    require_executable "$check_script" "Runtime check script"
    check_args=("$check_script" --app-root "$root" --mode "$runtime_check_mode")
    if [ -n "$manifest_path" ]; then
        check_args+=(--write-dependency-manifest "$manifest_path")
    fi
    if [ "$require_nvidia_smi" -eq 1 ]; then
        check_args+=(--require-nvidia-smi)
    fi
    if [ "$require_gl" -eq 1 ]; then
        check_args+=(--require-gl)
    fi

    run_command env -u CRIMSON_ALLOWED_RUNTIME_ROOTS "${check_args[@]}"
}

write_json_with_stage_metadata() {
    local output_path="$1"
    local release_kind="$2"
    local target_root="$3"
    local latest_current_root="${4:-}"
    local latest_release_root="${5:-}"
    local published_at_utc="$6"
    local commit="$7"
    local commit_short="$8"
    local branch="$9"

    if command -v python3 >/dev/null 2>&1; then
        run_command python3 - "$output_path" "$stage_root/release.json" "$release_name" \
            "$release_kind" "$published_at_utc" "$commit" "$commit_short" "$branch" \
            "$target_root" "$stage_root" "$latest_current_root" "$latest_release_root" <<'PY'
import json
import os
import sys

(
    output_path,
    stage_release_path,
    release_name,
    release_kind,
    published_at_utc,
    commit,
    commit_short,
    branch,
    target_root,
    stage_root,
    latest_current_root,
    latest_release_root,
) = sys.argv[1:13]

stage_metadata = {}
try:
    with open(stage_release_path, "r", encoding="utf-8") as f:
        stage_metadata = json.load(f)
except Exception:
    stage_metadata = {}

data = {
    "schema_version": 1,
    "platform": "linux",
    "release_name": release_name,
    "release_kind": release_kind,
    "published_at_utc": published_at_utc,
    "commit": commit,
    "commit_short": commit_short,
    "branch": branch,
    "published_target_root": target_root,
    "stage_root": stage_root,
    "stage_release_metadata": stage_metadata,
}
if latest_current_root:
    data["current_root"] = latest_current_root
if latest_release_root:
    data["release_root"] = latest_release_root

os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
with open(output_path, "w", encoding="utf-8") as f:
    json.dump(data, f, indent=2)
    f.write("\n")
PY
        return
    fi

    mkdir -p -- "$(dirname -- "$output_path")"
    {
        printf '{\n'
        printf '  "schema_version": 1,\n'
        printf '  "platform": "linux",\n'
        printf '  "release_name": %s,\n' "$(json_string "$release_name")"
        printf '  "release_kind": %s,\n' "$(json_string "$release_kind")"
        printf '  "published_at_utc": %s,\n' "$(json_string "$published_at_utc")"
        printf '  "commit": %s,\n' "$(json_string "$commit")"
        printf '  "commit_short": %s,\n' "$(json_string "$commit_short")"
        printf '  "branch": %s,\n' "$(json_string "$branch")"
        printf '  "published_target_root": %s,\n' "$(json_string "$target_root")"
        printf '  "stage_root": %s\n' "$(json_string "$stage_root")"
        printf '}\n'
    } > "$output_path"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --repo-root)
            repo_root="${2:-}"
            shift 2
            ;;
        --stage-root)
            stage_root="${2:-}"
            shift 2
            ;;
        --share-root)
            share_root="${2:-}"
            shift 2
            ;;
        --drop-name)
            drop_name="${2:-}"
            shift 2
            ;;
        --clean-destination)
            clean_destination=1
            shift
            ;;
        --release-name)
            release_name="${2:-}"
            release_name_provided=1
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

case "$runtime_check_mode" in
    dev|release) ;;
    *)
        echo "--runtime-check-mode must be 'dev' or 'release', got: $runtime_check_mode" >&2
        exit 2
        ;;
esac

[ -n "$repo_root" ] || { echo "--repo-root cannot be empty" >&2; exit 2; }
repo_root="$(cd -- "$repo_root" && pwd)"
if [ -z "$stage_root" ]; then
    stage_root="$repo_root/dist/Crimson"
else
    stage_root="$(absolute_path "$stage_root")"
fi
if [ -z "$share_root" ]; then
    echo "Pass --share-root with the Linux-visible publish root, for example /groups/.../crimson/linux-app" >&2
    exit 2
fi
share_root="$(absolute_path "$share_root")"

if [ "$archive_existing_current" -eq 1 ] && [ "$publish_current" -eq 0 ]; then
    echo "--archive-existing-current requires --publish-current" >&2
    exit 2
fi

require_path "$stage_root" "Stage root"
test_app_drop_layout "$stage_root"

commit="$(git_string rev-parse HEAD)"
commit_short="$(git_string rev-parse --short HEAD)"
branch="$(git_string rev-parse --abbrev-ref HEAD)"
if [ -z "$release_name" ] && { [ "$publish_current" -eq 1 ] || [ "$archive_existing_current" -eq 1 ]; }; then
    release_name="$(default_release_name "$commit_short")"
fi
if [ -z "$release_name" ]; then
    release_name="$(default_release_name "$commit_short")"
fi

if [ -z "$dependency_manifest" ] && { [ "$publish_current" -eq 1 ] || [ -n "$release_name" ]; }; then
    dependency_manifest="$stage_root/dependency_manifest.json"
fi
if [ -n "$dependency_manifest" ]; then
    dependency_manifest="$(absolute_path "$dependency_manifest")"
fi

echo "Crimson Linux app-drop publish"
echo "  repo:         $repo_root"
echo "  stage root:   $stage_root"
echo "  share root:   $share_root"
echo "  release name: $release_name"
echo "  publish current: $publish_current"
echo "  archive current: $archive_existing_current"

versioned_publish=0
if [ "$publish_current" -eq 1 ] || [ "$archive_existing_current" -eq 1 ] || [ "$release_name_provided" -eq 1 ]; then
    versioned_publish=1
fi

if [ "$skip_runtime_check" -eq 0 ]; then
    run_step "Runtime check"
    run_runtime_check "$stage_root" "$dependency_manifest"
fi

mkdir -p -- "$share_root"
published_at_utc="$(utc_timestamp)"
latest_manifest_path="$share_root/latest.json"
current_root="$share_root/$current_name"

if [ "$versioned_publish" -eq 1 ]; then
    releases_root="$share_root/$releases_dir_name"
    release_root="$releases_root/$release_name"
    mkdir -p -- "$releases_root"

    if [ -e "$release_root" ]; then
        echo "Release root already exists: $release_root" >&2
        exit 1
    fi

    if [ "$archive_existing_current" -eq 1 ] && [ -e "$current_root" ]; then
        archived_current_root="$releases_root/current-before-$release_name"
        if [ -e "$archived_current_root" ]; then
            echo "Archived current target already exists: $archived_current_root" >&2
            exit 1
        fi
        run_step "Archive current"
        copy_app_tree "$current_root" "$archived_current_root" "archived current app drop" 0
    fi

    run_step "Publish versioned release"
    copy_app_tree "$stage_root" "$release_root" "versioned Crimson Linux app release" 0
    write_json_with_stage_metadata "$release_root/release.json" "versioned" "$release_root" "" "$release_root" \
        "$published_at_utc" "$commit" "$commit_short" "$branch"
    test_app_drop_layout "$release_root"

    if [ "$publish_current" -eq 1 ]; then
        run_step "Refresh current"
        copy_app_tree "$release_root" "$current_root" "current Crimson Linux app drop" 1
        test_app_drop_layout "$current_root"
    fi

    latest_current_root=""
    if [ "$publish_current" -eq 1 ]; then
        latest_current_root="$current_root"
    fi
    write_json_with_stage_metadata "$latest_manifest_path" "latest" "$release_root" \
        "$latest_current_root" "$release_root" \
        "$published_at_utc" "$commit" "$commit_short" "$branch"

    echo ""
    echo "Published Crimson Linux versioned release:"
    echo "  $release_root"
    if [ "$publish_current" -eq 1 ]; then
        echo "Refreshed current app drop:"
        echo "  $current_root"
    fi
else
    target_root="$share_root"
    if [ -n "$drop_name" ]; then
        target_root="$share_root/$drop_name"
    fi

    run_step "Publish direct app drop"
    copy_app_tree "$stage_root" "$target_root" "Crimson Linux app drop" "$clean_destination"
    direct_current_root=""
    if [ "$drop_name" = "$current_name" ]; then
        direct_current_root="$target_root"
    fi
    write_json_with_stage_metadata "$target_root/release.json" "direct" "$target_root" \
        "$direct_current_root" "$target_root" \
        "$published_at_utc" "$commit" "$commit_short" "$branch"
    test_app_drop_layout "$target_root"
    write_json_with_stage_metadata "$latest_manifest_path" "latest" "$target_root" \
        "$direct_current_root" "$target_root" \
        "$published_at_utc" "$commit" "$commit_short" "$branch"

    echo ""
    echo "Published Crimson Linux app drop:"
    echo "  $target_root"
fi

echo ""
echo "Published latest metadata:"
echo "  $latest_manifest_path"
