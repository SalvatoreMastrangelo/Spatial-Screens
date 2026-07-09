# Floating Window Screens (Decouple Screens from Physical Displays) — Design

Date: 2026-07-06 · **Freshness-reconciled 2026-07-09**
Status: approved, pre-implementation
Branch: `feat/floating-window-screens`

> **⚠️ REVISED 2026-07-09 (freshness pass).** The original 2026-07-06 design was
> written before screen-selection, head-anchored-reorient, and camera-fusion
> merged, and it described a **`CaptureSource` interface with a shared grab
> thread that fans out over sources** — that abstraction never existed. The real
> capture layer is **`CaptureBackend`** (one self-threaded backend per source,
> one active at a time) and the renderer is **single-texture / single-descriptor-
> set**. This revision reconciles the architecture to the code as it actually
> stands on `main` (HEAD `f38058d`), and — per the 2026-07-09 scope decision —
> **keeps runtime spawn in v1** (append-only; see §Runtime spawn). Interaction
> model is unchanged: **`Ctrl+Alt+W` grab-focused-window** onto the active screen.
> Grounded against: `capture.h:12-39`, `capture_xshm.cpp`, `vk_renderer.{h,cpp}`,
> `scene.{h,cpp}`, `main.cpp` (hotkeys `425-441/721-760`, backend chain
> `509-562`, `active_screen` `643`), `config.{h,cpp}`, `Makefile:6,13`.

## Preconditions (all now MET on `main`)

- **Active-screen selection** — `docs/specs/2026-07-06-screen-selection-design.md`
  (merged 2026-07-07). A grabbed window is assigned to the active screen
  (`int active_screen` in `main.cpp:643`, set by the `two_up` gaze pick).
- **Per-screen pose override** — window screens are placed freely, not on the
  monitor grid (`ScreenCfg.has_pose_override / pose_ori / pose_pos`;
  `scene_screen_pose` override branch, `scene.cpp:62-77`).
- **Head-anchored reorientation** — `docs/specs/2026-07-08-head-anchored-reorient-design.md`
  (merged 2026-07-08). A grabbed active screen welds to the head and world-locks
  facing you on release. Reused here to **face a spawned/assigned window screen**.

This remains the **largest** backlog feature — it adds a new capture backend,
converts the renderer to per-source textures, and adds the first runtime
scene-append path. Sequence it after the above (done).

## Summary

Today every screen is a **UV sub-rect of one shared framebuffer** captured from a
physical-monitor region (multi-screen forces the XShm backend — `main.cpp:514`
comment: "portal can't feed N uv rects"). A screen is therefore always a *slice
of a physical display*.

This feature lets a screen instead show a **single application window**, sourced
independently of any physical display. The window is captured directly
(XComposite-redirected so it renders even when off-screen/occluded), gets its
**own texture**, and the floating screen is placed anywhere in space. The
interaction is **grab-focused-window**: `Ctrl+Alt+W` snapshots whatever window is
currently focused onto the active screen — or, if none is selected, **spawns** a
new floating screen in front of you.

## Goals

- A screen's source becomes a union: **`{ monitor-region | window }`**. Existing
  monitor-region screens are byte-for-byte unchanged (they keep `source_index=0`,
  the shared texture, and their sub-rect UV).
- **Per-window capture** via a new `WindowBackend`: XComposite-redirect the target
  window, `XShmGetImage` its named pixmap into a triple-buffered SHM image (the
  exact machinery `XShmBackend` already uses for the root window), upload to a
  per-source texture. Window screens use full `[0,1]` UVs of their own texture.
- **Grab focused window**: `Ctrl+Alt+W` reads `_NET_ACTIVE_WINDOW` and binds it to
  the active screen — or spawns a new floating screen in front if none is active.
- Correct **aspect** from the window's geometry.
- Graceful handling of window **resize / unmap / close** (never crash on a
  vanished window; a closed window's screen reverts to a placeholder).
- Declarative config so a window screen can be set up without runtime interaction
  (`screen.N.source = window`, `screen.N.window-match = <substr>`).

## Non-goals (v1)

- **No input forwarding** — screens are display-only mirrors, as today. Clicking
  "into" a floating window is a separate, much larger project.
- **No Wayland window capture** — the renderer is X11-bound (direct mode needs
  `VK_EXT_acquire_xlib_display`); XShm/XComposite are X11. Confirmed session is
  X11. Portal *window* capture is a future idea.
- **No zero-copy / dmabuf import** in v1 — CPU-copy per window via the proven XShm
  read path. Zero-copy (dmabuf + Vulkan external memory) is the perf follow-up.
- **No runtime screen removal / compaction** — spawn is **append-only**. A closed
  window's screen becomes an inert placeholder, reusable, but is never spliced out
  (splicing is the only thing that would invalidate the `active_screen` index).
  Full remove/compaction is a documented follow-up.
- **No per-window capture-rate control** beyond the global `--capture-hz`.
- **No cursor composite on window screens** — the global pointer is not blended
  into a floating window's texture (there is no input there).

## Current architecture (what actually exists — the baseline this changes)

- **Capture: `CaptureBackend`** (`capture.h:18-36`) — `start()`, `latest_frame(
  CaptureFrame&)`, `composites_cursor()`, `alive()`, `name()`, `stop()`,
  `on_outputs_changed()`. `CaptureFrame` (`capture.h:12-16`) is
  `{ const uint8_t* data /*32bpp BGRX*/, w, h, pitch }`. Three impls: `XShmBackend`
  (`capture_xshm.cpp`), `PortalBackend` (`capture_portal.cpp`), `TestBackend`
  (`capture.cpp`).
- **One active backend at a time** — `std::unique_ptr<CaptureBackend> cap`
  (`main.cpp:520`), chosen by walking a fallback `chain` (`main.cpp:509-562`);
  re-selected on death (`main.cpp:1102-1106`). **There is no shared multi-source
  grab thread.** Each backend runs its *own* thread + Display connection
  (`XShmBackend`: `thread_`/`dpy_`, `capture_xshm.cpp:47,167`).
- **XShm grab** (`capture_xshm.cpp:114-165`) — `XShmGetImage(dpy_,
  DefaultRootWindow(dpy_), img_[b], src_.x, src_.y, AllPlanes)`; depth-24 ZPixmap
  = 32bpp BGRX handed straight to Vulkan (no conversion); triple SHM buffer
  (`img_[3]`, `front_`/`reading_`). `XShmGetImage` reads any **Drawable** — a
  window *or a pixmap* — which is why the window backend can reuse it verbatim.
- **Renderer: ONE shared texture** (`vk_renderer.h:43-46`,
  `vk_renderer.cpp:351`), **one descriptor set** (`descriptorCount=1`, pool
  `maxSets=1`; `vk_renderer.cpp:227-236,326-338`), bound **once** per frame before
  the quad loop (`vk_renderer.cpp:548-549`). Each screen is one push-constant quad
  differentiated only by `mvp` + `uv[4]` (`PushBlock`, `vk_renderer.cpp:14-21`);
  `flags[0]=textured` lets a quad render **untextured** (solid color) — the
  placeholder path we reuse for dead windows. Upload = memcpy into a mapped
  staging buffer + `tex_dirty`, copied to the image inside `draw_impl`
  (`vk_renderer.cpp:464-535`).
- **Scene** — `ScreenInst { ScreenCfg cfg; float uv[4]; float aspect; }`
  (`scene.h:13-17`). `scene` is built **once** (`main.cpp:490`); **no runtime
  add/remove** exists (the count-shrink comment at `main.cpp:807` calls it a
  "future feature"). Per-screen pose override written via `world_to_rack_frame`
  (`scene.cpp:79-86`) from the gesture/hotkey paths.
- **Hotkeys** (`main.cpp:425-441`) — `KeySym hot[] = { r, [, ], -, =, q }` grabbed
  at `Ctrl+Alt` (+ NumLock/CapsLock variants); handled in the switch at
  `main.cpp:721-760`. Adding a key = append the keysym + one handler branch.
- **Config** (`config.cpp:97-140`) — INI `key = value`; per-screen keys are
  `screen.N.field` with a strict whitelist (`monitor, azimuth, elevation,
  distance, size`; `config.cpp:104-107`).
- **Build** (`Makefile:6,13`) — links `-lvulkan -lX11 -lXext -lXrandr -lXfixes
  -lpthread`. **`-lXcomposite` is not linked; no XComposite/Xdamage headers are
  included anywhere.** `libXcomposite-dev` (pkg-config 0.4.5) and runtime
  `Composite` are both present on the dev machine (verified 2026-07-09).

## Target architecture

Two structural changes, both bounded:

```
BEFORE                              AFTER
one active CaptureBackend  ───────► primary monitor backend  (slot 0, shared tex)
                                    + std::vector<WindowBackend>  (slots 1..N-1)
one shared texture + 1 dset ──────► fixed array of N sources, each:
                                      { texture, staging, dset, dirty, w/h/pitch }
ScreenInst{cfg,uv,aspect}   ──────► + int source_index   // 0 monitor · ≥1 window
                                                          // <0 → placeholder
scene built once            ──────► + append-only runtime spawn (never shrink)
```

- **Sources**, not one backend: keep the auto-selected **monitor** backend as
  today (it feeds source slot **0**, the shared texture). Add a bounded list of
  **`WindowBackend`** instances, each feeding its own source slot (`1..N-1`).
- **Renderer** grows the scalar texture/staging/descriptor state into a fixed
  array of `N = 8` **sources** and binds the per-source descriptor set **inside**
  the quad loop, keyed by `ScreenInst.source_index`. Monitor screens
  (`source_index=0`) are unchanged. **Approach chosen: per-source descriptor set
  with per-draw rebind** — it leaves `quad.frag` untouched, needs no
  descriptor-indexing feature/extension, and ≤8 quads × ≤2 eyes/frame makes the
  extra `vkCmdBindDescriptorSets` cost negligible. (Alternative considered: one
  descriptor with an 8-element sampler array + a `tex_index` push constant; it
  saves rebinds but needs a shader change and `shaderSampledImageArrayDynamic
  Indexing`. Rejected for v1 as higher-risk for no measurable win at this N.)

### Why CPU-copy, not GPU texture-from-pixmap

The renderer is **Vulkan**; `GLX_EXT_texture_from_pixmap` is a GLX extension (not
usable), and a Vulkan external-memory import of an X pixmap is driver-dependent
and fiddly. The app already has a proven XShm CPU grab + staging upload, and a
redirected window's pixmap reads through the **same** `XShmGetImage(drawable,…)`
call. So v1 reuses that machinery per window: simple, already off the render
thread, and correct. dmabuf/Vulkan external import is the documented perf path.

## Components

### 1. `WindowBackend` — `spatial-screens/src/capture_window.cpp` (new)

A new `CaptureBackend` implementation, structured as a sibling of `XShmBackend`
(own thread, own `Display` connection, triple SHM buffer, `latest_frame()`
handing out `img_[reading_]->data`). Factory in `capture.h`:

```cpp
std::unique_ptr<CaptureBackend> capture_create_window(Window win, int hz);
```

- **Construction / redirect:** on the backend's own connection,
  `XCompositeRedirectWindow(dpy_, win, CompositeRedirectAutomatic)` — the window
  keeps rendering even when unfocused / on another workspace / occluded. Select
  `StructureNotifyMask` on `win` for resize/unmap/destroy. Query XComposite once
  (`XCompositeQueryExtension`); absent → `start()` returns false (feature
  disabled cleanly; monitor screens unaffected).
- **`grab()` loop** (mirrors `capture_xshm.cpp:140-165`): drain pending
  `StructureNotify`; `XCompositeNameWindowPixmap(dpy_, win)` → pixmap
  (**re-named after any resize** — the pixmap is invalidated by resize);
  `XShmGetImage(dpy_, pixmap, img_[b], 0, 0, AllPlanes)` into the SHM slot;
  publish `front_`. On unmap/destroy, return the last frame stale and set an
  internal dead flag; `alive()` stays true (a dead *window* is not a dead
  *backend* — the owner decides to tear it down and free the slot). **No cursor
  composite** (`composites_cursor()` → false).
- **Size / aspect:** track current window `w×h`; `latest_frame` reports them so
  the renderer can (re)size the source texture and the screen can set `aspect`.
- **Reuse:** to avoid duplicating the SHM triple-buffer logic, extract a small
  `shm_image` helper (create/destroy/grab-into-slot for an arbitrary Drawable)
  shared by `XShmBackend` and `WindowBackend`. Keep the extraction minimal so the
  proven monitor path is untouched behaviorally. (If extraction proves invasive,
  the fallback is a self-contained copy in `capture_window.cpp` — the plan
  decides; either is acceptable.)
- **Pixel format / alpha:** a 24-bit-visual window's pixmap is BGRX (identical to
  the monitor path); a 32-bit-visual (ARGB) window is BGRA — force alpha opaque on
  upload (window screens are opaque quads in v1).

### 2. Per-source renderer — `spatial-screens/src/vk_renderer.{h,cpp}`

- Replace the scalar `tex/tex_mem/tex_view/tex_w/h/pitch` + `staging*` +
  `tex_dirty` + single `dset` with a fixed `Source src_[N]` (N=8), each holding
  its own image/view/memory, mapped staging buffer, dirty flag, and descriptor
  set. Pool `maxSets = N`; write each source's view into its own set.
- New API:
  - `void vkr_set_source_size(VkRend*, int idx, int w, int h, uint32_t pitch)` —
    lazily (re)create slot `idx`'s image + staging at the source's dimensions
    (guarded by the existing upload-fence discipline; slot 0 keeps its current
    init). Window sources call this when a window is bound or resizes.
  - `void vkr_upload_source(VkRend*, int idx, const uint8_t* data, size_t bytes)`
    — the per-slot twin of today's `vkr_upload` (which becomes `idx=0`).
- `QuadDraw` (`vk_renderer.h:9-16`) gains `int source_index`. `record_quads`
  binds the source's descriptor set before each quad; an untextured quad
  (`textured=false`, i.e. a placeholder for a dead window) needs no valid source.
  Stereo replay is unaffected (per-eye lists just replay the per-quad binds).

### 3. Scene / source binding — `spatial-screens/src/scene.{h,cpp}`

- `ScreenInst` gains `int source_index = 0;` — **0** = shared monitor texture
  (today's sub-rect UV path, unchanged); **≥1** = a window source (UV forced to
  `{0,0,1,1}`); **<0** = placeholder (dead window → render untextured).
- `scene_build` is unchanged for monitor screens. Config-declared window screens
  (`source = window`) are created with `source_index` unset (`-1` placeholder)
  and resolved to a live window + slot at startup (see §5); a miss warns and the
  screen stays a placeholder (mirrors the monitor-miss drop behavior).

### 4. Grab-focused-window + runtime spawn — `spatial-screens/src/main.cpp`

- **Hotkey:** add `XK_w` to `hot[]` (`main.cpp:429`) — it inherits `Ctrl+Alt` +
  all lock variants — and an `else if (ks == XK_w)` handler branch after
  `main.cpp:760`.
- **Handler:**
  1. Read `_NET_ACTIVE_WINDOW` off `root` (`XGetWindowProperty`).
  2. Validate: skip our own output/glasses window and non-normal windows; require
     a mapped top-level with sane geometry.
  3. Allocate a free **source slot** (1..N-1). None free → warn "window screen cap
     reached" and abort (never realloc the pipeline).
  4. `capture_create_window(win, capture_hz)` → `start()`. Failure → free slot +
     warn.
  5. **Bind:**
     - `active_screen >= 0` → repoint `scene[active_screen].source_index` to the
       new slot, set `uv={0,0,1,1}`, set `aspect` from the window.
     - `active_screen < 0` → **spawn**: prefer reviving the nearest existing
       **placeholder** (dead-window) screen; else `scene.push_back(ScreenInst{})`.
       Place it a default distance along head-forward and face it via the
       head-anchored facing machinery (write `pose_ori/pose_pos` through
       `world_to_rack_frame`; `has_pose_override=true`), size/distance from the
       global defaults, `aspect` from the window. Set `active_screen` to it.
       Append never invalidates existing indices.
- **Lifecycle each frame:** for every live window source, `latest_frame()` →
  `vkr_set_source_size` (if changed) → `vkr_upload_source`. A window backend
  reporting its window **destroyed** → `stop()` the backend, free its slot, set
  the bound screen's `source_index = -1` (placeholder). A truly **unmapped**
  (minimized) window freezes on its last frame (documented).
- **Telemetry:** extend the rss/fps panel (`telemetry.*`) with a live
  **window-source count**.

### 5. Declarative config — `spatial-screens/src/config.{h,cpp}`

- Add to `ScreenCfg` (`config.h:13-24`): `std::string source = "monitor";` and
  `std::string window_match;`.
- Add both to the per-screen whitelist + assignment (`config.cpp:104-114`):
  `screen.N.source = monitor|window`, `screen.N.window-match = <title/WM_CLASS
  substring>`. `window-match = :active` binds whatever is focused at launch.
- Startup resolution (in the scene-build/`main` init path): for each
  `source=window` screen, find the first window whose `WM_CLASS`/title contains
  the substring, create a `WindowBackend`, assign a slot. Miss → warn, leave the
  screen as a placeholder. Keeps the "config declarative, state app-written"
  split intact.

## Data flow / lifecycle

- **Grab / spawn** → slot allocated → `WindowBackend` started → next frames:
  `latest_frame` → (resize? `vkr_set_source_size`) → `vkr_upload_source` → the
  screen shows the window.
- **Resize** → `StructureNotify` → re-name pixmap, re-alloc SHM slot, report new
  `w/h` → renderer resizes the source texture → `aspect` updates.
- **Unmap** (minimized) → `grab` stale → screen shows its last frame frozen
  (documented) until remapped.
- **Destroy / close** → backend reports dead → owner `stop()`s it, frees the slot,
  screen → placeholder (untextured quad). Never crash. Reusable by the next grab.

## Error handling / risks

- **XComposite availability** — queried once per backend; absent → window sourcing
  disabled cleanly, monitor screens unaffected. Present on this project's
  GNOME/Mutter X11 target (verified 2026-07-09).
- **Unmapped windows produce no pixmap content** — `CompositeRedirectAutomatic`
  keeps a mapped-but-occluded / off-workspace window rendering, but a truly
  unmapped (minimized) one is frozen. Documented; keep target windows mapped (any
  workspace is fine).
- **Direct-mode independence** — the glasses output is a RandR display lease; the
  desktop compositor still composites windows on the laptop panel, so redirecting
  a window is orthogonal to the lease. No new lease/teardown concerns.
- **Capture cost** — each window source adds one `XShmGetImage` + upload per tick,
  bounded by `--capture-hz` and the slot cap; large windows dominate. Surface the
  source count in telemetry. **Xdamage is available** (`libXdamage` present) and
  would let a backend skip grabs on unchanged windows — noted as a refinement, not
  built in v1 (v1 uses the flat `--capture-hz`).
- **Slot cap** — fixed `N=8` (1 monitor + 7 window). Exceeding warns and refuses
  rather than reallocating the Vulkan descriptor pool / pipeline.
- **Runtime texture (re)creation** happens on the render thread between frames,
  guarded by the existing upload fence — never mid-flight.
- **Both-eyes / stereo** — texture selection is per draw, orthogonal to the
  two-viewport split; window screens work in SBS unchanged.

## Testing

- **C++ pure/unit** (`stereo-math-test` / `scene`): a window screen resolves to
  `source_index≥1` with full-frame UV while monitor screens keep `source_index=0`
  + sub-rect UV; aspect derives from window geometry; slot-cap refusal;
  placeholder (`source_index<0`) selects the untextured path; spawn appends
  without disturbing existing indices or `active_screen`.
- **Component (X-display-gated, like the existing XShm tests):** `WindowBackend`
  against a throwaway mapped X window — redirect, grab a known-size pixmap, assert
  buffer dims/aspect; resize → re-name → new dims; destroy → stale/dead handling.
- **Hardware pass:** `Ctrl+Alt+W` grabs the focused window onto the active screen;
  with no selection it spawns a faced floating screen; move/scale/face it
  (selection + head-anchored features); resize the source app and confirm the
  screen follows; close it and confirm a clean placeholder revert (no crash); a
  config `window-match` screen resolves at launch.

## Future ideas (documented, not built)

- **Zero-copy capture** via dmabuf + Vulkan external-memory import (drops the CPU
  copy per window) — the real perf path once v1 proves the UX.
- **Xdamage-driven grabs** — only re-grab a window when its contents change.
- **Runtime screen removal / compaction** — actually splice a screen out of
  `scene` (needs `active_screen` re-indexing across all gesture/hotkey paths).
- **Portal window capture** for Wayland parity (needs a windowed-mode renderer,
  not `acquire_xlib_display`).
- **Gesture to grab/point at a window** instead of the hotkey.
- **Input forwarding** (interact with the floating window) — a separate, large
  project.
- **Per-window capture rate** (a slow doc window vs a live terminal).

## Files touched

- `spatial-screens/src/capture.h` — `capture_create_window(Window, int)` factory;
  (optional) `shm_image` helper decl.
- `spatial-screens/src/capture_window.cpp` (new) — `WindowBackend` (XComposite
  redirect + XShm pixmap read, own thread/connection/SHM triple buffer).
- `spatial-screens/src/capture_xshm.cpp` — (only if extracting) route through the
  shared `shm_image` helper; behavior unchanged.
- `spatial-screens/src/vk_renderer.{h,cpp}` — per-source texture/staging/dset
  array (N=8); `vkr_set_source_size` / `vkr_upload_source`; `source_index` on
  `QuadDraw`; per-draw descriptor bind.
- `spatial-screens/src/scene.{h,cpp}` — `source_index` on `ScreenInst`; window
  screens = full-frame UV; placeholder path.
- `spatial-screens/src/config.{h,cpp}` — `source` / `window_match` keys +
  startup resolution.
- `spatial-screens/src/main.cpp` — `Ctrl+Alt+W` handler; slot allocator; assign-
  to-active + append-only spawn; per-frame window-source pump + death teardown;
  telemetry source count.
- `spatial-screens/src/telemetry.*` — window-source count in the panel.
- `spatial-screens/Makefile` — link `-lXcomposite`; new `capture_window` object.
- `docs/specs/2026-07-06-floating-window-screens-design.md` — this document.
- `docs/branches/feat-floating-window-screens.md` — branch resume doc.
