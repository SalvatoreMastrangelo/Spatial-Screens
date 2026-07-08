# Head-Anchored Reorientation on Reposition — Design

Date: 2026-07-08
Status: approved, pre-implementation
Branch: `feat/head-anchored-reorient` (to be created)

## Supersedes

This replaces the **face-the-user orientation** half of
[`2026-07-06-vertical-placement-design.md`](2026-07-06-vertical-placement-design.md)
(its Goals bullet 1–2, Non-goals about billboarding, and Components §1–§2:
`face_user_quat` / `quat_from_basis` / apply-on-release). That document's other
half — **vertical placement** — needs no work: it already shipped as collateral
of per-screen selection (the retargeted two-hand grab writes a full unrestricted
3D `pose_pos` with a genuine head-up component, `main.cpp:859-866` /
`gesture_manip.cpp:30-35`). The `Ctrl+Alt+PageUp/Down` nudge hotkeys and the
±1.5 m elevation clamp from the old doc are **deferred** (see Non-goals).

## Preconditions

- [`2026-07-06-screen-selection-design.md`](2026-07-06-screen-selection-design.md)
  merged (it is, on `main`): **active-screen selection** and the **per-screen
  pose override** (`ScreenCfg.has_pose_override` / `pose_pos` / `pose_ori`) this
  feature writes to, plus the retargeted two-hand grab it modifies.

## Summary

When you two-hand-grab the **active** screen, weld it to your head's orientation
for the duration of the grab: as your head yaws / pitches / rolls, the screen
rotates by the identical amount. Release and it world-locks at that orientation.

The two-hand grab is already head-anchored in **position** — the anchor offset is
stored in the head frame at grab-start and rebuilt from the current head pose
each frame (`main.cpp:834-837`, `853-864`), so the screen follows your head as
you look/lean and hand motion nudges it on top. This feature is the exact
**orientation twin** of that: store the screen's orientation in the head-local
frame at grab-start, re-expand it from the current head pose each frame. During a
grab the screen is then a rigid body mounted to your head (position **and**
facing); on release it bolts to the room.

Net user experience: look where you want the screen, let go, it stays there
facing the way you were looking. A screen carried overhead is already pitched
down toward you; one carried to your left is already toed-in — as an *emergent*
result of you turning toward where you place it, not a computed look-at.

## Why not a look-at ("face_user_quat")

The superseded design computed an absolute look-at on release: build the screen's
orientation from `normalize(head_pos - screen_pos)` with a gravity-up roll
reference. That is strictly more machinery and strictly more fragile:

- It needs a roll reference and **degenerates when the screen is directly
  overhead** (look vector ∥ up vector → basis collapses), requiring an epsilon
  fallback.
- It "stares" — a screen placed low tilts up at you; the aim can feel unnatural
  at extremes.

Head-delta coupling reuses the head's own quaternion, which is always
well-behaved: **no roll lock, no basis construction, no overhead singularity, no
`atan2`.** It is also the symmetric partner of the position anchoring already in
the code, so the two read as one idea.

## Decisions (locked during brainstorming)

- **Full head-delta, roll included.** The screen copies the head's complete
  orientation delta (yaw + pitch + roll), not a yaw-only or roll-suppressed
  subset. Simplest possible: the head quaternion delta, nothing removed.
- **Continuous during the grab, not once on release.** Orientation updates every
  frame alongside position (WYSIWYG — you watch it rotate as you aim), and the
  last frame's value is what world-locks. No separate release-edge computation.

## Goals

- The active screen, while two-hand-grabbed, tracks the head's full orientation
  delta and world-locks on release.
- Zero head rotation during a grab leaves orientation exactly at the seeded pose
  (no drift).
- No new helpers, no roll/singularity handling — reuse `qmul`/`qconj` and the
  existing `world_to_rack_frame`.

## Non-goals

- **No continuous billboarding.** Outside of an active grab, screens are
  world-locked and do not re-face the user each frame.
- **No hand-driven rotation.** The grab stays translation + scale; orientation is
  head-derived only. (Manual two-hand rotation remains a future idea.)
- **No change to the other paths:** one-hand distance gesture, fist-hold
  per-screen recenter (`main.cpp:938-956`), and rack-global grab keep today's
  behavior.
- **Deferred (not in this feature):** `Ctrl+Alt+PageUp/Down` vertical-nudge
  hotkeys and the ±1.5 m elevation comfort clamp. Vertical placement works via
  the grab today; these are separate polish.

## Architecture

One localized change to the **active-screen** branch of the two-hand grab in
`main.cpp`. No new files, no helper, no sidecar/protocol change. Everything the
change needs is already computed at grab-start today and currently discarded.

```
grab-start (active screen):   persist  sq0   = screen start world orientation   (scene_screen_pose already computes it, currently unused)
                              persist  hq0   = head orientation at start        (head_rc0, already computed for position anchoring)

grab-update (each frame):     head_rc  = current head orientation
                              world_ori = (head_rc · hq0⁻¹) · sq0               // head-local orientation, re-expanded
                              world_to_rack_frame(rack, world_ori, anchor) → cfg.pose_ori, cfg.pose_pos   // both written together
                              cfg.size = spread

grab-release:                 (no new code — last frame already wrote pose_ori; has_pose_override already true → world-locked)
```

## Components

### 1. Persist two quantities at grab-start — `spatial-screens/src/main.cpp`

In the active-screen sub-case of `if (!grab.active)` (around `main.cpp:819-825`,
`834`):

- The screen's start **world orientation** `sq0`. `scene_screen_pose` already
  computes it at `main.cpp:820-821` (`sq`) but the code uses only `sp`
  (position); capture `sq` too.
- The head's start orientation `hq0`. This is exactly `head_rc0 =
  qmul(qconj(ori_offset), head_q)` already computed at `main.cpp:834` for the
  position anchor; retain it across frames.

Store both as file-scope statics alongside the existing `grab_rel0` /
`grab_scale0` (e.g. `grab_ori0`, `grab_head_q0`).

### 2. Write orientation each frame — `spatial-screens/src/main.cpp`

In the active-screen branch of the grab update (`main.cpp:843-868`), which today
writes only `pose_pos` and `size` and holds orientation fixed (the comment at
`main.cpp:850` and the single-field write at `866`):

```
Quat world_ori = qmul(qmul(head_rc, qconj(grab_head_q0)), grab_ori0);
world_to_rack_frame(rack_q, rack_p, world_ori, anchor,
                    scene[active_screen].cfg.pose_ori,
                    scene[active_screen].cfg.pose_pos);
scene[active_screen].cfg.size = gr.diag;
```

- `head_rc` is already computed at `main.cpp:853`; `anchor` is the world position
  already computed at `main.cpp:864`.
- This **replaces** the current `pose_pos`-only write and the "orientation held
  fixed" comment. `world_to_rack_frame` (the same helper the fist-hold recenter
  uses, `scene.cpp:79-86`) writes `pose_ori` and `pose_pos` together in the rack
  frame, so position behavior is unchanged and orientation is added.
- `has_pose_override` stays `true` (already set).

Associativity note: `(head_rc · hq0⁻¹) · sq0 == head_rc · (hq0⁻¹ · sq0)` — the
second grouping is literally "screen orientation in the head-local frame,
re-expanded from the current head pose," the exact twin of how `grab_rel0`
(head-local position) is handled.

## Data flow

`pose_ori` / `pose_pos` are stored **rack-relative** (per the selection design),
so recenter still moves overridden screens as a group. Orientation is evaluated
in world space each frame and converted back with `qconj(rack_q)` inside
`world_to_rack_frame`. After release the screen is a fixed object in the room —
no floatiness, no per-frame re-facing.

## Error handling / risks

- **No singularities.** Because orientation is a quaternion product of the head
  pose and constants, there is no degenerate case to guard (this is the whole
  point of choosing head-delta over look-at).
- **Roll is intentional.** Full delta includes head roll, so a head tilted during
  placement leaves the screen tilted until re-grabbed. Accepted (chosen
  explicitly); the fix is to re-grab. Note for the hardware tester so it isn't
  logged as a bug.
- **Stereo.** Orientation feeds `scene_screen_pose`, consumed identically by both
  eyes; nothing stereo-specific changes. A steeply pitched screen widens the
  in-view depth range — confirm comfort on the hardware pass.
- **"Faces me" is placement-time, world-locked.** A screen aimed from one spot
  won't perfectly face you from across the room — intended.

## Testing

- **C++ pure unit** (`stereo_math_test.cpp`, where the pose round-trip tests
  live): given a start screen world orientation `sq0`, a start head orientation
  `hq0`, and a current head orientation `hq1 = Δ · hq0`, assert the computed
  world orientation equals `Δ · sq0` (`qmul(qmul(hq1, qconj(hq0)), sq0)`), and
  that `Δ = identity` yields exactly `sq0` (no drift). Round-trip through
  `world_to_rack_frame` → `scene_screen_pose` reproduces the same world
  orientation.
- **Hardware pass:** two-hand-grab the active screen; turn/lift your head to
  place it left / right / overhead; confirm it comes with you during the grab and
  world-locks facing the way you were looking on release. Confirm the one-hand
  distance gesture, fist-hold recenter, and rack-global grab are unchanged.

## Files touched

- `spatial-screens/src/main.cpp` — persist `sq0`/`hq0` at grab-start; write
  `pose_ori` each frame in the active-screen grab branch (replacing the
  fixed-orientation `pose_pos`-only write).
- `spatial-screens/src/stereo_math_test.cpp` — head-delta orientation cases.
- `docs/specs/2026-07-08-head-anchored-reorient-design.md` — this document.
- `docs/branches/feat-head-anchored-reorient.md` — branch resume doc (at worktree
  start).
- `docs/plan/roadmap.md` — mark backlog item #1's face-the-user half as specced
  (already notes vertical placement done).
