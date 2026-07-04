#!/usr/bin/env bash
# LD_LIBRARY_PATH needed: the SDK's bundled OpenCV carries RUNPATH=/usr/local/lib.
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$DIR/../sdk/lib/x86_64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# While the app runs, force the desktop to 100% scale so the captured screens
# carry full-density content into the glasses (GNOME auto-HiDPI puts the
# laptop panel at 200% otherwise; text-scaling-factor is left alone).
# Opt out with SPATIAL_SCREENS_KEEP_SCALE=1. If run.sh dies without the trap
# firing (kill -9, OOM), recover with:
#   gsettings reset org.gnome.desktop.interface scaling-factor
if [[ "${SPATIAL_SCREENS_KEEP_SCALE:-0}" != 1 ]] \
    && command -v gsettings >/dev/null \
    && [[ "${XDG_SESSION_TYPE:-}" == x11 && "${XDG_CURRENT_DESKTOP:-}" == *GNOME* ]]; then
  prev_scale="$(gsettings get org.gnome.desktop.interface scaling-factor)"
  restore_scale() { gsettings set org.gnome.desktop.interface scaling-factor "$prev_scale"; }
  trap restore_scale EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM
  gsettings set org.gnome.desktop.interface scaling-factor 1
fi

"$DIR/spatial-screens" "$@"
