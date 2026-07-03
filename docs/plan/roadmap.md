# Project roadmap — VITURE Luma Ultra spatial toolkit

Two phases; each phase ships something usable on its own.

## Phase 1 — Sensor visualizer (done, pending hardware sign-off)

Web dashboard (`sensor-viz/`) + native bridge (`bridge/`), covering every
sensor exposed by the glasses:

| Sensor / signal | WebHID path | Bridge path (SDK v2.0) |
|---|---|---|
| Orientation (euler/quat) | ✅ ~60–120 Hz | ✅ |
| 6DoF position (VIO) | — | ✅ Carina pose callback / `get_gl_pose_carina` |
| Gyro / accel | — | ✅ raw IMU callback (Carina layout: accel[0-2], gyro[3-5], dt[6]) |
| Magnetometer / temperature | — | ✖ not present in the Carina IMU callback (hardware-verified); legacy raw mode only on older models |
| VSync | — | ✅ carina vsync callback |
| Device state (brightness, volume, display mode, film) | partial (MCU events, hex-decoded) | ✅ polled getters + state callback |
| Firmware / model info | model from PID | ✅ `get_glasses_version`, market name |
| Stereo camera frames | — | available in SDK, deliberately out of scope for phase 1 |

Remaining phase-1 checklist:

- [x] `make install-udev` + replug, then `bridge/run.sh` streams real 6DoF pose
      (verified 2026-07-03: ~62 Hz predicted pose, firmware 12.0.01.101_20260605)
- [x] M0 answered — the Luma Ultra does NOT speak the classic HID protocol:
      64-byte-framed IMU-enable on both `35ca:1102` hidraw interfaces gets no
      response (padded/prefixed variants tried). The bridge is the sole data
      source on the Ultra; WebHID remains for older models.
- [x] Axes/units sanity-checked live (gravity magnitude, gyro-at-rest, yaw
      coupling + camera-rig pitch offset found and corrected via Level trim)

## Phase 2 — Spatial screens program

Researched plan: [`phase2-spatial-screens.md`](phase2-spatial-screens.md). Summary:

- **Stack:** native Linux app (Rust or C++) linking the vendored SDK for
  predicted 6DoF pose; PipeWire/xdg-desktop-portal screen capture; fullscreen
  Vulkan/GL output on the glasses' display. Desktop-agnostic (works on
  Pop!_OS/COSMIC where Breezy's GNOME/KDE integrations don't).
- **MVP:** one virtual screen anchored in space (6DoF), widescreen preset,
  recenter hotkey, distance/size controls.
- **V2:** multiple screens, curved/ultrawide presets, layout save/restore,
  follow modes, comfort smoothing.
- **V3+:** per-app capture, hand-tracking interactions if/when exposed on
  desktop, edge blending.
- **Reuse:** `bridge/` becomes the tracking service; `sensor-viz/` becomes the
  telemetry/control UI; mgschwan's `viture_virtual_display` (MIT) is a working
  capture→GL-quad skeleton.

### Sequencing

1. Phase-1 sign-off on real hardware (above checklist).
2. M0–M6 milestones as laid out in the phase-2 doc, starting with a pose-driven
   fullscreen quad rendering a static image, then PipeWire capture, then
   presets/layouts.
