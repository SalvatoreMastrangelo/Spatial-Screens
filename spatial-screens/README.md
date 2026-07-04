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

Direct mode is the default: the app puts the glasses output into a RandR
display lease (`non-desktop=1`, released by Mutter) and presents straight to
it via `VK_KHR_display`, flipping on the glasses' own vblank — locked
~118-120 fps at 1920×1200@120. `--window` forces the old windowed fallback
(EWMH fullscreen, paced by the compositor instead of the glasses' clock); the
app also falls back to it automatically if direct acquisition fails. Only one
instance can run at a time — the SDK is single-client and the global hotkeys
are grabbed process-wide, so a stale instance makes new launches fall back or
lose hotkeys.

Options: `--monitor NAME` (glasses output; auto-detects the 1920×1200 display),
`--capture NAME|test` (source monitor; defaults to the first non-glasses
output), `--distance M` (default 0.75), `--size INCHES` (diagonal, default
24), `--pitch-trim DEG` (camera-rig angle calibration from sensor-viz,
default 0), `--predict-ms MS` (pose prediction, default 0 — extrapolation
noise reads as shake), `--smooth-pos 0..1` / `--smooth-ori 0..1` (pose-filter
strength: per-frame EMA blend for position/orientation, defaults 0.10 /
0.40, 1 = off), `--window` (force the windowed fallback instead of direct
display mode).

Keys: the default (direct) mode has no focused window, so only the global
`Ctrl+Alt` grabs work: `Ctrl+Alt+R` recenter and re-place the screen in front
of you (adding `Shift` also resets the VIO origin), `Ctrl+Alt+[` /
`Ctrl+Alt+]` distance, `Ctrl+Alt+-` / `Ctrl+Alt+=` size, `Ctrl+Alt+Q` quit. In
`--window` mode, the plain keys (`R`, `[`, `]`, `-`, `=`, `Q`/`Esc`)
additionally work whenever the window has focus.

## Scope of this spike (per docs/plan/phase2-spatial-screens.md)

- ✅ M2: direct-mode Vulkan presentation on the glasses output — RandR lease
  acquired via `VK_EXT_acquire_xlib_display`, `VK_KHR_display` FIFO swapchain
  flipping on the glasses' own vblank; world-locked quad from
  `get_gl_pose_carina` with prediction. Windowed EWMH-fullscreen path kept
  only as the `--window`/auto fallback. Test harness: `make vk-test &&
  ./vk-test [--direct] [--monitor NAME] [--seconds N]` exercises the
  presentation stack standalone (no SDK link, no `run.sh`/viture-bridge
  conflict).
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
- Crash recovery: Mesa holds the RandR display lease for the process
  lifetime, released via `vkReleaseDisplayEXT` + RandR restore on normal
  teardown. If the app is SIGKILLed (or crashes hard) in direct mode, the
  glasses output can be left off the desktop with `non-desktop=1`; recover
  with `xrandr --output DP-1 --set non-desktop 0 && xrandr --output DP-1 --auto`.
- Build deps beyond the SDK: `libvulkan-dev`; `glslang-tools` only if editing
  shaders (SPIR-V headers are checked in); `vulkan-tools` (`vulkaninfo`) is
  handy for confirming the Intel ANV device exposes
  `VK_EXT_acquire_xlib_display` / `VK_EXT_direct_mode_display` /
  `VK_KHR_display`.
