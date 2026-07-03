#!/usr/bin/env bash
# LD_LIBRARY_PATH needed: the SDK's bundled OpenCV carries RUNPATH=/usr/local/lib.
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$DIR/../sdk/lib/x86_64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# Force vsync on both driver stacks (tearing on secondary X11 outputs).
export vblank_mode=3                # Mesa
export __GL_SYNC_TO_VBLANK=1        # NVIDIA
exec "$DIR/spatial-screens" "$@"
