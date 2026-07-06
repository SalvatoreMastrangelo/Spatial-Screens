# Floating Window Screens (Decouple Screens from Physical Displays) — Design

Date: 2026-07-06
Status: approved, pre-implementation
Branch: `feat/floating-window-screens` (to be created)

## Preconditions

- [`2026-07-06-screen-selection-design.md`](2026-07-06-screen-selection-design.md)
  merged: **active-screen selection** (a grabbed window is assigned to the
  active screen) and the **per-screen pose override** (window screens are placed
  freely, not on the monitor grid).
- Recommended alongside
  [`2026-07-06-vertical-placement-design.md`](2026-07-06-vertical-placement-design.md):
  a grabbed window screen should be placeable + faced like any free screen.

This is the **largest** of the three features — it adds a new capture path and
touches the renderer's texture binding. Sequence it last.

## Summary

Today all screens are **UV sub-rects of one shared framebuffer** captured from a
physical monitor region (multi-screen forces the XShm backend;
`main.cpp`: "portal can't feed N uv rects"). A screen is therefore always a
*slice of a physical display*.

This feature lets a screen instead show a **single application window**, sourced
independently of any physical display. The window is captured directly
(XComposite-redirected so it renders even when off-screen/occluded), gets its
**own texture**, and the floating screen is placed anywhere in space. The
interaction is **grab-focused-window** (chosen 2026-07-06): a hotkey/gesture
snapshots whatever window is currently focused onto the active floating screen.

## Goals

- A screen's source becomes a union: **`{ monitor-region | window }`**. Existing
  monitor-region screens are unchanged.
- **Per-window capture** via XComposite: redirect the target window, read its
  pixmap, upload to a per-screen texture (window screens use full `[0,1]` UVs of
  their own texture; monitor screens keep the shared-texture sub-rect).
- **Grab focused window**: `Ctrl+Alt+W` (and optionally a gesture) reads
  `_NET_ACTIVE_WINDOW` and binds it to the active screen — or spawns a new
  floating screen in front if none is active.
- Correct **aspect** from the window's geometry.
- Graceful handling of window **close / unmap / resize**.
- Declarative config option so a window screen can be set up without runtime
  interaction.

## Non-goals (v1)

- **No input forwarding** — screens are display-only mirrors, as today. Clicking
  "into" a floating window is a much larger project.
- **No Wayland window capture** — the renderer is X11-bound (direct mode needs
  `VK_EXT_acquire_xlib_display`) and XShm/XComposite are X11. Portal *window*
  capture is a future idea.
- **No zero-copy / dmabuf import** in v1 — CPU-copy per window (reuses the
  existing grab→upload machinery). Zero-copy is a later optimization.
- **No per-window capture-rate control** beyond the global `--capture-hz`.

## Architecture

Generalize the single capture into a **per-source** model. The existing off-
render-thread grab thread (added on the stereo branch) fans out over sources;
each source owns a CPU buffer; the renderer binds one texture per screen.

```
capture thread (existing, now multi-source)
  ├─ MonitorRegionSource  → shared framebuffer buffer  (existing XShm root grab)
  └─ WindowSource(win)     → per-window buffer
        XCompositeRedirectWindow(win, Automatic)   // window always renders
        XCompositeNameWindowPixmap(win) → Pixmap
        XShmGetImage(pixmap) → CPU buffer           // reuse xshm upload path
        │
        ▼
renderer: N textures (bounded array); each ScreenInst → { texture index, uv }
  - monitor screen: shared texture, uv sub-rect (today's path)
  - window  screen: own texture, uv = [0,0,1,1]
```

### Why CPU-copy, not GL texture-from-pixmap

The renderer is **Vulkan**, and `GLX_EXT_texture_from_pixmap` is a GLX
extension — not usable here, and a Vulkan external-memory import of an X pixmap
is fiddly and driver-dependent. The app already has a proven XShm CPU grab +
staging upload for monitor capture; a redirected window's pixmap reads through
the **same** `XShmGetImage` path. So v1 reuses that machinery per window: simple,
already-optimized (grabs run off the render thread), and correct. dmabuf/Vulkan
external import is the documented performance follow-up.

## Components

### 1. `CaptureSource` interface — `spatial-screens/src/capture.h`

Refactor the current single-capture into a small interface the grab thread
iterates:

```cpp
struct CaptureSource {
    virtual ~CaptureSource() = default;
    virtual bool grab() = 0;                 // fill cpu buffer; false if stale
    virtual const uint8_t* pixels() const = 0;
    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual float aspect() const = 0;
};
```

- `MonitorRegionSource` — wraps today's XShm root-window grab (the shared
  framebuffer). Behavior identical to current.
- `WindowSource` — owns a redirected window; see below.
- The grab thread loops sources at `--capture-hz`; the renderer reads the latest
  buffer per source (same double-buffer discipline as today's single capture).

### 2. `WindowSource` — `spatial-screens/src/capture_window.cpp` (new)

- On construction with an X11 window id:
  - `XCompositeRedirectWindow(dpy, win, CompositeRedirectAutomatic)` — so the
    window keeps rendering even when unfocused / on another workspace / occluded.
  - Track it: select `StructureNotifyMask` for resize/unmap/destroy.
- `grab()`:
  - `XCompositeNameWindowPixmap(dpy, win)` → pixmap (re-name after any resize;
    the pixmap is invalidated by resize).
  - `XShmGetImage` (or `XGetImage` fallback) the pixmap into the CPU buffer;
    convert to the renderer's expected format (same conversion the monitor path
    uses).
  - Return `false` (keep last frame) if the window is unmapped/destroyed.
- `aspect()` from current window width/height.
- Requires the **XComposite** extension; query at startup. If absent, window
  sourcing is disabled with a clear log line (monitor screens still work).

### 3. Multi-texture renderer — `spatial-screens/src/vk_renderer.*`, `scene.h`

- `ScreenInst` gains a source binding:
  ```cpp
  int source_index = 0;   // index into the active source list
  // uv[4] already exists: window screens set it to {0,0,1,1}
  ```
- The renderer holds a **bounded array of textures** (e.g. up to 8: one shared +
  N window). Each screen's draw selects its texture by `source_index` (descriptor
  array + push-constant index, or per-screen descriptor set). Upload each source
  to its texture when its grab produces a new frame.
- Monitor screens keep the shared texture + sub-rect UV — zero behavior change
  for the existing rack.
- Stereo path is unaffected: texture selection is per draw, orthogonal to the
  two-viewport split.

### 4. Grab-focused-window action — `spatial-screens/src/main.cpp` + X11 helper

- New hotkey `Ctrl+Alt+W` (grabbed globally like the others in the `hot[]` set):
  1. Read `_NET_ACTIVE_WINDOW` from the root window (`XGetWindowProperty`).
  2. Skip our own output / the glasses window; validate it's a normal top-level.
  3. Create a `WindowSource`, add it to the source list + a new texture slot.
  4. **Assign to the active screen** (from the selection feature): repoint its
     `source_index`, set `uv = {0,0,1,1}`, set its aspect from the window. If
     **no** screen is active, spawn a new floating `ScreenInst` in front of the
     user (default distance) and face it (vertical-placement feature).
- Optional gesture binding later (e.g. `point` + hold at a window) — hotkey is
  the v1 path; the gesture is a future idea to avoid overloading the gesture
  vocabulary now.

### 5. Declarative config — `spatial-screens/src/config.cpp` + `ScreenCfg`

- Extend the per-screen config so a screen can name a window source instead of a
  monitor:
  - `ScreenCfg.source` (new): `"monitor"` (default) or `"window"`.
  - `ScreenCfg.window_match` (new): a title / `WM_CLASS` substring to bind at
    startup (e.g. `window-match = code` → first window whose class/title
    contains "code"). `window:active` binds whatever is focused at launch.
- At startup, resolve window-match screens to live windows (best-effort; a miss
  warns and the screen falls back to the test pattern or is skipped, mirroring
  the monitor-miss behavior in `scene_build`).
- Keeps the app's "config is declarative, state is app-written" split intact.

## Data flow / lifecycle

- **Focus grab** → source created → next capture-thread tick fills its buffer →
  renderer uploads → active screen shows the window.
- **Resize** → `StructureNotify` → re-name pixmap, re-alloc CPU buffer + texture,
  update `aspect()`.
- **Unmap** (minimized / workspace with no compositor backing) → `grab()` returns
  stale → screen shows its last frame frozen (documented) until remapped.
- **Destroy / close** → source marked dead → the screen reverts to the test
  pattern with a log line (or, if it was a spawned floating screen, is removed).
  Never crash on a vanished window.

## Error handling / risks

- **XComposite availability.** Queried once; absent → feature disabled cleanly,
  monitor screens unaffected. On GNOME/Mutter (this project's target) Composite
  is present.
- **Unmapped windows produce no pixmap content.** A minimized window can't be
  captured; `CompositeRedirectAutomatic` keeps a mapped-but-occluded or
  off-workspace window rendering, but a truly unmapped one is frozen. Documented;
  the tester should keep target windows mapped (any workspace is fine).
- **Direct-mode interaction.** The glasses output is a RandR display lease; the
  desktop compositor still composites windows on the laptop panel, so redirecting
  a window is independent of the lease. No new lease/teardown concerns. (Existing
  direct-mode crash-recovery notes unchanged.)
- **Capture cost.** Each window screen adds an `XShmGetImage` + upload per tick.
  Bounded by `--capture-hz` and a small texture cap; large windows dominate.
  Note the cost in telemetry (extend the rss/fps panel with source count) and
  cap the number of window screens.
- **Texture cap.** Fixed descriptor-array size (e.g. 8). Exceeding it warns and
  refuses the grab rather than reallocating the pipeline.

## Testing

- **C++ pure/unit:** `scene`/renderer binding — a window screen resolves to its
  own texture index with full-frame UV while monitor screens keep sub-rect UVs;
  aspect derives from window geometry; texture-cap refusal path.
- **Component (headless-ish):** `WindowSource` against a throwaway mapped X
  window — redirect, grab a known-size pixmap, assert buffer dimensions/aspect;
  resize → re-name → new dimensions; destroy → stale/dead handling. Guard behind
  an X-display-available check (like the existing XShm tests).
- **Hardware pass:** `Ctrl+Alt+W` grabs the focused window onto the active
  screen; move/scale/face it (selection + vertical-placement features); resize
  the source app and confirm the floating screen follows; close it and confirm a
  clean revert; a config `window-match` screen resolves at launch.

## Future ideas (documented, not built)

- **Zero-copy capture** via dmabuf + Vulkan external-memory import (drops the CPU
  copy per window) — the real performance path once v1 proves the UX.
- **Portal window capture** for Wayland parity (needs the renderer to not depend
  on `acquire_xlib_display`, i.e. a windowed-mode variant).
- **Gesture to grab/point at a window** instead of the hotkey, once the gesture
  vocabulary has room.
- **Input forwarding** (interact with the floating window) — a separate, large
  project.
- **Per-window capture rate** (a slow-updating doc window vs a live terminal).

## Files touched

- `spatial-screens/src/capture.h` — `CaptureSource` interface; multi-source grab
  loop.
- `spatial-screens/src/capture_window.cpp` (new) — `WindowSource` (XComposite +
  XShm pixmap read).
- `spatial-screens/src/capture_xshm.cpp` — refactor today's grab into
  `MonitorRegionSource` behind the interface.
- `spatial-screens/src/vk_renderer.*` — bounded per-source texture array;
  per-screen texture binding.
- `spatial-screens/src/scene.h` / `scene.cpp` — `source_index` on `ScreenInst`;
  window screens = full-frame UV.
- `spatial-screens/src/config.h` / `config.cpp` — `source` / `window_match`
  keys; startup resolution.
- `spatial-screens/src/main.cpp` — `Ctrl+Alt+W` grab-focused-window, spawn/assign
  to active screen, source/texture lifecycle, telemetry source count.
- `spatial-screens/Makefile` — link `-lXcomposite`; new object.
- `docs/specs/2026-07-06-floating-window-screens-design.md` — this document.
- `docs/branches/feat-floating-window-screens.md` — branch resume doc (at
  worktree start).
