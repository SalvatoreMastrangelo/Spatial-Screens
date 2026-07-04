# spatial-screens (phase 2 — M3)

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
output; XShm-only — see `--capture-backend` below), `--capture-backend
auto|portal|xshm|test` (default `auto`: tries the PipeWire/xdg-desktop-portal
path first, falls back to XShm, then the test pattern; portal remembers the
picked monitor across launches via a restore token, and `--capture NAME`
applies to the XShm backend only — under portal the picker/token owns
source selection), `--distance M` (default 0.75), `--size INCHES` (diagonal,
default 24), `--pitch-trim DEG` (camera-rig angle calibration from
sensor-viz, default 0), `--predict-ms MS` (pose prediction, default 0 —
extrapolation noise reads as shake), `--smooth-pos 0..1` / `--smooth-ori
0..1` (pose-filter strength: per-frame EMA blend for position/orientation,
defaults 0.10 / 0.40, 1 = off), `--window` (force the windowed fallback
instead of direct display mode), `--ws-port N` (WebSocket telemetry port,
default 8765; `0` disables it), `--config PATH` (load a config file other
than the default), `--dump-config` (print the effective options — after
defaults/config/state/CLI have all been applied — plus the config/state
file paths, then exit).

Keys: the default (direct) mode has no focused window, so only the global
`Ctrl+Alt` grabs work: `Ctrl+Alt+R` recenter and re-place the screen in front
of you (adding `Shift` also resets the VIO origin), `Ctrl+Alt+[` /
`Ctrl+Alt+]` distance, `Ctrl+Alt+-` / `Ctrl+Alt+=` size, `Ctrl+Alt+Q` quit. In
`--window` mode, the plain keys (`R`, `[`, `]`, `-`, `=`, `Q`/`Esc`)
additionally work whenever the window has focus.

Gestures (if `gestures/hand_tracker.py`'s dependencies are installed —
`pip install -r gestures/requirements.txt`): pinch (thumb+index touching)
and drag vertically for distance (the screen's anchored position/orientation
stay fixed — only distance along that fixed axis changes); hold a fist for
about half a second to recenter. Size is hotkey-only (`-`/`=`). Gestures are
additive — the hotkeys above always work as a fallback. See
`../docs/specs/2026-07-03-hand-gesture-control-design.md` for the original
design (pinch-drag has since been narrowed to distance-only).

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
- ✅ M3: PipeWire portal capture (portal-preferred with XShm fallback),
  config file + persisted state, WebSocket telemetry to the phase-1
  dashboard
- ⏭ M4: multi-screen, curved/ultrawide presets, layout save/restore, follow
  modes

## Config

Config file: `~/.config/spatial-screens.conf` (respects `$XDG_CONFIG_HOME`),
`key = value` lines, `#` comments, no sections; keys are the long CLI flags
minus `--` (e.g. `pitch-trim`, `capture-backend`, `ws-port`). It's
user-authored — the app never writes to it. State file:
`~/.local/state/spatial-screens/state` (respects `$XDG_STATE_HOME`),
app-written only: live-tuned distance/size (saved on clean exit and via the
hotkeys) and the portal restore token (saved the moment a new one is
granted). Precedence: compiled defaults < config < state < CLI flags. Use
`--dump-config` to see the effective values after all four layers are
applied, plus the resolved config/state file paths.

## Telemetry

spatial-screens speaks the same bridge protocol (`ws://127.0.0.1:8765` by
default, `--ws-port N`/`0` to change/disable) that `bridge/` uses, so the
phase-1 sensor-viz dashboard doubles as its monitoring console: open the
dashboard and hit "Connect Bridge" while spatial-screens is running (never
at the same time as `viture-bridge` — the SDK is single-client) to see
device info, live 6DoF pose, and a compact "spatial-screens" panel (fps,
6DoF live/frozen, distance, size, capture backend, direct/window mode, and
`rss` — the process's resident memory in MB, a lightweight leak watchdog).
The dashboard's Recenter button sends `reset_pose`, which triggers the same
full VIO-reset recenter as the `Ctrl+Alt+Shift+R` hotkey.

## Notes

- Portal capture is session-agnostic (X11 or Wayland); the XShm fallback
  backend is X11-only (it reads the root window). The renderer/presentation
  path is unaffected either way — it stays X11-bound (direct mode needs
  `VK_EXT_acquire_xlib_display`).
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
- Build deps beyond the SDK: `libvulkan-dev`; `libpipewire-0.3-dev` (portal
  capture's PipeWire stream; dbus-1 dev headers are also required for the
  portal D-Bus session and are commonly already present on a GNOME desktop —
  both are picked up via `pkg-config` in the Makefile); `glslang-tools` only
  if editing shaders (SPIR-V headers are checked in); `vulkan-tools`
  (`vulkaninfo`) is handy for confirming the Intel ANV device exposes
  `VK_EXT_acquire_xlib_display` / `VK_EXT_direct_mode_display` /
  `VK_KHR_display`.
