#!/usr/bin/env bash
# LD_LIBRARY_PATH needed: the SDK's bundled OpenCV carries RUNPATH=/usr/local/lib.
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$DIR/../sdk/lib/x86_64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$DIR/spatial-screens" "$@"
