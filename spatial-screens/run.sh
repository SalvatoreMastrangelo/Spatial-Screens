#!/usr/bin/env bash
# LD_LIBRARY_PATH needed: the SDK's bundled OpenCV carries RUNPATH=/usr/local/lib.
#
# Stereo multi-screen workspace: when the config enables stereo and a
# workspace grid, upscale the capture panel (xrandr --scale) so the desktop
# framebuffer is grid*1920x1200, and split it into VS1..VSn logical monitors
# (--setmonitor). Restore runs on EXIT even if the app segfaults — this
# wrapper deliberately does NOT exec. Config keys are grepped from the conf
# file; CLI-only overrides (e.g. --stereo passed to the app) are invisible
# here, so keep stereo/workspace in the conf.
# See docs/specs/2026-07-05-stereo-3d-design.md §1.
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$DIR/../sdk/lib/x86_64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

CONF="${XDG_CONFIG_HOME:-$HOME/.config}/spatial-screens.conf"
conf_get() { sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*\(.*\)/\1/p" "$CONF" 2>/dev/null | tail -1; }

STEREO="$(conf_get stereo)";       STEREO="${STEREO:-true}"
WORKSPACE="$(conf_get workspace)"; WORKSPACE="${WORKSPACE:-2x2}"
PANEL="$(conf_get capture)"
if [ -z "$PANEL" ]; then  # default: first connected non-glasses output
    PANEL="$(xrandr | awk '/ connected/ {print $1}' | grep -v '^DP-1$' | head -1)"
fi

TILE_W=1920 TILE_H=1200
MONITORS=()
SCALED=0
cleanup() {
    local m
    for m in "${MONITORS[@]:-}"; do
        [ -n "$m" ] && xrandr --delmonitor "$m" >/dev/null 2>&1 || true
    done
    [ "$SCALED" = 1 ] && xrandr --output "$PANEL" --scale 1x1 >/dev/null 2>&1 || true
}
trap cleanup EXIT

if [ "$STEREO" = "true" ] && [ "$WORKSPACE" != "off" ] && [ -n "$PANEL" ]; then
    COLS="${WORKSPACE%x*}" ROWS="${WORKSPACE#*x}"
    if [[ "$COLS" =~ ^[1-9]$ ]] && [[ "$ROWS" =~ ^[1-9]$ ]]; then
        FB_W=$((TILE_W * COLS)) FB_H=$((TILE_H * ROWS))
        # Native (preferred) mode of the panel — first mode line flagged '+'.
        NAT="$(xrandr | awk -v out="$PANEL" '
            $1 == out {f=1; next} f && /\+/ {print $1; exit} f && /connected/ {exit}')"
        NAT_W="${NAT%x*}" NAT_H="${NAT#*x}"
        if [ -n "$NAT_W" ] && [ -n "$NAT_H" ]; then
            SX=$(awk -v a="$FB_W" -v b="$NAT_W" 'BEGIN{printf "%.6f", a/b}')
            SY=$(awk -v a="$FB_H" -v b="$NAT_H" 'BEGIN{printf "%.6f", a/b}')
            echo "workspace: $PANEL -> ${FB_W}x${FB_H} (scale ${SX}x${SY}), grid ${COLS}x${ROWS}"
            xrandr --output "$PANEL" --scale "${SX}x${SY}"
            SCALED=1
            i=1
            for ((row = 0; row < ROWS; row++)); do
                for ((col = 0; col < COLS; col++)); do
                    name="VS$i"
                    owner="none"; [ "$i" -eq 1 ] && owner="$PANEL"
                    xrandr --setmonitor "$name" \
                        "${TILE_W}/300x${TILE_H}/190+$((col * TILE_W))+$((row * TILE_H))" \
                        "$owner"
                    MONITORS+=("$name")
                    i=$((i + 1))
                done
            done
        else
            echo "workspace: cannot read $PANEL native mode — skipping split" >&2
        fi
    else
        echo "workspace: bad grid '$WORKSPACE' (want e.g. 2x2) — skipping split" >&2
    fi
fi

"$DIR/spatial-screens" "$@"
