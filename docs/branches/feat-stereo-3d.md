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
- [ ] Glasses-on verification pass
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

- [ ] 1. 0x45 lights up in-app; eyes-on depth separation between rack screens
- [ ] 2. HUD dots/landmarks fuse comfortably (no ghosting)
- [ ] 3. Sustained 90 Hz; measure real capture-hz ceiling at 3840x2400
- [ ] 4. Mutter honors --scale + --setmonitor for a full session
- [ ] 5. Hardware 2D/3D button under the lease → self-heal path works
- [ ] 6. Restore paths: clean exit, SIGINT, kill -9 + sbs-spike --restore
- [ ] 7. IPD calibration: far screen fuses without divergence; tune ipd-mm.
       If depth reads INVERTED (near screens look far), the panel routes the
       left half to the right eye — negate stereo_eye_offset's sign
- [ ] 8. stereo=false mono multi-screen still renders correctly

## How to test

`cd spatial-screens && ./run.sh` (never with viture-bridge running — single
SDK client). Spike re-run if needed: `./run-spike.sh --mode 0x45`. Safety notes
(NVIDIA idle-blank hang, panic xrandr restore, SSH recovery) in the spike
handoff doc, §"Safety first".
