#!/usr/bin/env bash
# Launcher for viture-bridge. LD_LIBRARY_PATH is required because the SDK's
# bundled OpenCV libs carry RUNPATH=/usr/local/lib, which stops the dynamic
# linker from using the executable's rpath for their own dependencies.
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$DIR/../sdk/lib/x86_64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$DIR/viture-bridge" "$@"
