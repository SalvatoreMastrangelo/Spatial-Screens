# Phase 1 design — VITURE Luma Ultra sensor visualizer

Date: 2026-07-03 · Status: implemented

## Goal

A web tool that visualizes all sensors of the Luma Ultra, structured so its
data layer becomes the foundation of the phase-2 spatial-screens program.

## Key constraints discovered during research

1. There is **no official web SDK**. The official **VITURE XR Glasses SDK
   v2.0.0** (unified, 2025) is a native C/C++ library; on Linux it ships as
   `libglasses.so` + `libcarina_vio.so` (VIO/SLAM on host CPU, OpenCV 4.2).
2. Luma Ultra = SDK device type **Carina**: 6DoF pose (position + quaternion,
   OpenGL/EUS coords), raw IMU (gyro/accel/mag/temp), VSync, stereo camera
   frames, `reset_pose_carina`.
3. Browsers can still reach the glasses **directly over WebHID** using the
   community-documented HID protocol (64-byte framed packets, CRC-16-CCITT,
   IMU-enable command `0x15`, euler floats at payload offset 0x12). This gives
   3DoF orientation only.
4. Hardware quirk (verified live): the HID MCU/IMU interfaces enumerate on a
   companion USB device `35ca:1102` ("VITURE Microphone"), not on the glasses'
   main `35ca:1104` device. WebHID filters must include 0x1102.
5. Both paths need the udev rule (`TAG+="uaccess"` for VID 35ca); without it
   the SDK segfaults in `xr_device_provider_start()`.

## Architecture (chosen from 3 options)

**Dual-source web app** — a Vite+Three.js dashboard with two interchangeable
drivers behind one normalized event interface:

- `drivers/viture-webhid.js` — zero-install path, Chrome-only, 3DoF + MCU events.
- `drivers/bridge-ws.js` ⇄ `bridge/viture-bridge` — a ~500-line C++ daemon
  linking the vendored SDK, streaming JSON over a localhost WebSocket
  (RFC 6455 server implemented in `bridge/ws_server.hpp`, no deps).

Rejected alternatives:
- *Pure WebHID app*: simplest, but can never show 6DoF/raw IMU — fails "all
  the sensors".
- *Native-only app (Qt/imgui)*: full data but throws away web iteration speed
  and doesn't match the "web visualization tool" ask; also phase 2 wants a
  reusable tracking daemon anyway — the bridge is exactly that.

## Data flow

```
HID packets ─▶ viture-webhid.js ─┐                      ┌─▶ Scene3D (three.js head + trail)
                                 ├─▶ state.js (ring     ├─▶ Scope charts (canvas, no deps)
SDK callbacks ─▶ bridge (C++) ───┘    buffers, rates,   ├─▶ panels (device, caps, stats)
               ws://localhost:8765    recenter offsets) └─▶ event log
```

- Recentering is done app-side (quaternion conjugate offset + position delta),
  plus `reset_pose` forwarded to the VIO on the bridge path.
- Charts window: 15 s; bridge throttles pose→60 Hz, imu→30 Hz to keep JSON
  cheap; rates/jitter measured client-side.

## Error handling

- WebHID unsupported → banner + bridge path still available.
- Bridge unreachable → clear message; auto-reconnect with backoff once it had
  connected.
- Invalid IMU floats (NaN / out-of-range) dropped before they reach state.
- Unknown MCU events logged as hex rather than silently dropped.

## Testing

- `npm run build` and bridge compile are CI-able today.
- Hardware-in-the-loop: bridge run on the real device (reached `start()`;
  blocked only on udev permissions), WebHID session pending user sign-off.
- No unit-test framework added in phase 1; the math module is the only
  candidate and is exercised continuously by the live view.
