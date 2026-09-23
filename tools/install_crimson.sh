#!/usr/bin/env bash
set -Eeuo pipefail

script_path="${BASH_SOURCE[0]}"
while [ -L "$script_path" ]; do
    script_dir="$(cd -- "$(dirname -- "$script_path")" && pwd)"
    link_target="$(readlink -- "$script_path")"
    if [[ "$link_target" = /* ]]; then
        script_path="$link_target"
    else
        script_path="$script_dir/$link_target"
    fi
done

script_dir="$(cd -- "$(dirname -- "$script_path")" && pwd)"

default_source_root="${CRIMSON_LINUX_SOURCE_ROOT:-}"
if [ -z "$default_source_root" ]; then
    if [ -x "$script_dir/bin/redgui" ]; then
        default_source_root="$script_dir"
    else
        default_source_root="/groups/ahrens/ahrenslab/crimson/linux-app/current"
    fi
fi

default_data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
source_root="$default_source_root"
install_root="${CRIMSON_LINUX_INSTALL_ROOT:-$default_data_home/Crimson}"
launcher_link="${CRIMSON_LINUX_LAUNCHER_LINK:-$HOME/bin/crimson}"
runtime_check_mode="${CRIMSON_LINUX_RUNTIME_CHECK_MODE:-release}"
replace_existing=0
create_symlink=0
replace_launcher_link=0
create_desktop_entry=0
launch=0
skip_preflight_check=0
skip_post_install_check=0
require_nvidia_smi=0
require_gl=0

usage() {
    cat <<'EOF'
Usage: install_crimson.sh [options]

Copies a Crimson Linux app drop to a user-local install root, writes
install_metadata.json, and optionally creates a launcher symlink or desktop
entry.

Defaults:
  source root:  script directory when run from an app drop, otherwise
                /groups/ahrens/ahrenslab/crimson/linux-app/current
  install root: $XDG_DATA_HOME/Crimson or ~/.local/share/Crimson
  launcher:     ~/bin/crimson

Options:
  --source-root PATH          Source app drop. Default as above.
  --install-root PATH         Install destination. Default: ~/.local/share/Crimson.
  --replace-existing          Replace an existing install root.
  --create-symlink            Create a launcher symlink. Default path: ~/bin/crimson.
  --launcher-link PATH        Symlink path used by --create-symlink.
  --replace-launcher-link     Replace an existing launcher symlink/file.
  --create-desktop-entry      Create ~/.local/share/applications/crimson.desktop.
  --runtime-check-mode MODE   dev or release. Default: release.
  --skip-preflight-check      Skip source app-drop runtime check.
  --skip-post-install-check   Skip installed app runtime check.
  --require-nvidia-smi        Runtime checks fail if nvidia-smi is unavailable.
  --require-gl                Runtime checks fail if no GL/X probe succeeds.
  --launch                    Launch Crimson after install.
  -h, --help                  Show this help.

Examples:
  ./install_crimson.sh --replace-existing --create-symlink

  tools/install_crimson.sh \
    --source-root /groups/ahrens/ahrenslab/crimson/linux-app/current \
    --replace-existing \
    --create-symlink
EOF
}

absolute_path() {
    local path_value="$1"
    if [[ "$path_value" = /* ]]; then
        printf '%s\n' "$path_value"
    else
        printf '%s\n' "$PWD/$path_value"
    fi
}

canonical_existing_or_parent_path() {
    local path_value="$1"
    local parent
    local name

    if [ -d "$path_value" ]; then
        cd -- "$path_value" && pwd
    elif [ -e "$path_value" ]; then
        parent="$(dirname -- "$path_value")"
        name="$(basename -- "$path_value")"
        printf '%s/%s\n' "$(cd -- "$parent" && pwd)" "$name"
    else
        parent="$(dirname -- "$path_value")"
        name="$(basename -- "$path_value")"
        if [ -d "$parent" ]; then
            printf '%s/%s\n' "$(cd -- "$parent" && pwd)" "$name"
        else
            absolute_path "$path_value"
        fi
    fi
}

json_string() {
    local value="$1"
    value="${value//\\/\\\\}"
    value="${value//\"/\\\"}"
    value="${value//$'\n'/ }"
    printf '"%s"' "$value"
}

json_field_string() {
    local json_file="$1"
    local key="$2"

    [ -r "$json_file" ] || return 0
    sed -n -E "s/^[[:space:]]*\"$key\"[[:space:]]*:[[:space:]]*\"(.*)\"[[:space:]]*,?[[:space:]]*$/\\1/p" "$json_file" \
        | head -n 1
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

    require_path "$path_value" "$label"
    if [ ! -x "$path_value" ]; then
        echo "$label is not executable: $path_value" >&2
        exit 1
    fi
}

path_contains_or_equals() {
    local parent="$1"
    local child="$2"

    parent="$(canonical_existing_or_parent_path "$parent")"
    child="$(canonical_existing_or_parent_path "$child")"
    [ "$child" = "$parent" ] || [[ "$child" = "$parent"/* ]]
}

safe_remove_install_root() {
    local path_value="$1"
    local canonical
    local home_canonical

    canonical="$(canonical_existing_or_parent_path "$path_value")"
    home_canonical="$(canonical_existing_or_parent_path "$HOME")"

    case "$canonical" in
        ""|"/"|"/tmp"|"/var"|"/var/tmp"|"/usr"|"/usr/"|"/usr/local"|"/usr/local/"|"/opt"|"/opt/"|"/home"|"/groups"|"/nvme1"|"$home_canonical"|"$home_canonical/.local"|"$home_canonical/.local/share")
            echo "Refusing to remove unsafe install root: $canonical" >&2
            exit 1
            ;;
    esac

    rm -rf -- "$canonical"
}

run_runtime_check() {
    local app_root="$1"
    local label="$2"
    local check_script="$app_root/check_crimson_runtime.sh"
    local args

    if [ ! -x "$check_script" ]; then
        echo ""
        echo "Skipping Crimson runtime check ($label):"
        echo "  missing or not executable: $check_script"
        return
    fi

    args=("$check_script" --app-root "$app_root" --mode "$runtime_check_mode")
    if [ "$require_nvidia_smi" -eq 1 ]; then
        args+=(--require-nvidia-smi)
    fi
    if [ "$require_gl" -eq 1 ]; then
        args+=(--require-gl)
    fi

    echo ""
    echo "Running Crimson runtime check ($label)..."
    env -u CRIMSON_ALLOWED_RUNTIME_ROOTS "${args[@]}"
}

source_update_info() {
    local resolved_source_root="$1"
    local leaf
    local parent
    local grandparent

    leaf="$(basename -- "$resolved_source_root")"
    parent="$(dirname -- "$resolved_source_root")"
    if [ "$leaf" = "current" ]; then
        printf '%s\n%s/latest.json\n%s/current\n' "$parent" "$parent" "$parent"
        return
    fi

    if [ "$(basename -- "$parent")" = "releases" ]; then
        grandparent="$(dirname -- "$parent")"
        printf '%s\n%s/latest.json\n%s/current\n' "$grandparent" "$grandparent" "$grandparent"
        return
    fi

    printf '\n\n\n'
}

write_install_metadata() {
    local output_path="$1"
    local release_json="$2"
    local latest_manifest_path="$3"
    local current_root="$4"
    local share_root="$5"
    local release_name=""
    local release_published_at=""
    local commit=""
    local installed_at_utc

    installed_at_utc="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
    if [ -r "$release_json" ]; then
        release_name="$(json_field_string "$release_json" "release_name")"
        release_published_at="$(json_field_string "$release_json" "published_at_utc")"
        commit="$(json_field_string "$release_json" "commit")"
    fi

    mkdir -p -- "$(dirname -- "$output_path")"
    {
        printf '{\n'
        printf '  "schema_version": 1,\n'
        printf '  "installed_at_utc": %s,\n' "$(json_string "$installed_at_utc")"
        printf '  "install_root": %s,\n' "$(json_string "$install_root")"
        printf '  "source_root": %s,\n' "$(json_string "$source_root")"
        printf '  "installed_release_name": %s,\n' "$(json_string "$release_name")"
        printf '  "installed_release_published_at_utc": %s,\n' "$(json_string "$release_published_at")"
        printf '  "installed_commit": %s,\n' "$(json_string "$commit")"
        printf '  "installed_release_metadata_path": %s,\n' "$(json_string "$install_root/release.json")"
        printf '  "latest_manifest_path": %s,\n' "$(json_string "$latest_manifest_path")"
        printf '  "current_root": %s,\n' "$(json_string "$current_root")"
        printf '  "share_root": %s\n' "$(json_string "$share_root")"
        printf '}\n'
    } > "$output_path"
}

create_launcher_symlink() {
    local target="$install_root/bin/crimson"
    local link_path="$launcher_link"
    local link_parent

    require_executable "$target" "Installed launcher"
    link_parent="$(dirname -- "$link_path")"
    mkdir -p -- "$link_parent"

    if [ -e "$link_path" ] || [ -L "$link_path" ]; then
        if [ -L "$link_path" ] && [ "$(readlink -- "$link_path")" = "$target" ]; then
            echo "Launcher symlink already points to installed Crimson:"
            echo "  $link_path -> $target"
            return
        fi
        if [ "$replace_launcher_link" -eq 0 ]; then
            echo "Launcher path already exists: $link_path" >&2
            echo "Rerun with --replace-launcher-link to replace it." >&2
            exit 1
        fi
        rm -f -- "$link_path"
    fi

    ln -s -- "$target" "$link_path"
    echo "Created launcher symlink:"
    echo "  $link_path -> $target"
}

create_desktop_file() {
    local applications_dir="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
    local desktop_path="$applications_dir/crimson.desktop"
    local launcher="$install_root/bin/crimson"

    require_executable "$launcher" "Installed launcher"
    mkdir -p -- "$applications_dir"
    {
        printf '[Desktop Entry]\n'
        printf 'Type=Application\n'
        printf 'Name=Crimson\n'
        printf 'Comment=Crimson video review and analysis UI\n'
        printf 'Exec=%s\n' "$launcher"
        printf 'Path=%s\n' "$install_root/bin"
        printf 'Terminal=false\n'
        printf 'Categories=Science;Graphics;\n'
    } > "$desktop_path"
    chmod +x "$desktop_path"
    echo "Created desktop entry:"
    echo "  $desktop_path"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --source-root)
            source_root="${2:-}"
            shift 2
            ;;
        --install-root)
            install_root="${2:-}"
            shift 2
            ;;
        --replace-existing)
            replace_existing=1
            shift
            ;;
        --create-symlink)
            create_symlink=1
            shift
            ;;
        --launcher-link)
            launcher_link="${2:-}"
            create_symlink=1
            shift 2
            ;;
        --replace-launcher-link)
            replace_launcher_link=1
            shift
            ;;
        --create-desktop-entry)
            create_desktop_entry=1
            shift
            ;;
        --runtime-check-mode)
            runtime_check_mode="${2:-}"
            shift 2
            ;;
        --skip-preflight-check)
            skip_preflight_check=1
            shift
            ;;
        --skip-post-install-check)
            skip_post_install_check=1
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
        --launch)
            launch=1
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
        echo "--runtime-check-mode must be dev or release, got: $runtime_check_mode" >&2
        exit 2
        ;;
esac

[ -n "$source_root" ] || { echo "--source-root must be non-empty" >&2; exit 2; }
[ -n "$install_root" ] || { echo "--install-root must be non-empty" >&2; exit 2; }

source_root="$(canonical_existing_or_parent_path "$source_root")"
install_root="$(canonical_existing_or_parent_path "$install_root")"
launcher_link="$(absolute_path "$launcher_link")"

if [ "$source_root" = "$install_root" ]; then
    echo "Source root and install root are the same: $source_root" >&2
    exit 1
fi

if path_contains_or_equals "$source_root" "$install_root"; then
    echo "Install root must not be inside source root:" >&2
    echo "  source:  $source_root" >&2
    echo "  install: $install_root" >&2
    exit 1
fi

if path_contains_or_equals "$install_root" "$source_root"; then
    echo "Source root must not be inside install root:" >&2
    echo "  source:  $source_root" >&2
    echo "  install: $install_root" >&2
    exit 1
fi

require_executable "$source_root/bin/redgui" "Source redgui"
require_executable "$source_root/bin/crimson" "Source launcher"
require_executable "$source_root/check_crimson_runtime.sh" "Source runtime check"
require_path "$source_root/share/crimson/fonts" "Source fonts directory"
require_path "$source_root/share/crimson/config" "Source config directory"

readarray -t update_info < <(source_update_info "$source_root")
source_share_root="${update_info[0]:-}"
latest_manifest_path="${update_info[1]:-}"
current_root="${update_info[2]:-}"

if [ "$skip_preflight_check" -eq 0 ]; then
    run_runtime_check "$source_root" "source app drop"
fi

if [ -e "$install_root" ]; then
    if [ "$replace_existing" -eq 0 ]; then
        echo "Install root already exists: $install_root" >&2
        echo "Rerun with --replace-existing to update it." >&2
        exit 1
    fi
    echo "Removing existing Crimson install:"
    echo "  $install_root"
    safe_remove_install_root "$install_root"
fi

mkdir -p -- "$(dirname -- "$install_root")"
mkdir -p -- "$install_root"

echo ""
echo "Installing Crimson app drop:"
echo "  from: $source_root"
echo "  to:   $install_root"
cp -a -- "$source_root"/. "$install_root"/

require_executable "$install_root/bin/redgui" "Installed redgui"
require_executable "$install_root/bin/crimson" "Installed launcher"
require_path "$install_root/share/crimson/fonts" "Installed fonts directory"
require_path "$install_root/share/crimson/config" "Installed config directory"

install_metadata_path="$install_root/install_metadata.json"
write_install_metadata "$install_metadata_path" "$source_root/release.json" \
    "$latest_manifest_path" "$current_root" "$source_share_root"

if [ "$skip_post_install_check" -eq 0 ]; then
    run_runtime_check "$install_root" "installed app"
fi

if [ "$create_symlink" -eq 1 ]; then
    create_launcher_symlink
fi

if [ "$create_desktop_entry" -eq 1 ]; then
    create_desktop_file
fi

echo ""
echo "Crimson install completed."
echo "Installed:"
echo "  app:      $install_root/bin/redgui"
echo "  launcher: $install_root/bin/crimson"
echo "  fonts:    $install_root/share/crimson/fonts"
echo "  config:   $install_root/share/crimson/config"
echo "  metadata: $install_metadata_path"

if [ "$launch" -eq 1 ]; then
    echo ""
    echo "Launching Crimson..."
    "$install_root/bin/crimson" &
else
    echo ""
    echo "Launch command:"
    echo "  $install_root/bin/crimson"
fi
