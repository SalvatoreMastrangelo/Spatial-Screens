# feat/stereo-3d — multi-screen stereo rendering (SBS 3D)

Branch resume doc. Update this as the branch evolves; it must always be enough
to pick the work back up cold.

## Goal

Move `spatial-screens` from one world-anchored quad to **multiple virtual
screens racked at different depths**, rendered in true stereo via the glasses'
side-by-side 3D mode. Decided 2026-07-05: multi-screen depth separation is the
point — a single flat screen gains little from stereo (6DoF parallax already
sells its distance) and isn't worth the 120→90 Hz cost.

## Verified foundation (done, on this branch)

Glasses-on spike 2026-07-05 (`docs/testing/2026-07-05-sbs-3d-spike-handoff.md`):
- `set_display_mode(0x45)` → **3840×1200@90 SBS**, rc 0, X auto-adopts the
  3840-wide DP-1 mode, each eye gets one half. **This is the mode to use.**
- Restore with `set_display_mode(0x44)` (native 2D, 1920×1200@120) — clean.
- `get_display_mode` lags a full command cycle — never poll it for
  confirmation; trust return codes + xrandr.
- The mode switch reflows the desktop layout (DP-1 jumps position, GNOME
  re-stacks outputs) — integration must expect/pin layout around the switch.
- Spike code: `spatial-screens/src/sbs_spike.cpp`, `run-spike.sh`,
  `make sbs-spike` (throwaway, SDK-only, always restores).

## Plan of record

1. **Design doc** — `docs/specs/2026-07-05-stereo-3d-design.md` (written
   2026-07-06, spec of record): "slice one framebuffer" — eDP-1
   upscaled to 3840×2400 (`xrandr --scale`) and split into VS1..VS4 logical
   monitors (`--setmonitor`; run.sh owns setup + trap-EXIT restore); one XShm
   grab → one texture, screens = UV sub-rects; SBS 0x45 on startup / 0x44
   restore (app owns; `sbs-spike --restore` = panic tool); two-viewport
   stereo, parallel frusta, ±IPD/2 (`ipd-mm`, default 63); runtime
   mono↔stereo adaptation off swapchain OUT_OF_DATE; mono fallback on any
   stereo failure. Free per-screen placement + window capture = roadmap.
   NOTE: work continues in worktree `.claude/worktrees/stereo-3d` — the main
   checkout hosts a parallel session (feat/two-hand-gestures).
2. **Implementation plan** — `docs/specs/2026-07-06-stereo-3d-plan.md`
   (written 2026-07-06): 12 bite-sized TDD tasks — pose-math extraction,
   config keys, scene module, stereo helpers, uv/two-viewport renderer,
   sbs_mode module + spike --restore, run.sh workspace, then 4 staged
   main.cpp integration tasks (SDK reorder → multi-screen mono → stereo →
   runtime adaptation), docs last.
3. Implementation per the plan (est. ~2–4 days).
4. Hardware verification pass, then merge to master.

## Current state / next step

- [x] Spike verified + committed (`e8f6d85`)
- [x] Write the stereo design doc (spec of record: `docs/specs/2026-07-05-stereo-3d-design.md`)
- [x] Implement multi-screen scene
- [x] Implement stereo rendering + mode switch lifecycle
- [x] Glasses-on verification pass (2026-07-06, 8/8 — results in
      `docs/testing/2026-07-06-stereo-3d-test-handoff.md`)
- [ ] Merge to master

## Hardware verification session (next)

Preflight: `pkill viture-bridge; pgrep -af spatial-screens` (nothing may hold
the SDK), glasses plugged + awake, `gnome-session-inhibit --inhibit idle sleep 3600 &`
(NVIDIA idle-blank hang), recovery terminal on the laptop panel or
`ssh sonmorri`.

Run: `cd spatial-screens && make && ./run.sh`
Panic if killed hard: `LD_LIBRARY_PATH=../sdk/lib/x86_64 ./sbs-spike --restore`
and `xrandr --output DP-1 --set non-desktop 0 && xrandr --output DP-1 --auto`;
run.sh's trap restores the workspace on its own.

- [x] 1. 0x45 lights in-app (~700 ms, direct 3840x1200@90); eyes-on depth separation
       between rack screens confirmed (center-near / sides / top-far read correctly)
- [x] 2. No ghosting, comfortable fusion (eyes-on 2026-07-06)
- [x] 3. Sustained 86-89 fps after moving XShm grabs + cursor composite onto a capture
       thread (was 20); real capture ceiling at 3840x2400 ≈ 30-80/s (load-dependent)
- [x] 4. Mutter honors --scale + --setmonitor for a full session — but ONLY when applied
       after the lease (mutter reverts anything set before the SBS-adopt/lease reflows);
       run.sh now sequences this. VS1-4 held a 7-min session
       — verify VS1..VSn actually overlay the capture panel after the 0x45
       reflow (`xrandr --listmonitors`); if the panel moved off the framebuffer
       origin, tiles must be offset by its +X+Y or the scene drops to single-screen
- [x] 5. Hardware 2D/3D button: on this firmware it toggles the OPTICS only — no DP
       renegotiation, no present failures, app unaffected (worked in both positions).
       The self-heal rebuild path therefore stays unexercised by the button; it still
       guards SDK-commanded/firmware-variant mode changes
- [x] 6. Restore paths: clean exit ✓, SIGINT ✓ (0x44 restore needs 1-3 retries — USB
       retrain race, now retried 6x500ms), kill -9: run.sh trap cleans workspace ✓,
       sbs-spike --restore recovers a stranded panel ✓; stereo kill -9 variant ✓ (glasses
       worn: wear sensor keeps the panel alive; recovery drill = sbs-spike --restore,
       then xrandr --output DP-1 --set non-desktop 0 && xrandr --output DP-1 --auto.
       Glasses NOT worn: the panel sleeps, the DP link drops, and only a physical
       USB replug + the same drill brings it back)
- [x] 7. Depth polarity correct (not inverted); comfortable at default ipd-mm 63,
       near-to-far refocus fine — no tuning needed
- [x] 8. stereo=false mono multi-screen renders: 4-screen rack, mono, 1920x1200@120,
       103-110 fps, panel never leaves 0x44 (eyes-on look still worthwhile)

## How to test

`cd spatial-screens && ./run.sh` (never with viture-bridge running — single
SDK client). Spike re-run if needed: `./run-spike.sh --mode 0x45`. Safety notes
(NVIDIA idle-blank hang, panic xrandr restore, SSH recovery) in the spike
handoff doc, §"Safety first".

## Deferred cleanups (roadmap)

- async texture upload: the 36 MB staging memcpy runs on the render thread
  and costs ~4-5 ms per consumed frame — the last barrier between ~80 and a
  locked 90 Hz (move the memcpy to the capture thread, double-stage)
- view-dependent capture: grab/upload only the workspace tiles whose screens
  intersect either eye frustum (with margin + hysteresis against edge pop-in).
  Draw-side FOV culling is free already (GPU clips off-view quads) — the
  win is skipping their share of the grab blit and upload

- sbs_mode: clamp a poisoned orig mode — if the startup read returns 0x45, the restore target should be 0x44 (native), not 0x45.
- vk_surface: gate the 3840×1200 direct-mode preference on stereo intent (don't prefer the SBS mode when stereo is off).
- Makefile: the `main.o` dependency line omits `scene.h`/`stereo.h`/`sbs_mode.h` — edits to those don't trigger a rebuild.
- run.sh detects the glasses as literal `DP-1` while the app detects by mode — unify the identity contract.
- main.cpp: the capture-source predicate is duplicated 3×; dead `old_distance`/`(void)` in the single-mode pinch branch.
- config.cpp: redundant `dot2 == 7` guard; stereo_math_test: document the 1.1x threshold derivation.
- dashboard: label the rack multipliers as multipliers (panels.js shows "m"/inches in multi mode).
