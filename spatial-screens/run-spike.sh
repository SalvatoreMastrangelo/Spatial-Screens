#!/usr/bin/env bash
# Launch the throwaway SBS/3D spike with the SDK library path set (same reason
# as run.sh: the SDK's bundled OpenCV carries RUNPATH=/usr/local/lib).
# Stop viture-bridge and any spatial-screens first — the SDK is single-client.
# Use --restore to panic-recover a panel stuck in 3D mode (kill -9 recovery).
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$DIR/../sdk/lib/x86_64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$DIR/sbs-spike" "$@"
