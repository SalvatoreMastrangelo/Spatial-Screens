# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Toolkit for the VITURE Luma Ultra XR glasses, Linux x86_64 only. Two independent modules, each with its own build:

- `sensor-viz/` — web dashboard (vanilla JS ESM + Vite + Three.js; no framework, no TypeScript)
- `spatial-screens/gestures/` — Python 3 sidecar (MediaPipe Hands) that classifies pinch/pose gestures from the glasses' tracking camera; spawned by `spatial-screens` over a local Unix socket, see `docs/specs/2026-07-03-hand-gesture-control-design.md`
- `bridge/` — C++17 daemon that streams SDK data (6DoF pose, raw IMU, temp, device events) as JSON over WebSocket at `ws://localhost:8765`; protocol documented in the header comment of `bridge/main.cpp`
- `sdk/` — vendored closed-source VITURE SDK v2.0.0 (C headers + prebuilt x86_64 `.so`s). © VITURE Inc. — never modify these files, and flag any plan to publish this repo (redistribution is a licensing concern)
- `reference/` — gitignored third-party prior-art clones. Read-only reference, not this project's code
- `spatial-screens/` — phase-2 native app (C++17 + Vulkan): one world-anchored virtual screen on the glasses, direct-display by default, portal/XShm capture chain, gesture sidecar (`gestures/`, Python/MediaPipe), config in `~/.config/spatial-screens.conf`, WS telemetry on 8765. Build deps beyond the SDK: `libvulkan-dev`, `libpipewire-0.3-dev`, dbus-1 dev headers. Like the bridge: launch via `./run.sh`, never with viture-bridge running (single-client SDK)

## Commands

Web app, in `sensor-viz/`: `npm run dev` (http://localhost:5173), `npm run build`, `npm test` (Vitest, unit tests for `src/math.js`), `npm run lint` (ESLint flat config).

Bridge, in `bridge/`:
- `make` — builds against `../sdk`
- `./run.sh` (or `make run`) — the only correct way to launch: it sets `LD_LIBRARY_PATH=../sdk/lib/x86_64`. Running `./viture-bridge` directly fails to load libs (the SDK's bundled OpenCV `.so`s carry `RUNPATH=/usr/local/lib`)
- `make install-udev` — installs the udev rule (needs sudo)

No C++ tests, linter, or formatter for `bridge/`. Verification there is manual, on hardware.

Gesture sidecar, in `spatial-screens/gestures/`: `pip install -r requirements.txt` once, then `python3 -m pytest tests/ -v` for the pure-logic unit tests (`protocol.py`, `classify.py`). The sidecar itself (`hand_tracker.py`) isn't run standalone — `spatial-screens` spawns it automatically.

## Hardware gotchas

- The udev rule is mandatory: without it, `xr_device_provider_start()` segfaults inside the closed SDK. Install it, then unplug/replug the glasses.
- WebHID path (3DoF) is Chrome/Edge only. In the device chooser, pick the "VITURE Microphone" companion device (VID `0x35ca`, PID `0x1102`) — the vendor HID interfaces enumerate there, not on the main `0x1104` device.
- 6DoF poses use OpenGL/EUS coordinates: x→right, y→up, z→backward.

## Branching & worktrees

- New features are built in isolated git worktrees (under `.claude/worktrees/`), so multiple agents can work in parallel without colliding. Never develop a feature directly on `master` or `main`.
- `master` is the integration branch: merge a feature into `master` only once it's finalized and working in its worktree.
- `main` is the production / ship branch. Merging into `main` requires the user's explicit authorization each time — never merge to `main` on your own initiative.

## Conventions

- Design/spec docs go in `docs/specs/` named `YYYY-MM-DD-name.md`.
- JS: 2-space indent, single quotes, semicolons. C++: snake_case functions, `g_` prefix for atomic globals.
