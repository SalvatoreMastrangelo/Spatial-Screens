#!/usr/bin/env bash
# LD_LIBRARY_PATH needed: the SDK's bundled OpenCV carries RUNPATH=/usr/local/lib.
#
# Stereo multi-screen workspace: when the config enables stereo and a
# workspace grid, upscale the capture panel (xrandr --scale) so the desktop
# framebuffer is grid*1920x1200, and split it into VS1..VSn logical monitors
# (--setmonitor). The workspace is applied only AFTER the app takes its
# display lease: mutter reapplies its stored monitor config on both the SBS
# mode adopt and the lease, clobbering anything set earlier (seen on
# hardware 2026-07-06) — the app waits for the grid before building its
# scene. Restore runs on EXIT even if the app segfaults — this wrapper
# deliberately does NOT exec. Config keys are grepped from the conf file;
# CLI-only overrides (e.g. --stereo passed to the app) are invisible here,
# so keep stereo/workspace in the conf.
# See docs/specs/2026-07-05-stereo-3d-design.md §1.
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$DIR/../sdk/lib/x86_64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

CONF="${XDG_CONFIG_HOME:-$HOME/.config}/spatial-screens.conf"
conf_get() { [ -f "$CONF" ] || return 0; sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*\(.*\)/\1/p" "$CONF" | tail -1 || true; }

STEREO="$(conf_get stereo)";       STEREO="${STEREO:-true}"
case "$STEREO" in true|1|yes) STEREO=true ;; *) STEREO=false ;; esac
WORKSPACE="$(conf_get workspace)"; WORKSPACE="${WORKSPACE:-2x2}"
GLASSES="DP-1"  # roadmap: unify with the app's detect-by-mode identity
PANEL="$(conf_get capture)"
if [ -z "$PANEL" ]; then  # default: first connected non-glasses output
    PANEL="$(xrandr | awk '/ connected/ {print $1}' | grep -v "^${GLASSES}\$" | head -1 || true)"
fi

TILE_W=1920 TILE_H=1200
MONITORS=()
SCALED=0
APP_PID=""
cleanup() {
    local m
    if [ -n "$APP_PID" ] && kill -0 "$APP_PID" 2>/dev/null; then
        kill -TERM "$APP_PID" 2>/dev/null || true
        wait "$APP_PID" 2>/dev/null || true
    fi
    for m in "${MONITORS[@]:-}"; do
        [ -n "$m" ] && xrandr --delmonitor "$m" >/dev/null 2>&1 || true
    done
    [ "$SCALED" = 1 ] && xrandr --output "$PANEL" --scale 1x1 >/dev/null 2>&1 || true
}
trap cleanup EXIT

COLS="${WORKSPACE%x*}" ROWS="${WORKSPACE#*x}"
WANT_WS=false
if [ "$STEREO" = "true" ] && [ "$WORKSPACE" != "off" ] && [ -n "$PANEL" ]; then
    if [[ "$COLS" =~ ^[1-9]$ ]] && [[ "$ROWS" =~ ^[1-9]$ ]]; then
        WANT_WS=true
    else
        echo "workspace: bad grid '$WORKSPACE' (want e.g. 2x2) — skipping split" >&2
    fi
fi

apply_workspace() {
    local FB_W FB_H NAT NAT_W NAT_H PGEOM PX PY i name owner row col
    # Native (preferred) mode of the panel — first mode line flagged '+'.
    NAT="$(xrandr | awk -v out="$PANEL" '
        $1 == out {f=1; next} f && /\+/ {print $1; exit} f && /connected/ {exit}')"
    NAT_W="${NAT%x*}" NAT_H="${NAT#*x}"
    if [ -z "$NAT_W" ] || [ -z "$NAT_H" ]; then
        echo "workspace: cannot read $PANEL native mode — skipping split" >&2
        return 0
    fi
    # Native-res grid: tile the NATIVE framebuffer (no upscale), so the composited
    # pixel count stays at native — far lighter iGPU load than scaling up to
    # grid*1920x1200 (e.g. 2560x1600 = 4.1MP vs 3840x2400 = 9.2MP for 2x2). Tiles
    # are native/grid (integer; any remainder is dropped from the last row/col).
    FB_W=$NAT_W FB_H=$NAT_H
    TILE_W=$((NAT_W / COLS)) TILE_H=$((NAT_H / ROWS))
    echo "workspace: $PANEL native ${FB_W}x${FB_H} (no upscale), grid ${COLS}x${ROWS}, tiles ${TILE_W}x${TILE_H}"
    xrandr --output "$PANEL" --scale 1x1  # normalize any leftover scale from a prior run
    # Tile at the panel's framebuffer origin, not (0,0): --scale/reflow
    # (or a multi-output layout) can move the panel, and the app's
    # containment filter drops every VS monitor not inside it. Read +X+Y
    # AFTER the scale; default to origin if the geometry can't be parsed.
    PGEOM="$(xrandr | awk -v out="$PANEL" \
        '$1==out {for(i=1;i<=NF;i++) if($i ~ /^[0-9]+x[0-9]+[+-][0-9]+[+-][0-9]+$/){print $i; exit}}' || true)"
    PX=0 PY=0
    if [[ "$PGEOM" =~ \+([0-9]+)\+([0-9]+)$ ]]; then
        PX="${BASH_REMATCH[1]}" PY="${BASH_REMATCH[2]}"
    fi
    i=1
    for ((row = 0; row < ROWS; row++)); do
        for ((col = 0; col < COLS; col++)); do
            name="VS$i"
            owner="none"; [ "$i" -eq 1 ] && owner="$PANEL"
            xrandr --setmonitor "$name" \
                "${TILE_W}/300x${TILE_H}/190+$((PX + col * TILE_W))+$((PY + row * TILE_H))" \
                "$owner"
            MONITORS+=("$name")
            i=$((i + 1))
        done
    done
}

if [ "$WANT_WS" = true ]; then
    "$DIR/spatial-screens" "$@" &
    APP_PID=$!
    # Wait for the app's display lease — the LAST layout reflow. A leased
    # connector reads "disconnected" in xrandr. Plain grep (not -q): -q
    # exits early and SIGPIPEs xrandr, which pipefail turns into a miss.
    # The timeout covers window-fallback runs that never take a lease.
    for _ in $(seq 1 150); do
        kill -0 "$APP_PID" 2>/dev/null || break
        if xrandr | grep "^${GLASSES} disconnected" >/dev/null; then break; fi
        sleep 0.2
    done
    sleep 1  # let mutter finish its post-lease re-layout
    if kill -0 "$APP_PID" 2>/dev/null; then
        apply_workspace
    fi
    rc=0; wait "$APP_PID" || rc=$?
    APP_PID=""
    exit "$rc"
fi

"$DIR/spatial-screens" "$@"
