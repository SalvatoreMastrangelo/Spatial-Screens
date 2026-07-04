# spatial-screens M3 remainder — portal capture, config file, WS telemetry (design)

**Date:** 2026-07-04
**Status:** approved
**Scope:** finish milestone M3 of `docs/plan/phase2-spatial-screens.md` in
`spatial-screens/`: (1) PipeWire/xdg-desktop-portal screen capture alongside the
existing XShm path, (2) a config file plus persisted runtime state, (3) WebSocket
telemetry speaking the bridge protocol so the phase-1 `sensor-viz` dashboard
becomes the monitoring console. Gesture control (M4 of the hand-gesture plan) is
already merged and is untouched by this work.

## Why

- **Capture:** XShm reads the X root window — it dies the day the session moves
  to Wayland (COSMIC is coming to Pop!_OS), and it required special-case
  handling for desktop reflows (RandR events, source-rect re-resolution after
  the direct-mode lease). The portal ScreenCast API + PipeWire is the
  compositor-blessed path on both X11 and Wayland, and the compositor tracks
  the picked monitor itself.
- **Config:** eight CLI flags and growing; the calibrated `--pitch-trim`, the
  tuned distance/size, and the smoothing factors have to be retyped (or fished
  out of shell history) every launch.
- **Telemetry:** the phase-2 plan's stated intent is that the phase-1 dashboard
  becomes phase 2's monitoring console. Today spatial-screens prints fps/pose
  lines to stdout only.

## Environment facts (verified 2026-07-04)

- Session: X11, GNOME (Pop!_OS 22.04). `xdg-desktop-portal-gnome` is running;
  the ScreenCast portal reports **version 4** → `persist_mode`/`restore_token`
  supported (the monitor-picker dialog can be skipped on relaunch).
- `libdbus-1` dev headers already installed; `libpipewire-0.3-dev` **not**
  installed (candidate 1.0.2 in apt; runtime libpipewire is present).
- The renderer texture is `VK_FORMAT_B8G8R8A8_UNORM` (`vk_renderer.cpp:346`) —
  identical byte layout to the portal's default `BGRx`/`BGRA` SPA formats, so
  no conversion is needed.
- `bridge/ws_server.hpp` is a dependency-free single-header WebSocket server
  (RFC 6455 text frames, own thread, thread-safe `broadcast()` + on-message
  callback). The bridge protocol is documented in `bridge/main.cpp`'s header
  comment; `sensor-viz/src/drivers/bridge-ws.js` is the consuming client and
  ignores unknown message fields (unknown *types* land in the debug log).
- The SDK is single-client: `viture-bridge` and `spatial-screens` can never run
  simultaneously, so both can bind the same WS port (8765) without conflict.

## Approach decisions

- **Capture — chosen: dual backend, portal preferred.** `--capture-backend
  auto|portal|xshm|test`, default `auto`: try portal, fall back to XShm, then
  test pattern. Keeps today's zero-dialog X11 path as a fallback while making
  the Wayland-proof path the default (so it gets day-to-day testing).
  *Rejected — portal only:* a portal/PipeWire failure would leave only the test
  pattern. *Rejected — XShm default, portal opt-in:* the new path would rot
  untested.
- **Config — chosen: read-only config + app-written state file.** The config
  file is user-authored and never touched by the app; runtime adjustments
  (distance/size) and the portal restore token go to a separate state file.
  Precedence: defaults < config < state < CLI.
  *Rejected — no persistence:* loses live tuning and forces the portal dialog
  every launch. *Rejected — app rewrites the config file:* clobbers hand edits
  and comments; machine state doesn't belong in a human-edited file.
- **Telemetry — chosen: reuse the bridge protocol + one new message type +
  a small dashboard panel.** The dashboard's existing device/pose/log panels
  work unmodified the moment spatial-screens serves `hello`/`pose`/`log` on
  8765; a new `app` message carries spatial-screens-specific status, displayed
  by a new compact panel. *Rejected — no UI work:* fps/tracking quality would
  be buried in the event log. *Rejected — full status page:* that is V2's
  control-UI scope.
- **D-Bus client for the portal dance — chosen: libdbus-1.** Already installed,
  no GLib main-loop entanglement in a poll-based render loop.
  *Rejected — GDBus/libportal:* pulls a GLib main loop. *Rejected — sd-bus:*
  `libsystemd-dev` is not installed; libdbus is sufficient.

## Architecture

```
spatial-screens/src/
  capture.h            CaptureBackend interface + factory + backend enum
  capture_xshm.cpp     existing XShm logic moved out of main.cpp (behavior unchanged)
  capture_portal.cpp   NEW: portal ScreenCast session (libdbus-1) + PipeWire stream
  config.h/.cpp        NEW: config-file + state-file load/save (shared parser)
  telemetry.h/.cpp     NEW: wraps wsrv::Server; message formatting + throttles
  main.cpp             orchestration, pose math, input, render loop (shrinks)
```

`ws_server.hpp` stays in `bridge/` and is included via `-I../bridge` — one
copy, no duplication. Makefile gains `pkg-config`-driven flags for
`libpipewire-0.3` and `dbus-1`, and the new objects.

### 1. Capture backend interface

```cpp
struct CaptureFrame { const uint8_t* data; int w, h; uint32_t pitch; };
class CaptureBackend {
public:
    virtual bool start() = 0;                       // acquire the source
    virtual bool latest_frame(CaptureFrame& out) = 0; // newest complete frame, or false
    virtual bool size_changed() = 0;                // true once per source resize
    virtual void stop() = 0;
    virtual ~CaptureBackend() = default;
};
```

- The render loop keeps its existing 30 Hz upload cadence: poll
  `latest_frame`, `vkr_upload` on success; on `size_changed()`, rebuild the
  Vulkan texture (the logic that today lives in the RRScreenChangeNotify
  handler).
- **XShm backend:** current behavior, relocated. Still consumes
  RRScreenChangeNotify (forwarded from main's event loop) to re-resolve the
  source rect and detect resizes. `--capture NAME` selects the monitor as
  today.
- **Portal backend:** D-Bus session bus via libdbus-1 —
  `org.freedesktop.portal.ScreenCast`: `CreateSession` → `SelectSources`
  (`types=MONITOR`, `persist_mode=2`, `restore_token` when the state file has
  one) → `Start` (system picker appears unless the token restores) →
  `OpenPipeWireRemote` → PipeWire fd. Each portal request's `Request.Response`
  signal is pumped synchronously during startup (blocking with a timeout is
  fine here — this happens once, before the render loop).
  A `pw_thread_loop` + `pw_stream` connects to the returned node; the process
  callback copies each frame into one side of a double buffer under a mutex;
  `latest_frame` returns the other side. SPA format negotiation requests
  `BGRx`/`BGRA` at the source size. Stream `param_changed` with a new size
  sets the `size_changed` flag. Under portal, `--capture NAME` is ignored
  (the picker/token owns source selection) — a log line says so.
- **Selection & fallback (`auto`):** portal start failure (no portal on the
  bus, dialog cancelled, PipeWire connect failure) → log the reason, fall back
  to XShm; XShm failure → test pattern. Explicit `--capture-backend portal`
  or `xshm` skips the fallback chain for that backend (failure → test
  pattern, matching today's behavior). `test` forces the pattern (subsumes
  `--capture test`, which stays as an alias).
- **Runtime stream death** (compositor restart, session revoked): log, stop
  the portal backend, then degrade the same way startup would — under `auto`,
  XShm next, then test pattern; under an explicit `--capture-backend portal`,
  straight to the test pattern. Never exit mid-session for a capture problem
  (same philosophy as the existing draw-fail tolerance).

### 2. Config + state

- **Config:** `~/.config/spatial-screens.conf` (respecting
  `$XDG_CONFIG_HOME`), INI-style `key = value`, `#` comments, no sections.
  Keys mirror the long CLI flags minus `--`: `monitor`, `capture`,
  `capture-backend`, `distance`, `size`, `pitch-trim`, `predict-ms`,
  `smooth-pos`, `smooth-ori`, `window` (bool `true`/`1`), `ws-port`.
  Unknown key or unparsable value → stderr warning, line skipped, never
  fatal. `--config PATH` loads a different file; a missing default config is
  silently fine (a missing `--config PATH` warns).
- **State:** `~/.local/state/spatial-screens/state` (respecting
  `$XDG_STATE_HOME`), same parser. Keys: `distance`, `size`,
  `restore-token`. The whole file (all keys, from in-memory state) is
  rewritten atomically (temp file + rename):
  1. immediately when `Start` returns a new restore token (tokens are
     single-use — waiting for exit would lose the grant on a crash), and
  2. on clean exit (Q hotkey, SIGINT, SIGTERM), persisting the live
     distance/size.
  A corrupt or unwritable state file warns and is otherwise ignored.
- **Precedence:** compiled defaults < config < state < CLI flags. CLI parsing
  runs twice over the same table: once to find `--config`, then config/state
  load, then the full pass. (Implementation detail: a small
  option-descriptor table shared by CLI parsing, config parsing, and the
  usage string, so the three cannot drift.)

### 3. WebSocket telemetry

- spatial-screens embeds `wsrv::Server`, listening on `127.0.0.1:8765` by
  default (`--ws-port N` / config; `--ws-port 0` disables). Bind failure is a
  warning, never fatal — the app runs on without telemetry.
- **Server → client**, matching the bridge protocol where types overlap:
  - `hello` every 2 s: market name, pid, firmware
    (`xr_device_provider_get_glasses_version`), `device_type` — the
    dashboard's device panel and connection flow work unmodified.
  - `pose` at ≤60 Hz from the render loop's already-polled predicted pose
    (post-smoothing, i.e. what is actually rendered).
  - `log` for notable events: recenter (any trigger), backend fallback,
    capture rebuild, gesture-sidecar connect/disconnect, WS-triggered
    actions.
  - **New** `app` at ~2 Hz:
    `{"type":"app","fps":118.9,"sixdof":true,"anchored":true,"distance":0.75,"size":24,"backend":"portal","direct":true}`
    (`fps` reuses the existing 2 s fps accounting; `sixdof` is the existing
    liveness heuristic; `backend` is the *active* capture backend after any
    fallback).
- **Client → server:** `{"type":"reset_pose"}` (the dashboard's existing
  button) maps to the full Shift+R behavior — `reset_pose_carina` + recenter
  + re-place — matching what that button already means when the bridge serves
  it. The WS callback runs on the server thread; it only sets a
  `std::atomic<bool>` request flag that the render loop consumes at the top
  of each frame (same pattern as `g_running`).
- **sensor-viz** (vanilla JS, matching existing style):
  - `drivers/bridge-ws.js`: `case 'app'` → `this._emit('app', msg)`.
  - `ui/panels.js`: a compact "spatial-screens" panel (fps, 6DoF LIVE/frozen,
    distance, size, backend, direct/window) — hidden until the first `app`
    message of a connection, hidden again on disconnect. No chart, no
    controls (that is V2).

### 4. Build, docs, verification

- **New build dep:** `libpipewire-0.3-dev` (brings the SPA headers). dbus-1
  dev headers are already present on the dev machine; both resolved via
  `pkg-config` in the Makefile. Documented in `spatial-screens/README.md`.
- **`make capture-test`** (mirrors the `vk-test` precedent): standalone
  binary exercising a capture backend with no SDK link and no glasses —
  `./capture-test [--backend auto|portal|xshm] [--frames N]` grabs N frames,
  writes PPMs to `/tmp`, prints per-frame timing and the negotiated
  size/format. This is the headless verification path for the portal dance
  (picker, restore token, stream) before any glasses-on run.
- **No C++ unit tests** (repo convention). Manual verification matrix:
  portal picker on first run; token-restored silent start on second run;
  `--capture-backend xshm` parity with today; portal→XShm fallback (portal
  service stopped); config precedence (config vs state vs CLI); state
  write-back of live-tuned distance/size; dashboard shows device info + pose
  + app panel while spatial-screens runs; `reset_pose` button recenters.
- **No new Vitest tests**: the panel/driver additions are DOM/IO glue with no
  pure logic, matching the existing convention (unit tests cover `math.js`
  only).
- **Docs:** README (backend flag, config/state files, telemetry, deps,
  updated key list), roadmap M3 checkboxes, and fix the stale CLAUDE.md line
  claiming phase 2 "exists only as a plan".

## Out of scope

- Multi-screen, presets, layouts, follow modes — M4/V2.
- Any control surface in the dashboard beyond the existing `reset_pose`
  button (V2's control UI).
- Wayland *presentation* (direct mode is X11-bound by design;
  `VK_EXT_acquire_drm_display` is a future concern). This work Wayland-proofs
  the *capture* side only.
- Window (as opposed to monitor) capture — V3's per-app screens.
