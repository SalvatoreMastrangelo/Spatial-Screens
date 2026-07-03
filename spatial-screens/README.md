# spatial-screens (phase 2 — M2/M3 spike)

One virtual screen, world-anchored in 3D space via the Luma Ultra's 6DoF VIO,
rendered fullscreen on the glasses' display and textured with a live X11
capture of a source monitor.

## Run

```bash
make
# IMPORTANT: stop viture-bridge first — the SDK supports one client at a time.
./run.sh --pitch-trim 16          # your calibrated rig angle from sensor-viz
```

Options: `--monitor NAME` (glasses output; auto-detects the 1920×1200 display),
`--capture NAME|test` (source monitor; defaults to the first non-glasses
output), `--distance M` (default 1.75), `--size INCHES` (diagonal, default
120), `--predict-ms MS` (pose prediction, default 8).

Keys: `R` recenter and re-place the screen in front of you (`Shift+R` also
resets the VIO origin), `[` / `]` distance, `-` / `=` size, `Q`/`Esc` quit.

## Scope of this spike (per docs/plan/phase2-spatial-screens.md)

- ✅ M2: fullscreen GL on the glasses output, world-locked quad from
  `get_gl_pose_carina` with prediction, vsync
- ✅ M3 (partial): live monitor capture (X11 XShm) textured onto the screen,
  widescreen sizing, recenter hotkey
- ⏭ M3 remainder: PipeWire portal capture (Wayland-proof), config file
- ⏭ M4: multi-screen, curved/ultrawide presets, layout save/restore, follow
  modes, WebSocket telemetry to the phase-1 dashboard

## Notes

- X11 session only for capture (XShm reads the root window); the renderer
  itself works regardless. PipeWire portal is the planned replacement.
- The virtual screen is placed at recenter time: at your gaze heading,
  `--distance` meters ahead, vertical (gravity-aligned). Position tracking
  then keeps it fixed in the room as you move.
- Coordinate frames, the pitch-trim rationale (camera rig angle), and all
  hardware findings are documented in `docs/` and `sensor-viz/`.
