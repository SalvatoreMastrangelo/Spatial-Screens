# Stereo 3D (multi-screen SBS) — hardware verification results

Session 2026-07-06, glasses-on. Branch `feat/stereo-3d`, final HEAD `7bef0ef`.
**All 8 checklist items verified. Branch is hardware-complete.**

Build: `cd spatial-screens && make`. Stop `viture-bridge` first. Launch only
via `./run.sh`. Never double-start. After any hard kill in direct mode:
`LD_LIBRARY_PATH=../sdk/lib/x86_64 ./sbs-spike --restore`, then
`xrandr --output DP-1 --set non-desktop 0 && xrandr --output DP-1 --auto`
(run.sh's trap restores the workspace on its own).

## Checklist results

- [x] 1. 0x45 lights in-app (~600–1700 ms to 3840-wide), direct mode picks
      3840x1200@90, `render: stereo (SBS), eye 1920x1200`. Eyes-on: depth
      separation between rack screens clearly visible, correct polarity
      (center-near / sides-mid / top-far).
- [x] 2. No ghosting; HUD dots fuse comfortably.
- [x] 3. Sustained 86–89 fps (soak >7 min). Went 20 → ~45 → 86–89 through two
      fixes (below). Real XShm ceiling at 3840x2400: ~30–80 grabs/s,
      load/clock dependent. Remaining gap to a locked 90 is the 36 MB staging
      memcpy on upload frames (roadmap: async upload).
- [x] 4. Mutter honors --scale + --setmonitor for a full session — but ONLY
      when applied AFTER the display lease. It reapplies stored monitor
      config on both the SBS-adopt reflow and the lease reflow, silently
      undoing anything set earlier (first run: scene fell back to 1 screen).
      run.sh now waits for the lease before building the workspace, and the
      app waits up to 10 s for the grid at scene build.
- [x] 5. Hardware 2D/3D button under the lease: on this firmware it toggles
      the OPTICS only — no DP renegotiation, no present failures, the app
      keeps rendering and both button positions look correct. The self-heal
      rebuild path is therefore NOT exercisable via the button; it remains
      as a guard for SDK-commanded / firmware-variant mode changes.
- [x] 6. Restore paths, all verified:
      - clean exit + SIGINT: 0x44 restore. Restoring AFTER lease release
        raced the DP retrain (rc -4 "USB execution error", once through all
        6 retries) — the happy path now restores 0x44 BEFORE releasing the
        lease: lands first try, rc 0.
      - kill -9 (mono and stereo): run.sh's trap cleans tiles + scale.
        Worn glasses keep the panel alive → recovery drill above suffices.
        UNWORN glasses sleep the panel, the DP link drops for real, and only
        a physical USB replug + the drill brings DP-1 back.
      - `sbs-spike --restore` recovers a stranded panel (used twice). Note:
        the spike often wedges in SDK teardown after restoring — SIGKILL it;
        the restore has already landed.
- [x] 7. Depth polarity correct (NOT inverted — no sign flip needed).
      Comfortable at default `ipd-mm = 63`; near↔far refocus fine.
- [x] 8. `./run.sh --stereo false`: mono multi-screen — 4-screen rack at
      1920x1200@120, 103–110 fps, panel never leaves 0x44.

## Bugs found on hardware, fixed in-session

| Commit | Fix |
|---|---|
| `f1df3a3` | Workspace applied post-lease (mutter reflow clobber); app waits for grid; mode-command retries |
| `2608b03` | XShm grabs + cursor composite moved to a capture thread with its own X connection (20 → 86-89 fps; the render loop's XFixesGetCursorImage round trip was queueing behind ~22 ms server-side grab blits) |
| `893dbaf` | `~Telemetry()` joins the WS thread — early-exit paths SIGABRTed (hit when the DP link was down at startup) |
| `7bef0ef` | Per-frame painter's sort (user-found: overlapping screens drew in scene order; static sort insufficient with 6DoF walk-around); 0x44 restore while leased; `direct_restore` preferred-mode fallback |

## Eyes-on feedback (Salvatore)

- Depth, fusion, comfort: all good at defaults.
- Occlusion request, implemented + re-verified live: nearest screen must win
  where screens overlap, including when viewed from the other side after
  walking around the rack (per-frame head-distance sort).

## Open items (roadmap, in `docs/branches/feat-stereo-3d.md`)

- Async texture upload (last ~10 fps to a locked 90).
- View-dependent capture (grab/upload only tiles in either eye's frustum).
- `6dof frozen` when the glasses sit on the desk (VIO has nothing to track);
  LIVE when worn — same behavior as previous sessions, not stereo-related.
- Known baseline RSS drift (+1.3 MB/min) unchanged; no new leak from stereo.
