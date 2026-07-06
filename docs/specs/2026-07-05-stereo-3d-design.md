# Stereo 3D design — multiple virtual screens at different depths

Branch: `feat/stereo-3d` (resume doc: `docs/branches/feat-stereo-3d.md`).
Foundation: the SBS mode-switch spike, verified glasses-on 2026-07-05
(`docs/testing/2026-07-05-sbs-3d-spike-handoff.md`).

## Goal

Move `spatial-screens` from one world-anchored quad to **multiple virtual
screens racked at different depths**, rendered in true stereo through the
Luma Ultra's side-by-side 3D mode (`0x45` = 3840×1200@90, native 1920×1200
per eye). Multi-screen depth separation is the point of stereo here: a single
flat screen gains little (6DoF motion parallax already sells its distance)
and isn't worth 120→90 Hz. Decided 2026-07-05.

Scope decisions (2026-07-06):
- Content = **multiple monitor capture**. The laptop panel (eDP-1, driven by
  the Intel/modesetting driver) is upscaled with `xrandr --scale` to a larger
  virtual framebuffer and split into **logical monitors** with
  `xrandr --setmonitor`; each virtual screen mirrors one logical monitor.
  Default: 3840×2400 split 2×2 into four 1920×1200 monitors — exactly the
  per-eye panel resolution, so content is pixel-for-pixel sharp.
- Layout = **config-defined rack** with whole-rack controls. Free per-screen
  placement (grab a screen and move it in 3D) is the target UX but explicitly
  deferred — see Roadmap.
- Architecture = **"slice one framebuffer"**: one XShm grab per capture tick
  feeds one texture; screens differ only in UV sub-rect and model matrix.
  Per-screen textures/backends (needed for window capture) are the documented
  growth path, kept open by giving screens a `{texture, uv_rect}` reference
  from day one.

## 1. Session & display lifecycle

Ownership is split by crash-survivability:

- **`run.sh` owns the desktop workspace.** Before launching the app: snapshot
  eDP-1 state, apply `xrandr --output eDP-1 --scale ...` to reach the virtual
  resolution, create logical monitors named `VS1..VSn` with
  `xrandr --setmonitor` (grid from the `workspace` key, which run.sh greps
  out of `~/.config/spatial-screens.conf`; default `2x2`). A `trap ... EXIT`
  restores the
  original scale and deletes the monitors even if the app segfaults. This
  mirrors the existing run.sh pattern (it already pins desktop scale).
- **The app owns the glasses panel mode** (it holds the SDK; one client max).

Startup order (● = change from today):

1. Parse config/state; read `stereo` (default **true**; `stereo=false` runs
   exactly today's mono path, untouched).
2. ● **SDK init moves before display acquisition** (today it is last): the
   panel must be in the 3840-wide mode *before* Vulkan enumerates modes.
3. ● `set_display_mode(0x45)`, then poll `list_outputs()` until DP-1 reports
   3840×1200 (bounded, ~3 s). **Never** poll `get_display_mode` for
   confirmation — it lags a full command cycle (spike finding #1); trust
   return codes + RandR. The RandR snapshot used by `direct_restore` is taken
   *after* the switch, so the lease restore re-enables DP-1 at its current
   (SBS) mode; the panel-mode restore is a separate, later teardown step.
4. `direct_acquire` as today, ● except the mode pick prefers an exact
   3840×1200@90 match over the largest-area heuristic.
5. Everything else unchanged (capture, gestures, telemetry, hotkeys).

Teardown (reverse, on normal exit and SIGINT/SIGTERM, latched so each step
runs once): release lease → `direct_restore` → ● `set_display_mode(0x44)` →
SDK stop/shutdown. Hard-crash recovery: ● `sbs-spike` gains a `--restore`
flag (set 0x44 and exit) — the panic tool next to the existing
`xrandr --output DP-1 --set non-desktop 0 && xrandr --output DP-1 --auto`.

Desktop reflow hazard (spike finding #3): the 0x44↔0x45 switches reflow the
desktop layout. During the session DP-1 is leased/non-desktop, so reflow only
matters in the pre-lease and post-restore windows; the app re-snapshots
outputs after each (it already does this around the lease).

### Runtime mode-change adaptation

If the panel mode changes mid-session (e.g. the glasses' hardware 2D/3D
toggle), the app adapts rather than dying:

- **Primary signal:** a real mode change invalidates the direct-mode
  swapchain (`VK_ERROR_OUT_OF_DATE_KHR` on present). On that error the app
  re-queries the display's current mode, rebuilds the swapchain, and selects
  the render path from the new extent — **3840-wide → stereo, 1920-wide →
  mono**. Same machinery both directions; the swapchain extent stays the
  single source of truth (as today).
- **Secondary signal:** a 1 Hz `get_display_mode` poll, informational only
  and debounced (it lags), used for log/telemetry — never to gate rendering.
- Whether the hardware button works at all while the output is leased is
  unknown → hardware checklist item. If it never fires, the adaptation code
  still covers "session started in the other mode".

## 2. Stereo rendering

**Eye model.** The Luma's optics are two parallel per-eye panels with ~100 %
overlap, so: per-eye camera offset **±IPD/2 along head-local x**, **parallel
symmetric frusta**. Infinity fuses at zero disparity; a 0.75 m screen gets
natural near-convergence. No toe-in (vertical parallax → eye strain), no
off-axis shift needed for this optic. A `convergence` knob is a documented
future option, default 0.

**Per-eye matrices.** Today's projection math reused with per-eye dimensions:
each eye is 1920×1200 at the same 52° diagonal FOV, so frustum `r`/`t` are
computed from 1920×1200 — not the 3840 extent. View:
`view_eye = translate(∓ipd/2, 0, 0) · view` (offset in view space, after the
head-pose inverse).

**Render loop.** The draw-list build becomes a function of the
view-projection and runs once per eye (~40 quads of CPU matrix math —
negligible). `vkr_draw` gains per-half viewport/scissor: viewport
`[0,0,1920,1200]` draws the left list, `[1920,0,1920,1200]` the right. One
render pass, one command buffer, one present — frame pacing unchanged at
90 Hz.

**Head-locked HUD** (tracking/pinch dots, hand-landmark panel): their
matrices are `proj · eye_translate` with no world transform; in stereo they
get the same ±IPD/2 x-offset so they fuse at their 0.5 m design depth instead
of ghosting.

**Shader/QuadDraw.** One addition: `vec4 uv_rect` (`u0,v0,u1,v1` sub-rect of
the shared texture); `v_uv` maps into it. Mono/single-screen uses
`(0,0,1,1)`. Push-constant block stays within 128 bytes.

**Mono path.** `stereo=false` or a 1920-wide swapchain → today's
single-viewport path. Multi-screen still works in mono (depth via motion
parallax only).

## 3. Scene, config, capture

**Scene.** `Screen = {monitor, azimuth°, elevation°, distance m, size in}`;
each screen yaws/pitches to face the rack origin. `monitor` is the logical
monitor name from `XRRGetMonitors` (`VS1..VSn` as created by run.sh). Config
keys (flat, existing parser style): `screen.N.monitor`, `screen.N.azimuth`,
`screen.N.elevation`,
`screen.N.distance`, `screen.N.size`, plus `stereo` (bool, default true),
`ipd-mm` (float, default 63.0), `workspace` (run.sh grid, default `2x2`).
With no `screen.N.*` config the rack is derived from the logical monitor
count: center 0° @ 0.75 m, sides ±35° @ 1.2 m, fourth +25° elevation @ 1.5 m
— depth variation is deliberate.

**Controls.** Recenter (Ctrl+Alt+R / fist-hold / dashboard) re-places the
whole rack from the current head yaw — today's `place_screen()` generalized.
`[`/`]` and pinch-drag scale **all** distances proportionally; `-`/`=` scale
all sizes. Persisted in the state file as two rack multipliers
(`rack-distance-scale`, `rack-size-scale`); mono keeps the existing
`distance`/`size` state keys.

**Capture.** Multi-screen forces the **xshm** backend (portal delivers one
picked stream; it remains the single-screen default). One grab of the full
scaled eDP-1 rect per tick → one texture; per-screen UV rects come from
`XRRGetMonitors` (the `--setmonitor` objects) mapped into that rect. The
cursor overlay is untouched: it composites into the one staging buffer and
appears on whichever screen's rect contains the pointer. Cost: 3840×2400 ≈
35 MB/grab; `capture-hz=120` would be ~4.2 GB/s of memcpy — multi-screen
default stays 30 Hz, real ceiling measured on hardware.

## 4. Failure handling

- `set_display_mode(0x45)` fails, or DP-1 never reports 3840-wide in ~3 s →
  restore 0x44, **fall back to mono multi-screen**, keep running. Stereo is
  never worth dying for.
- Mutter reverts `--scale`/`--setmonitor` (detected via `XRRGetMonitors`
  mismatch) → fall back to slicing the framebuffer by plain geometry;
  capture is identical, only WM tiling is lost. Warn + telemetry.
- Swapchain `OUT_OF_DATE` → the §1 adaptation path (rebuild, re-pick
  stereo/mono by extent). Repeated present failure keeps the existing
  120-frame latch → shutdown.
- Existing RSS watchdog unchanged (staging grows to ~35 MB — noise at the
  2 GB warn threshold).

## 5. Testing

- **Stereo debug without a lease:** `--window` + stereo skips the panel
  mode switch entirely and renders the SBS pair side-by-side into a desktop
  window — disparity/UV mapping eyeballable on the laptop panel. (Glasses
  must still be plugged in for the SDK/pose; only the direct-display/mode
  machinery is bypassed.)
- **Unit tests:** new `stereo-math-test` binary (pattern of
  `gesture-parse-test`): per-eye view/projection derivation, monitor-rect →
  UV mapping, `screen.N.*` config parsing, rack placement math.
- **Hardware checklist** (tracked in `docs/branches/feat-stereo-3d.md`):
  1. In-app 0x45 switch lights up stereo; eyes-on depth separation between
     screens at different distances.
  2. HUD elements fuse comfortably (no ghosting/strain).
  3. Sustained 90 Hz presentation; capture cost at 3840×2400 measured
     (find the real capture-hz ceiling).
  4. Mutter honors `--scale` + `--setmonitor` for a full session.
  5. Hardware 2D/3D button behavior under an active lease (adaptation path).
  6. All three restore paths: clean exit, SIGINT, `kill -9` +
     `sbs-spike --restore` + run.sh trap.
  7. IPD calibration: far screen (≥5 m) fuses without divergence; tune
     `ipd-mm`.
  8. Mono multi-screen fallback renders correctly when stereo is off.

## 6. Roadmap (explicitly deferred)

1. **Free per-screen placement** — the target UX: select a screen
   (gaze/point or cycle gesture), pinch-grab to move/rotate it in 3D,
   per-screen persistence. Builds on the rack scene unchanged.
2. **Window capture (XComposite)** — each screen shows one X11 window;
   grows Approach 1 into per-screen textures via the `{texture, uv_rect}`
   indirection.
3. **Convergence knob** (`convergence`, default 0) if fixed-parallel frusta
   prove uncomfortable for near work.
4. **Per-screen capture rates** (focused screen fast, peripheral slow) if
   the single-grab cost bites at high hz.
