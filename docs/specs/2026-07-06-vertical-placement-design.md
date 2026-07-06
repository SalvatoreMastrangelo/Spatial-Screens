# Vertical Placement & Face-the-User Orientation — Design

Date: 2026-07-06
Status: approved, pre-implementation
Branch: `feat/vertical-placement` (to be created)

## Preconditions

- [`2026-07-06-screen-selection-design.md`](2026-07-06-screen-selection-design.md)
  merged: **active-screen selection** and the **per-screen pose override**
  (`ScreenCfg.has_pose_override` / `pose_pos` / `pose_ori`) that this feature
  writes to.
- `feat/two-hand-gestures` merged: the two-hand grab (`gesture_manip.{h,cpp}`)
  whose midpoint reposition already produces vertical motion.

## Summary

Let screens be placed **higher or lower** in space (above / below eye level, not
just around a horizontal ring), and have a repositioned screen **re-orient to
face the user** — so a screen parked above you tilts down toward your head
("screens above me, facing me").

Two observations shrink this feature to its real core:

1. **Vertical motion already exists.** The two-hand grab repositions in the
   head's **right/up plane** (`up0` basis in `grab_update`), so moving your
   hands up/down already lifts/lowers the target. Once the selection feature
   retargets the grab to the active screen, per-screen vertical placement falls
   out of it for free.
2. **The missing piece is orientation.** The grab deliberately **holds
   orientation fixed** (design of record for two-hand gestures). A screen shoved
   upward therefore keeps facing horizontally and becomes unreadable. This
   feature adds a **face-the-user** orientation, applied once when a reposition
   ends, then world-locked.

So the deliverable is: (a) confirm/round out the vertical range through the
grab, (b) a pure "face user" orientation helper, and (c) apply it on
reposition-release to the active screen's pose override.

## Goals

- A repositioned screen orients to **look at the user's head position at the
  moment the reposition ends**, then stays world-locked (the "face-me at
  placement" model, chosen 2026-07-06).
- Correct facing for screens placed **above, below, or to the side** — the
  screen's normal points at the head; its roll stays gravity-up (no tilt-roll).
- A pure, unit-tested `face_user_quat(screen_pos, head_pos, world_up)` helper.
- A non-gesture affordance to nudge the active screen up/down (hotkeys), so the
  feature is usable without the two-hand grab and testable deterministically.

## Non-goals (v1)

- **No continuous billboarding.** Screens do not re-face the user every frame as
  they walk around — that fights world-anchoring (decided 2026-07-06). Facing is
  recomputed only on reposition (and, optionally, on recenter).
- **No new capture / source behavior** — orientation and elevation only.
- **No pitch/roll from the hands** — the grab stays translation+scale; facing is
  derived, not hand-driven.
- **No gravity re-derivation** — reuse the existing gravity-up basis already used
  to place the rack vertical.

## Architecture

A pure orientation helper, one call site at reposition-release, and two hotkeys.
No sidecar/protocol changes.

```
two-hand grab release  ──┐
Ctrl+Alt+PageUp/PageDn ──┼─► set active screen's pose_pos (elevation)
                          │   then ori = face_user_quat(pose_pos_world, head_p, up)
                          ▼
                 ScreenCfg.pose_pos / pose_ori (rack-relative)
                          │
                          ▼
                 scene_screen_pose() → renders facing the user
```

## Components

### 1. `face_user_quat` — `spatial-screens/src/pose_math.h`

Pure function; no SDK/X11. Given a screen world position, the user's head world
position, and world-up:

```
fwd  = normalize(head_pos - screen_pos)   // screen looks toward the head
right = normalize(cross(world_up, -fwd))  // gravity-up roll reference
up    = cross(-fwd, right)
quat  = quat_from_basis(right, up, -fwd)  // screen's -Z faces the head (EUS)
```

- Screen convention matches `scene_screen_pose`: the quad faces along its local
  **−Z**, so build the basis so that **−Z points at the head**.
- **Roll lock:** derive `right` from `world_up` so the screen never rolls; if
  the screen is directly above/below the head (`fwd ∥ world_up`, degenerate
  cross), fall back to the head's forward axis for the roll reference. Guard the
  near-parallel case with an epsilon.
- Add a small `quat_from_basis(right, up, fwd)` helper if `pose_math.h` doesn't
  already have one (it has `quat_axis_angle`, `qmul`, `qrot`, `qconj`,
  `yaw_twist`). Keep it beside those.

### 2. Apply on reposition-release — `spatial-screens/src/main.cpp`

- The selection feature already writes the active screen's `pose_pos` during a
  two-hand grab (with `has_pose_override = true`, orientation fixed). This
  feature adds, at the **grab-release edge** for the active screen:
  1. Compute the screen's world position from its (just-updated) `pose_pos`.
  2. `pose_ori_world = face_user_quat(pos_world, head_p, {0,1,0})`.
  3. Store it back rack-relative: `cfg.pose_ori = qmul(qconj(rack_q), pose_ori_world)`.
- Only the **active** screen is re-faced (rack-global grab keeps today's fixed
  orientation — a whole-rack move shouldn't spin every screen).
- Optional: on **recenter**, re-face all overridden screens to the new head
  position (cheap; makes "recenter = tidy the layout to face me" feel right).
  Gate behind a constant so it can be toggled on hardware.

### 3. Vertical nudge hotkeys — `spatial-screens/src/main.cpp`

- `Ctrl+Alt+PageUp` / `Ctrl+Alt+PageDown`: raise/lower the active screen by a
  fixed step (e.g. 0.1 m) along world-up, writing `pose_pos` (seeding the
  override from the formula pose on first use, exactly like the grab does), then
  applying `face_user_quat`.
- No-op with a log line when no screen is active (elevation is a per-screen act).
- These give a deterministic path for the unit/hardware tests and a fallback for
  users who find the two-hand grab's vertical axis fiddly (noted as a learning
  curve on the two-hand hardware pass).

### 4. Elevation range

- The grab and hotkeys move in metric world space, so vertical range is bounded
  only by comfort. Clamp world-up offset to a sane band (e.g. ±1.5 m from the
  rack origin) to prevent a screen vanishing overhead. Constant, tunable.

## Data flow

`pose_pos` is stored **rack-relative** (per the selection design), so recenter
moves faced screens as a group and `face_user_quat` is always evaluated in world
space then converted back with `qconj(rack_q)`. The screen therefore faces the
head at placement time and then behaves as a fixed object in the room as the
user moves — no floatiness.

## Error handling / risks

- **Degenerate look-at (screen directly overhead).** `cross(up, fwd)` collapses;
  guarded by the epsilon fallback to a forward-based roll reference. Unit-tested
  explicitly (screen at `head + (0, +1.5, 0)`).
- **Roll drift.** Building `right` from `world_up` guarantees zero roll; verified
  by asserting the resulting up vector has no roll component in the test.
- **Interaction with stereo.** Orientation feeds `scene_screen_pose`, which both
  eyes already consume; nothing stereo-specific changes. A steeply pitched
  screen widens the depth range in view — confirm comfort on the hardware pass
  (the stereo design's IPD/parallax path is unchanged).
- **"Faces me" vs world-lock expectation.** Because facing is computed once, a
  screen faced from one spot won't perfectly face you from across the room — this
  is intended (world-locked). Documented for the hardware tester so it isn't
  logged as a bug.

## Testing

- **C++ pure units** (`pose_math` test): `face_user_quat` — screen in front,
  above, below, left, and directly overhead (degenerate); assert the screen −Z
  points at the head (`dot(qrot(q,{0,0,-1}), normalize(head-screen)) ≈ 1`) and
  roll is zero (up stays in the gravity plane).
- **C++ unit:** the reposition-release path writes a rack-relative `pose_ori`
  that, when re-expanded by `scene_screen_pose`, faces the head.
- **Hardware pass:** two-hand-grab a screen up high and confirm it tilts down to
  face you and is readable; same below eye level; PageUp/PageDown nudge works on
  the active screen only; optional recenter-refaces-all behaves.

## Future ideas (documented, not built)

- **Continuous billboard mode** as an opt-in per screen (a "follow" flag) for
  HUD-style panels that should always face you — distinct from the default
  world-locked screens.
- **Curved vertical rack presets** (a stacked column of screens above/below),
  once free placement + facing exist — folds into M4 presets.
- **Two-hand rotation** (already noted in the two-hand design) would let the user
  override the auto-facing manually.

## Files touched

- `spatial-screens/src/pose_math.h` — `face_user_quat` (+ `quat_from_basis` if
  needed).
- `spatial-screens/src/main.cpp` — reposition-release re-face, vertical nudge
  hotkeys, optional recenter-refaces-all, elevation clamp.
- `spatial-screens/src/stereo_math_test.cpp` (or a new `pose_math_test`) —
  `face_user_quat` cases.
- `docs/specs/2026-07-06-vertical-placement-design.md` — this document.
- `docs/branches/feat-vertical-placement.md` — branch resume doc (at worktree
  start).
