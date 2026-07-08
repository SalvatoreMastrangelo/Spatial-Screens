# feat/head-anchored-reorient — branch record

**Status: COMPLETE — hardware pass PASSED 2026-07-08. Ready to merge to `main`.**

## What it does

While the **active** screen is two-hand-grabbed, its orientation welds to the
head's **full rotation delta** (yaw + pitch + roll) and world-locks on release —
the orientation twin of the grab's pre-existing head-anchored *position*. Result:
look where you want a screen, let go, and it stays there facing the way you were
looking (a screen carried overhead ends up tilted down toward you). Facing comes
out as an emergent result of turning your head toward where you place things — no
look-at, no roll lock, no overhead singularity.

## Design decisions (brainstormed 2026-07-08)

- Chose **full head-delta including roll** over a `face_user_quat` look-at. The
  look-at needed a gravity-up roll reference and a degenerate-overhead guard;
  head-delta reuses the head's own quaternion, which is always well-behaved.
- **Continuous during the grab** (written every frame), world-locked on release
  (no separate release-edge code — the last frame's write persists because
  `has_pose_override` stays true).
- Supersedes the face-the-user half of
  `docs/specs/2026-07-06-vertical-placement-design.md`. Vertical *placement* was
  already collateral of per-screen selection (grab writes free 3D `pose_pos`).
- Spec: `docs/specs/2026-07-08-head-anchored-reorient-design.md`.
- Plan: `docs/superpowers/plans/2026-07-08-head-anchored-reorient.md`.

## Implementation

- `spatial-screens/src/pose_math.h` — pure helper
  `head_delta_orient(start_ori, head_start, head_now)` = `(head_now · head_start⁻¹) · start_ori`.
- `spatial-screens/src/main.cpp` — capture `grab_ori0` (screen world ori) and
  `grab_head_q0` (head ori) at grab-start; each frame in the active-screen grab
  branch write `pose_ori`/`pose_pos` via `world_to_rack_frame` (position math
  unchanged, orientation added). One-hand distance, fist-hold recenter, and the
  rack-global grab branch are untouched.
- `spatial-screens/src/stereo_math_test.cpp` — `test_head_delta_orient` (pins
  multiply order, roll carry-through, no-drift, and a `world_to_rack_frame →
  scene_screen_pose` round-trip).

Commits: `4527805` (helper + tests), `532980f` (wiring), `97fa4a0` (comment fix).

## Verification

- Unit: `make stereo-math-test && ./stereo-math-test` → all checks passed.
- Build: `make` → clean.
- Reviews: per-task + whole-branch (opus) — 0 Critical / 0 Important; math,
  multiply order, position-equivalence, capture/read split, and test coverage
  all verified correct.
- **Hardware pass 2026-07-08: PASSED** ("all ok"). Glasses at 1920×1200@120,
  stereo 2×2 workspace. Select (`two_up`) → two-hand grab → head-move to place →
  release-and-face confirmed; one-hand distance, fist-hold recenter, and
  rack-global grab regressions all clean.

## Notes for future

- Full delta includes head **roll**: a head tilt during placement leaves the
  screen rolled until re-grabbed (chosen behavior). Re-grab to fix.
- Don't recenter *during* an active grab (pre-existing frame-jump, now symmetric
  across position + orientation).
- Deferred polish (backlog, low priority): `Ctrl+Alt+PageUp/Down` vertical-nudge
  hotkeys + ±1.5 m elevation comfort clamp.
- Incident during build: a low-tier implementer subagent left a stray commit on
  `main` (cleaned up); lesson saved to auto-memory `subagent-worktree-commit-leak`.
