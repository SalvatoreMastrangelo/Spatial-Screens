# spatial-screens direct mode — Vulkan direct display (design)

**Date:** 2026-07-03
**Status:** approved
**Scope:** replace the GLX windowed presentation path in `spatial-screens/` with a
Vulkan renderer that drives the glasses' display directly via a RandR lease
(`VK_EXT_acquire_xlib_display` + `VK_KHR_display`), with a windowed Vulkan
fallback.

## Why

The last four commits are a fight against compositor frame pacing on X11
multi-monitor: EWMH fullscreen for the unredirect path, `_NET_WM_BYPASS_COMPOSITOR`,
`SGI_video_sync` experiments (which cost fps and tracked the wrong CRTC). Root
cause: the compositor and X server pace presentation to one monitor's clock,
and the glasses are never guaranteed to be that monitor. Direct mode removes
the compositor and the X presentation path entirely — we lease the glasses'
connector+CRTC from X (the mechanism built for VR headsets, used by
SteamVR/Monado) and present on its real vblank.

## Environment facts (verified 2026-07-03)

- Glasses = `DP-1` on the **Intel iGPU** (`card0`, i915, X `modesetting` driver,
  Provider 0). NVIDIA dGPU is a separate provider with its own connectors —
  uninvolved. No cross-GPU buffer sharing needed.
- `DP-1` exposes exactly one mode: **1920×1200@120**. It is currently the
  *primary* X monitor; direct mode takes it away from the desktop while running.
- X.Org 21.1.4, RandR 1.6 (lease protocol present). WM is Mutter
  (`_MUTTER_PRESENTATION_OUTPUT` property present); session is X11.
- Mesa 25.1.5 ANV installed. System libXrandr is 1.5.2 (no Xlib
  `XRRCreateLease` wrapper) — irrelevant under this design because Mesa's WSI
  issues the lease itself over XCB inside `vkAcquireXlibDisplayEXT`.
- The RandR `non-desktop` output property exists on this server's outputs.

## Approach decision

- **Chosen — Vulkan direct display:** `VK_EXT_direct_mode_display` +
  `VK_EXT_acquire_xlib_display` acquire the output (Mesa performs the RandR
  lease internally); `VK_KHR_display` + `VK_KHR_swapchain` present on the CRTC.
  Full renderer rewrite from fixed-function GL to Vulkan, but the driver owns
  the lease/modeset/flip mechanics, and it aligns with the standard Linux VR
  runtime path (Monado).
- **Rejected — RandR lease + GBM/EGL + drmModePageFlip:** would have kept the
  GL renderer, but hand-rolls the lease, modeset, and flip-event loop that
  Vulkan WSI already implements.
- **Rejected — EGLStreams/EGLOutput:** NVIDIA-only, N/A on Mesa.
- **Rejected — X11 Present-extension tuning:** still compositor-entangled;
  that is the problem being escaped.

## Architecture

One Vulkan renderer, two surface backends. The GLX context, swap-interval,
and `SGI_video_sync` machinery are deleted (including the `--sgi-sync` flag).

- **Direct backend (default):** leased display → display-plane surface. No
  window, no compositor; FIFO present flips on the glasses' own vblank.
- **Window backend (fallback):** same renderer presenting into an X11 window
  via `VK_KHR_xlib_surface`, keeping today's EWMH-fullscreen +
  `_NET_WM_BYPASS_COMPOSITOR` window setup. Selected automatically when any
  direct-acquisition step fails (with the reason logged), or forced with the
  new `--window` flag.

Untouched: SDK glue, pose polling, One-Euro filtering, placement math, 6DoF
liveness heuristic, XShm capture, global Ctrl+Alt hotkey grabs (root-window
grabs on the X connection, which stays open in both backends).

### File layout (`spatial-screens/src/`)

- `main.cpp` — app logic: args, X/output selection, SDK, pose filter, capture,
  hotkeys, and the app loop (input → pose → capture → draw calls). Sheds all
  GL/GLX code.
- `vk_renderer.{h,cpp}` — instance/device/swapchain/pipeline and the per-frame
  acquire/record/submit/present mechanics, exposed as begin/draw/end calls.
  Consumes a `VkSurfaceKHR` + extent; agnostic to backend. For the window
  backend, device selection prefers the integrated Mesa device that supports
  presenting to the surface (avoids a PRIME copy from the dGPU).
- `vk_surface.{h,cpp}` — the two surface backends: RandR non-desktop dance +
  display acquisition + restore guard; X11 window creation + xlib surface.
- `shaders/quad.vert`, `shaders/quad.frag` — GLSL; generated SPIR-V is checked
  in so builds work without a shader compiler (Makefile regenerates via
  `glslangValidator` when available).

Raw Vulkan C API (no vulkan.hpp), matching the project's raw-Xlib style.
C++: snake_case functions, `g_` prefix for atomic globals (existing convention).

## Direct display acquisition (in order, on the existing X connection)

1. Find the glasses output via RandR exactly as today: 1920×1200-mode
   heuristic, never eDP/LVDS, `--monitor NAME` override.
2. Save the output's current state (mode, position, primary flag). Set the
   RandR `non-desktop=1` property. Mutter releases the output and disables its
   CRTC — required, because X will not lease a CRTC the desktop is actively
   scanning out. If the CRTC is still active after a short wait, explicitly
   disable it via `XRRSetCrtcConfig` (belt and suspenders).
3. Enumerate Vulkan physical devices. For each, call
   `vkGetRandROutputDisplayEXT(dpy, output_xid)`; the device returning a valid
   `VkDisplayKHR` owns the connector (Intel succeeds, NVIDIA fails — natural
   device selection). Then `vkAcquireXlibDisplayEXT`.
4. Enumerate display modes; pick native resolution at the highest refresh
   (1920×1200@120 here). Pick a supported display plane. Create the surface
   with `vkCreateDisplayPlaneSurfaceKHR`. From here it is a normal swapchain.
5. **Restore on every exit path** — normal exit, signals, or an init failure
   after step 2: release the display, set `non-desktop=0`, re-enable the CRTC
   if Mutter does not re-adopt it, restore the primary flag. A RAII guard owns
   restoration so a mid-init error cannot strand the output. Signal handlers
   set the existing `g_running` flag; restoration runs in normal teardown.

**Capture-geometry knock-on:** removing DP-1 makes Mutter reflow the remaining
monitors, so the capture source's root coordinates go stale. After acquisition
(and on `RRScreenChangeNotify`), re-query output geometry; if the source moved,
update the grab origin; if it resized, rebuild the XShm segment and texture.

## Vulkan renderer

The scene is one textured quad plus a colored border — the renderer stays
deliberately minimal:

- **One graphics pipeline, zero vertex buffers.** Vertices derived from
  `gl_VertexIndex` in the vertex shader. Push constants (≤128 B guaranteed):
  MVP (64 B), RGBA color (16 B), textured/solid flag + quad half-extents
  (16 B). Quad = one draw; border = four thin solid quads drawn after it.
  No depth attachment (painter's order) — also sidesteps any `wideLines`
  feature dependency.
- **Projection:** same 52° diagonal FOV frustum math, adjusted for Vulkan clip
  space (Y flip, Z ∈ [0,1]). Existing `Quat`/`mat_from_pose` code reused; MVP
  composed on CPU.
- **Capture texture:** persistent host-visible staging buffer; XShm grab
  (BGRX) → `vkCmdCopyBufferToImage` into a `B8G8R8A8_UNORM` sampled image at
  the 30 Hz capture tick, standard layout transitions recorded in that frame's
  command buffer. Sampler: linear, clamp-to-edge, no mips (matches GL build).
- **Frame loop / pacing:** FIFO present mode (mandatory-supported, tear-free),
  `minImageCount` 2 for lowest queue depth, two frames in flight with
  per-frame fences, command buffer re-recorded each frame. Pose is sampled
  immediately before recording, so blocking in acquire/present late-latches
  the pose to the glasses' 120 Hz vblank. Handle
  `VK_ERROR_OUT_OF_DATE_KHR`/`SUBOPTIMAL` by recreating the swapchain (matters
  for the window backend on resize; harmless for direct).
- **Clear color:** true black (OLED pixels off = transparent in the glasses).

## Error handling summary

- Any step-1–4 failure → log reason, fall back to window backend. If the
  failure happens after step 2, the restore guard runs *first* (non-desktop
  cleared, CRTC re-enabled) — the window backend needs DP-1 back on the
  desktop before it can fullscreen a window there.
- Post-acquisition failure → restore guard runs, then exit non-zero.
- Swapchain out-of-date → recreate.
- SDK init failure → unchanged behavior (exit), restore guard still runs.

## Dependencies

New build deps: `libvulkan-dev`, `glslang-tools` (build-time shader compile;
SPIR-V also checked in), `vulkan-tools` (verification only). Runtime needs
Mesa ANV ≥ 22 (25.1.5 installed). `libdrm`/`gbm`/XCB code: none — Mesa WSI
handles the lease and flips.

## Verification (manual, on hardware — project has no C++ test rig)

0. **Step zero, before any code:** `vulkaninfo` on the Intel device must list
   `VK_EXT_acquire_xlib_display`, `VK_EXT_direct_mode_display`,
   `VK_KHR_display`. This is the go/no-go gate for the whole design.
1. Window backend renders the same scene as today's GLX build (parity check).
2. Direct backend lights the glasses with DP-1 gone from the desktop
   (`xrandr` shows it released; Mutter reflows).
3. Exit paths: normal quit, SIGINT/SIGTERM, and a forced post-acquisition
   failure all leave DP-1 restored as a desktop output.
4. fps counter reads ~120; head-turn latency subjectively ≤ current build.
5. Capture still tracks the source monitor after the desktop reflow, including
   after a monitor-layout change while running.

## Risks

- **Mutter must release the CRTC on `non-desktop=1`.** Mitigation is built in
  (explicit `XRRSetCrtcConfig` off). Worst case the lease fails and the window
  backend takes over — never a hard failure.
- **ANV display-surface path is less traveled than window swapchains.**
  Verification step 2 is scheduled as early as possible in implementation.
- **Glasses unplug while leased** → display surface dies; treat as fatal,
  restore guard runs on teardown.

## Future (out of scope)

- SBS 3D (3840×1200 half-width per eye) — direct mode makes the mode-switch
  trivial later; nothing in this design assumes 2D-only beyond the quad scene.
- `VK_GOOGLE_display_timing` for measured motion-to-photon.
- PipeWire capture (Wayland-proof) — orthogonal to presentation.
