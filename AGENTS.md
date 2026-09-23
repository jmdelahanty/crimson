# Agent Notes

## GUI Playback Smoke

Crimson's GUI smoke needs a real authenticated X display with GPU/OpenGL/CUDA
interop. A sandboxed or headless run may build `redgui` but fail to launch the
window.

After building `redgui`, run the playback smoke from the repo root:

```bash
CRIMSON_PLAYBACK_SMOKE_ZARR=/groups/johnson/johnsonlab/jeremy/recordings/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop/zarr/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop_analysis.zarr \
CRIMSON_PLAYBACK_SMOKE_RANGE=0:300 \
scripts/gui_smoke_playback.sh
```

If GLFW fails with a stale X cookie, for example:

```text
Invalid MIT-MAGIC-COOKIE-1 key
Glfw Error 65544: X11: Failed to open display :1
```

probe available display/auth pairs before rerunning Crimson:

```bash
for display in "$DISPLAY" :0 :1; do
  for xauth in "$XAUTHORITY" "$HOME/.Xauthority" /run/user/$(id -u)/.mutter-Xwaylandauth.*; do
    if [ -z "$display" ] || [ -z "$xauth" ] || [ ! -e "$xauth" ]; then
      continue
    fi
    printf 'DISPLAY=%s XAUTHORITY=%s ... ' "$display" "$xauth"
    if env DISPLAY="$display" XAUTHORITY="$xauth" xdpyinfo >/dev/null 2>&1; then
      echo OK
    else
      echo FAIL
    fi
  done
done
```

Use an `OK` pair explicitly:

```bash
CRIMSON_PLAYBACK_SMOKE_DISPLAY=:1 \
CRIMSON_PLAYBACK_SMOKE_XAUTHORITY=/run/user/$(id -u)/.mutter-Xwaylandauth.0U8EP3 \
CRIMSON_PLAYBACK_SMOKE_ZARR=/groups/johnson/johnsonlab/jeremy/recordings/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop/zarr/2026-06-14T21-12-08Z_arena_1_GoodCopBadCop_analysis.zarr \
CRIMSON_PLAYBACK_SMOKE_RANGE=0:300 \
scripts/gui_smoke_playback.sh
```

On 2026-06-21, the working pair on this host was:

```text
DISPLAY=:1
XAUTHORITY=/run/user/64406/.mutter-Xwaylandauth.0U8EP3
```

The smoke passed with:

```text
[PlaybackSmoke] PASS start_frame=0 end_frame=300 presented_frame=300 presented_slot=0 view_idx=0 presented_count=300 elapsed_s=3.00624
```

