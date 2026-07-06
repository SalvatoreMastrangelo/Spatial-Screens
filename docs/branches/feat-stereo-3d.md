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

1. **Design doc** — `docs/specs/2026-07-05-stereo-3d-design.md` (in progress):
   multi-screen scene graph with per-screen distance/pose; switch to 0x45 on
   startup + restore 0x44 on exit (incl. crash paths); two eye views into the
   3840-wide framebuffer (viewports `[0..1920]` / `[1920..3840]`, camera
   ±IPD/2, off-axis frustum); IPD as a config value (no SDK API for it);
   handle desktop-layout reflow around the switch.
2. Implementation per the design doc (est. ~2–4 days).
3. Hardware verification pass, then merge to master.

## Current state / next step

- [x] Spike verified + committed (`e8f6d85`)
- [ ] **NEXT: write the stereo design doc** (brainstorm → spec)
- [ ] Implement multi-screen scene
- [ ] Implement stereo rendering + mode switch lifecycle
- [ ] Glasses-on verification pass
- [ ] Merge to master

## How to test

`cd spatial-screens && ./run.sh` (never with viture-bridge running — single
SDK client). Spike re-run if needed: `./run-spike.sh --mode 0x45`. Safety notes
(NVIDIA idle-blank hang, panic xrandr restore, SSH recovery) in the spike
handoff doc, §"Safety first".
