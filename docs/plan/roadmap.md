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

### Status

- M0–M2: done (bridge = 6DoF spike; direct-mode Vulkan renderer glasses-validated 2026-07-04).
- M3: done — portal/XShm capture chain, config + state files, WS telemetry to the
  phase-1 dashboard (spec: `docs/specs/2026-07-04-m3-remainder-design.md`).
  Hand-gesture control (pinch-drag, fist-hold) also merged.
- Stereo/SBS multi-screen: done — merged 2026-07-06 (`feat/stereo-3d`, 8/8
  hardware pass; design `docs/specs/2026-07-05-stereo-3d-design.md`).
- Next: M4 preset & layout engine; M5 outreach.

### Future ideas / possible additions (backlog)

Forward-looking feature ideas, now with design docs (2026-07-06) ready to seed
isolated feature worktrees. They share one dependency chain: **per-screen
selection is the foundation** both other features build on, so implement in the
order below.

1. **Per-screen selection & independent manipulation** *(foundation — build
   first)* — design:
   [`docs/specs/2026-07-06-screen-selection-design.md`](../specs/2026-07-06-screen-selection-design.md).
   An index+middle "select" pose picks the screen nearest your gaze; the active
   screen is highlighted; existing gestures (one-hand distance, two-hand grab)
   retarget to it via a new per-screen world pose override. Lets each screen
   move independently instead of everything acting on the whole rack.
2. **Vertical placement + face-the-user** *(builds on #1)* — design:
   [`docs/specs/2026-07-06-vertical-placement-design.md`](../specs/2026-07-06-vertical-placement-design.md).
   Place screens higher/lower (the two-hand grab already moves vertically); on
   reposition a screen re-orients to face the user's head, then world-locks
   ("screens above me, facing me").
3. **Floating window screens (decouple from physical displays)** *(builds on #1;
   largest)* — design:
   [`docs/specs/2026-07-06-floating-window-screens-design.md`](../specs/2026-07-06-floating-window-screens-design.md).
   A screen's source becomes `{monitor-region | window}`: per-window XComposite
   capture with its own texture, and `Ctrl+Alt+W` grabs the focused window onto
   the active screen — a free-floating panel, not a slice of a physical output.
