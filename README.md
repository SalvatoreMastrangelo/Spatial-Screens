# VITURE Luma Ultra — Spatial Toolkit

Two-phase project around the VITURE Luma Ultra XR glasses on Linux:

1. **Phase 1 (this repo, working):** `sensor-viz/` — a web dashboard that
   visualizes every sensor the glasses expose: orientation (3DoF euler /
   quaternion), full **6DoF pose** (position + rotation from the on-host VIO),
   raw IMU (gyro / accel / magnetometer), temperature, VSync, and device state
   (brightness, volume, display mode, electrochromic film).
2. **Phase 2 (planned):** a standalone native program that places virtual
   screens in 3D space using the 6DoF tracking, with presets (ultrawide,
   curved, multi-panel). Full roadmap: [`docs/plan/phase2-spatial-screens.md`](docs/plan/phase2-spatial-screens.md).

```
┌─────────────────────┐   WebHID (3DoF euler + MCU events)   ┌──────────────┐
│  Luma Ultra glasses │ ────────────────────────────────────▶│              │
│  VID 35ca           │                                       │  sensor-viz  │
│  PID 1104 (glasses) │   USB (SDK v2.0: 6DoF VIO, raw IMU)   │  web app     │
│  PID 1102 (HID hub) │ ──▶ bridge/viture-bridge ──WebSocket─▶│  (Vite+Three)│
└─────────────────────┘        ws://localhost:8765            └──────────────┘
```

Two independent data paths:

| Path | Needs | Delivers |
|---|---|---|
| **WebHID** (Chrome/Edge) | udev rule only | 3DoF orientation + MCU events — older models only. **Not functional on Luma Ultra**: its HID endpoints don't speak the classic protocol (hardware-verified; the Carina SDK uses USB control transfers instead). Use the bridge. |
| **Native bridge** | udev rule + `make` | 6DoF pose, raw accel/gyro, VSync, device state, firmware info (mag/temp: not exposed by the Luma Ultra's Carina IMU callback; older models get them via raw mode) |

## One-time setup (required for BOTH paths)

The glasses' USB/hidraw nodes are root-only by default. Install the udev rule
and replug the glasses:

```bash
cd bridge && make install-udev   # sudo cp 70-viture-xr.rules /etc/udev/rules.d/ + reload
# then unplug/replug the glasses
```

## Run the dashboard

```bash
cd sensor-viz
npm install
npm run dev          # → http://localhost:5173 (Chrome for WebHID)
```

- **Connect WebHID** — pick "VITURE Luma Ultra" in the device chooser (the HID
  interfaces live on the `35ca:1102` companion device).
- **Connect Bridge** — for the full 6DoF + raw IMU set, first start the bridge:

```bash
cd bridge
make                 # builds against sdk/ (vendored VITURE SDK v2.0)
./run.sh             # ws://127.0.0.1:8765 ; --port N, --imu-freq 0..4, --verbose
```

Controls: **Recenter** zeroes heading + position (yaw-twist only — never tilts
the reference), **Level** calibrates out the constant pitch offset of the
sensor rig (~35° on Luma Ultra: the VIO reports the downward-angled tracking
cameras' orientation, not the wearer's gaze — look straight ahead and click;
persisted in localStorage), **Pause** freezes charts, the event log shows
MCU/state events as they arrive.

## Repo layout

- `spatial-screens/` — **phase 2** (in progress): world-anchored virtual screen
  renderer — 6DoF pose → fullscreen GL on the glasses, live X11 monitor
  capture, recenter/distance/size hotkeys (see its README; stop the bridge
  before running it)
- `sensor-viz/` — web app (Vite + Three.js, no other runtime deps)
- `bridge/` — native daemon: official SDK → WebSocket JSON (see header of
  `bridge/main.cpp` for the message protocol)
- `sdk/` — vendored **official VITURE XR Glasses SDK v2.0.0** (Luma-series
  capable; see `sdk/README.md` for provenance — *not* the legacy 3DoF One SDK)
- `docs/plan/` — [`roadmap.md`](docs/plan/roadmap.md) (both phases),
  [`phase2-spatial-screens.md`](docs/plan/phase2-spatial-screens.md) (researched
  feature plan for the spatial-screens program)
- `docs/specs/` — phase-1 design doc
- `reference/` — cloned prior art (git-ignored): XRLinuxDriver,
  viture-webxr-extension (WebHID protocol), viture_virtual_display

## Hardware notes (verified on this machine)

- Luma Ultra enumerates as `35ca:1104` (“VITURE Luma Ultra XR GLASSES”) plus a
  companion `35ca:1102` (“VITURE Microphone”) that carries the two 64-byte
  vendor HID interfaces (MCU commands/events + IMU stream) on USB interfaces
  1.3/1.4.
- The SDK identifies the device as type 2 (**Carina**) and runs its
  visual-inertial odometry on the host (`libcarina_vio.so` + OpenCV), fed by
  the glasses' stereo tracking cameras.
- Without the udev rule, `xr_device_provider_start()` segfaults inside the
  closed SDK (USB open fails ungracefully) — install the rule before running.
