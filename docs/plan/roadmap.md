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
- Per-screen selection & independent manipulation: done — landed on `main`
  2026-07-07, hardware pass PASSED (`two_up` gaze-select active screen + green
  border, per-screen pose override retargets distance/size/grab gestures; design
  `docs/specs/2026-07-06-screen-selection-design.md`). Was backlog item #1.
- Next: M4 preset & layout engine; M5 outreach.

### Future ideas / possible additions (backlog)

Forward-looking feature ideas, now with design docs (2026-07-06) ready to seed
isolated feature worktrees. The foundation — **per-screen selection** — is now
**done** (see Status), so items 1–2 below are unblocked and each builds on it.
Item 3 is an independent track (no dependency on selection).

1. **Vertical placement + face-the-user** *(builds on per-screen selection;
   half already landed)* — design:
   [`docs/specs/2026-07-06-vertical-placement-design.md`](../specs/2026-07-06-vertical-placement-design.md).
   Vertical *placement* is **already done as collateral** of per-screen
   selection: the retargeted two-hand grab writes a full unrestricted 3D
   `pose_pos` with a genuine head-up component (`main.cpp:859-866`,
   `gesture_manip.cpp:30-35`), so screens already move higher/lower freely.
   What remains: **face-the-user** — on grab-release a screen should re-orient
   its normal toward the head (a `face_user_quat` look-at with gravity-up roll
   lock) and world-lock, so a screen lifted overhead stays readable. The grab
   currently holds orientation fixed on purpose (`main.cpp:850`); the only
   facing write today is fist-hold recenter, which *relocates* onto the gaze
   axis rather than re-facing in place. Optional polish still open:
   `Ctrl+Alt+PageUp/Down` nudge hotkeys + a ±1.5 m elevation comfort clamp.
2. **Floating window screens (decouple from physical displays)** *(builds on
   per-screen selection; largest)* — design:
   [`docs/specs/2026-07-06-floating-window-screens-design.md`](../specs/2026-07-06-floating-window-screens-design.md).
   A screen's source becomes `{monitor-region | window}`: per-window XComposite
   capture with its own texture, and `Ctrl+Alt+W` grabs the focused window onto
   the active screen — a free-floating panel, not a slice of a physical output.
3. **Camera fusion for depth** *(independent track — the proper next project;
   deferred 2026-07-06)* — design (surfaced by the two-hand gestures feature):
   [`docs/specs/2026-07-06-two-hand-gestures-design.md`](../specs/2026-07-06-two-hand-gestures-design.md)
   ("Future ideas"). Fuse the two grayscale tracking cameras to recover each
   hand's true 3D position from stereo disparity — unlocking depth-aware
   gestures (push/pull-to-move-in-Z) a single 2D camera can't do robustly, and
   inherently deduplicating hands (one 3D entity, not one-per-camera). Not a
   quick fix: the SDK exposes **no stereo calibration** (raw left/right buffers
   only), so it needs self-calibration → rectification → correspondence →
   triangulation — a multi-day computer-vision effort. Today the sidecar already
   forwards both camera planes but uses only the left; the second is reserved
   for this. **Status:** code-complete on branch `feat/camera-fusion` (default
   flipped fusion-ON, `--no-fusion` escape hatch), awaiting a hardware pass —
   not yet merged.
