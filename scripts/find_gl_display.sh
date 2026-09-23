#!/usr/bin/env bash
set -uo pipefail

uid="$(id -u)"

add_unique() {
    local value="$1"
    local array_name="$2"
    local current
    [ -n "$value" ] || return 0
    eval "for current in \"\${${array_name}[@]}\"; do
        [ \"\$current\" = \"\$value\" ] && return 0
    done"
    eval "${array_name}+=(\"\$value\")"
}

add_glob_matches() {
    local pattern="$1"
    local array_name="$2"
    local match
    while IFS= read -r match; do
        add_unique "$match" "$array_name"
    done < <(compgen -G "$pattern" || true)
}

version_is_at_least_3_3() {
    local text="$1"
    local major minor
    read -r major minor < <(
        printf '%s\n' "$text" |
            sed -n 's/.*: *\([0-9][0-9]*\)\.\([0-9][0-9]*\).*/\1 \2/p' |
            head -n 1
    )
    [ -n "${major:-}" ] || return 1
    [ "$major" -gt 3 ] || { [ "$major" -eq 3 ] && [ "$minor" -ge 3 ]; }
}

quote_export() {
    printf '%q' "$1"
}

displays=()
xauths=()
glx_vendors=()

add_unique "${DISPLAY:-}" displays
add_unique ":1" displays
add_unique ":0" displays
add_unique ":2" displays

add_unique "${XAUTHORITY:-}" xauths
add_unique "$HOME/.Xauthority" xauths
add_glob_matches "/run/user/${uid}/.mutter-Xwaylandauth.*" xauths
add_glob_matches "/run/user/${uid}/gdm/Xauthority" xauths

add_unique "${__GLX_VENDOR_LIBRARY_NAME:-}" glx_vendors
add_unique "nvidia" glx_vendors

if [ "${#xauths[@]}" -eq 0 ]; then
    echo "No XAUTHORITY candidates found." >&2
    exit 1
fi

if ! command -v xdpyinfo >/dev/null 2>&1; then
    echo "xdpyinfo is required but was not found on PATH." >&2
    exit 2
fi

if ! command -v glxinfo >/dev/null 2>&1; then
    echo "glxinfo is required for GLX/OpenGL probing but was not found on PATH." >&2
    echo "Install mesa-utils or run the X-only checks manually." >&2
    exit 2
fi

found_x=0
found_gl=0

for display in "${displays[@]}"; do
    for xauth in "${xauths[@]}"; do
        [ -n "$display" ] || continue
        [ -n "$xauth" ] || continue
        [ -e "$xauth" ] || continue

        printf '== DISPLAY=%s XAUTHORITY=%s\n' "$display" "$xauth"

        if ! env DISPLAY="$display" XAUTHORITY="$xauth" \
            xdpyinfo >/dev/null 2>&1; then
            echo "  X: FAIL"
            continue
        fi

        found_x=1
        echo "  X: OK"

        for glx_vendor in "${glx_vendors[@]}"; do
            if [ -n "$glx_vendor" ]; then
                printf '  GLX vendor override: %s\n' "$glx_vendor"
                glx_output="$(
                    env DISPLAY="$display" \
                        XAUTHORITY="$xauth" \
                        __GLX_VENDOR_LIBRARY_NAME="$glx_vendor" \
                        glxinfo -B 2>&1
                )"
            else
                echo "  GLX vendor override: <default>"
                glx_output="$(
                    env DISPLAY="$display" \
                        XAUTHORITY="$xauth" \
                        glxinfo -B 2>&1
                )"
            fi
            glx_status=$?
            if [ "$glx_status" -ne 0 ]; then
                echo "  GLX: FAIL"
                printf '%s\n' "$glx_output" | sed 's/^/    /'
                continue
            fi

            direct_line="$(printf '%s\n' "$glx_output" |
                grep -E '^direct rendering:' | head -n 1)"
            renderer_line="$(printf '%s\n' "$glx_output" |
                grep -E '^OpenGL renderer string:' | head -n 1)"
            vendor_line="$(printf '%s\n' "$glx_output" |
                grep -E '^OpenGL vendor string:' | head -n 1)"
            version_line="$(printf '%s\n' "$glx_output" |
                grep -E '^OpenGL (core profile )?version string:' | head -n 1)"

            printf '  %s\n' "${direct_line:-direct rendering: <unknown>}"
            printf '  %s\n' "${vendor_line:-OpenGL vendor string: <unknown>}"
            printf '  %s\n' "${renderer_line:-OpenGL renderer string: <unknown>}"
            printf '  %s\n' "${version_line:-OpenGL version string: <unknown>}"

            if printf '%s\n' "$direct_line" | grep -Fq 'Yes' &&
                version_is_at_least_3_3 "$version_line"; then
                found_gl=1
                echo "  Crimson: LIKELY OK"
                printf '  export DISPLAY=%s\n' "$(quote_export "$display")"
                printf '  export XAUTHORITY=%s\n' "$(quote_export "$xauth")"
                if [ -n "$glx_vendor" ]; then
                    printf '  export __GLX_VENDOR_LIBRARY_NAME=%s\n' \
                        "$(quote_export "$glx_vendor")"
                else
                    echo "  unset __GLX_VENDOR_LIBRARY_NAME"
                fi
            else
                echo "  Crimson: probably not usable for OpenGL 3.3 GLFW"
            fi
        done
    done
done

if [ "$found_gl" -eq 0 ]; then
    if [ "$found_x" -eq 0 ]; then
        echo "No working X display/auth pair found." >&2
        exit 3
    fi
    echo "Found X display/auth pairs, but none reported usable OpenGL 3.3 GLX." >&2
    exit 4
fi
