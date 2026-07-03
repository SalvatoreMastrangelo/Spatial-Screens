#!/usr/bin/env bash
# Starts everything sensor-viz needs: builds/launches the native bridge
# daemon (ws://localhost:8765) in the background, then runs the Vite dev
# server in the foreground. Ctrl+C stops both.
set -uo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BRIDGE_DIR="$DIR/bridge"
WEB_DIR="$DIR/sensor-viz"

BRIDGE_PID=""

port_in_use() {
    (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null
    local status=$?
    exec 3<&- 2>/dev/null
    exec 3>&- 2>/dev/null
    return $status
}

cleanup() {
    if [[ -n "$BRIDGE_PID" ]] && kill -0 "$BRIDGE_PID" 2>/dev/null; then
        kill "$BRIDGE_PID" 2>/dev/null
        wait "$BRIDGE_PID" 2>/dev/null
    fi
}
trap cleanup EXIT INT TERM

if port_in_use 8765; then
    echo "==> Bridge already running on ws://localhost:8765 — reusing it."
else
    if [[ ! -x "$BRIDGE_DIR/viture-bridge" ]]; then
        echo "==> Building bridge daemon..."
        if ! (cd "$BRIDGE_DIR" && make); then
            echo "!! Bridge build failed; continuing without 6DoF/raw IMU data."
        fi
    fi

    if [[ -x "$BRIDGE_DIR/viture-bridge" ]]; then
        echo "==> Starting bridge daemon (ws://localhost:8765)..."
        "$BRIDGE_DIR/run.sh" &
        BRIDGE_PID=$!
        sleep 1
        if ! kill -0 "$BRIDGE_PID" 2>/dev/null; then
            echo "!! Bridge daemon exited immediately."
            echo "   Fix: cd bridge && make install-udev, then unplug/replug the glasses."
            BRIDGE_PID=""
        fi
    else
        echo "!! No bridge binary; skipping 6DoF/raw IMU bridge (dashboard will still run)."
    fi
fi

if [[ ! -d "$WEB_DIR/node_modules" ]]; then
    echo "==> Installing sensor-viz dependencies..."
    (cd "$WEB_DIR" && npm install)
fi

echo "==> Starting sensor-viz dashboard (watch for the 'Local:' link below)..."
echo ""

(cd "$WEB_DIR" && npm run dev)
