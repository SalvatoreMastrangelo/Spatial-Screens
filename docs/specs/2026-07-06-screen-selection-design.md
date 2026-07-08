# Per-Screen Selection & Independent Manipulation — Design

Date: 2026-07-06
Status: approved, pre-implementation
Branch: `feat/screen-selection` (to be created)

> **Foundation feature.** This is the enabler for two roadmap items that both
> need "act on ONE screen, not the whole rack":
> [`2026-07-06-vertical-placement-design.md`](2026-07-06-vertical-placement-design.md)
> and
> [`2026-07-06-floating-window-screens-design.md`](2026-07-06-floating-window-screens-design.md).
> Build this one first.

## Preconditions

- `feat/two-hand-gestures` merged to `main` (per-hand `HandState`, per-hand
  arming, one-hand pinch-drag = distance, two-hand grab in
  `gesture_manip.{h,cpp}`). This design extends that gesture layer.
- Multi-screen stereo rack in place (`scene.{h,cpp}`, `ScreenInst`,
  `scene_screen_pose`, `rack_q`/`rack_p` in `main.cpp`) — already on `main` via
  `feat/stereo-3d`.

## Summary

Today every gesture and hotkey acts on the **whole rack**: distance changes
`rack_dist_scale`, size changes `rack_size_scale`, the two-hand grab moves the
single rack anchor. There is no way to move one screen independently.

This feature adds the concept of an **active screen**. A new
index-plus-middle-finger pose selects the screen nearest your gaze; the active
screen is highlighted; and the existing manipulation gestures (one-hand
distance, two-hand grab) retarget to the active screen. With no screen
selected, everything behaves exactly as today (rack-global) — fully
backward-compatible.

The mechanism that makes independent motion possible is a **per-screen world
pose override**: a selected screen stops being derived from the rack
azimuth/elevation/distance formula and instead carries its own world position +
orientation that gestures write to. That override field is the substrate the
vertical-placement and floating-window features build on.

## Goals

- A discrete **select** gesture (index + middle extended, side by side, ring +
  pinky curled) that sets the active screen.
- **Gaze-center pick**: the active screen is the one whose world direction is
  closest to the head-forward axis at the moment of the gesture.
- **Highlight** the active screen so the selection is unambiguous on-glasses.
- **Retarget** one-hand distance and the two-hand grab (reposition + resize) to
  the active screen; fall back to rack-global when nothing is selected.
- A per-screen world **pose override** in `ScreenCfg`, consumed by
  `scene_screen_pose`.
- Deselect path (return to rack-global control).

## Non-goals (v1)

- **No new motion axes** — reposition/resize still come from the existing
  gestures; this only changes *what they target*. (Vertical range + face-the-user
  orientation are the next feature.)
- **No layout persistence across runs** beyond what the state file already does
  — full per-screen layout save/restore is M4 roadmap. A minimal serialization
  hook is left in place but writing every screen's override is optional in v1.
- **No pointer/ray selection** — gaze-center only (decided 2026-07-06).

## Architecture

One new pose out of the sidecar, one new piece of render-loop state
(`active_screen`), one new field on `ScreenCfg`, and a retarget branch in the
gesture block. No socket/protocol changes — the pose rides the existing
`HandState.pose` string.

```
classify.py (new "two_up" pose)
        │  (existing event schema, pose string)
        ▼
main.cpp render loop
  ├─ active_screen : int   (-1 = none / rack-global)
  ├─ on two_up rising edge → gaze-center pick → set/clear active_screen
  ├─ frame the active ScreenInst with a green outside border (4 edge bars)
  └─ distance / grab gestures: if active_screen>=0 write that screen's
     ScreenCfg (incl. pose override); else keep today's rack-global path
        │
        ▼
scene_screen_pose(): if cfg.has_pose_override → use cfg.pose_pos/pose_ori
                     else → existing az/el/dist formula
```

## Components

### 1. New pose `two_up` — `spatial-screens/gestures/classify.py`

Extend `classify_pose`. `curled` is already computed as
`[index, middle, ring, pinky]`. Add, after the `point` case and before the
final `return "none"`:

```python
if not curled[0] and not curled[1] and curled[2] and curled[3]:
    return "two_up"
```

- `two_up` = index + middle extended, ring + pinky curled. Mutually exclusive
  with `point` (index only) and `open_palm` / `fist`.
- **Optional adjacency refinement** (tunable on hardware): to distinguish a
  deliberate "two fingers together" from a spread V/peace, require the index and
  middle *tips* to be close relative to palm size
  (`_dist(index_tip, middle_tip) / palm < TWO_UP_ADJACENCY`). Start without it;
  add only if a spread hand false-triggers. Keep the threshold a module-level
  constant.
- Unit tests in `tests/test_classify.py`: synthetic landmarks for two_up (both
  orientations), plus negative cases against `point`, `open_palm`, and a spread
  V (if adjacency is enabled).

### 2. Active-screen state + gaze-center pick — `spatial-screens/src/main.cpp`

- New render-loop variable `int active_screen = -1;` (-1 = none → rack-global).
- **Selection is a discrete, armed action** reusing the existing per-hand arm
  gate (open palm arms; completing the action disarms), so a stray hand can't
  reselect. Fire on the **rising edge** of `pose == "two_up"` on an armed hand.
- **Gaze-center pick** (pure helper, unit-tested):
  - For each `ScreenInst`, get its world position via `scene_screen_pose(...)`.
  - Direction from head: `dir = normalize(screen_pos - head_p)`.
  - Head-forward: `fwd = qrot(head_q, {0,0,-1})`.
  - Score = `dot(dir, fwd)`; pick the max. Require it inside a cone
    (`acos(score) < SELECT_CONE_DEG`, default ~40°). If nothing qualifies, set
    `active_screen = -1` (this doubles as **deselect**: make the gesture while
    looking away from all screens).
  - Re-picking the currently-active screen is a no-op (stays selected).
- Extract the scoring into a pure function
  `int pick_gaze_screen(const std::vector<Vec3>& screen_pos, Vec3 head_p, Quat head_q, float cone_deg)`
  so it unit-tests without X11/SDK.

### 3. Per-screen pose override — `spatial-screens/src/config.h` + `scene.cpp`

Add to `ScreenCfg`:

```cpp
bool has_pose_override = false;   // true once a gesture has freely placed it
Vec3 pose_pos;                    // world position, in the rack frame
Quat pose_ori;                    // world orientation, in the rack frame
```

(Include `pose_math.h` in `config.h`, or forward via a small POD if the include
order is a problem — `ScreenCfg` already lives beside the math types at use
sites.)

`scene_screen_pose` gains a short-circuit at the top (note: `dist_scale` is
**ignored** for an overridden screen — its position is fully specified by
`pose_pos`):

```cpp
if (s.cfg.has_pose_override) {
    out_q = qmul(rack_q, s.cfg.pose_ori);
    out_p = { rack_p.x + qrot(rack_q, s.cfg.pose_pos).x, ... };
    return;
}
// else: existing yaw/pitch az-el-dist formula
```

Its inverse — used to seed the override from a formula pose and to store a
grabbed world pose — is a pure helper in `scene.{h,cpp}`:

```cpp
// out_pos = qrot(qconj(rack_q), world_p - rack_p);  out_ori = qmul(qconj(rack_q), world_q)
void world_to_rack_frame(const Quat& rack_q, const Vec3& rack_p,
                         const Quat& world_q, const Vec3& world_p,
                         Quat& out_ori, Vec3& out_pos);
```

The override is stored **relative to the rack origin** so recenter still moves
overridden screens as a group (they follow `rack_q`/`rack_p` like every other
screen). This keeps recenter coherent.

### 4. Retarget manipulations — `spatial-screens/src/main.cpp`

Where the gesture block currently mutates rack-global scales, branch on
`active_screen`:

Once a screen is active it **always carries an override** (selection seeds it —
see below), so the override world pose is the single source of truth for that
screen's placement; `cfg.azimuth/elevation/distance` go dormant for it. The
retargeted manipulations therefore write the *override*, not the az/el/dist
formula:

- **One-hand pinch-drag distance** (and the `[` `]` hotkeys): if
  `active_screen>=0`, scale the active screen's `cfg.pose_pos` length — i.e.
  move it toward/away from the rack origin along its current direction, clamped
  to `[0.25, 10] m`. (For an overridden screen `cfg.distance` is inert, so
  "distance" is expressed as `|pose_pos|`.) Else keep `rack_dist_scale`.
- **Size hotkeys (`-` `=`) / two-hand spread:** if `active_screen>=0`, adjust
  that screen's `cfg.size` (clamped `[10, 400]` in / to `grab_update`'s
  `[GRAB_DIAG_MIN, GRAB_DIAG_MAX]`); else keep `rack_size_scale`. (Size is
  independent of the pose override.)
- **Two-hand grab reposition:** if `active_screen>=0`, feed the *active screen's*
  current world pos as `grab_begin`'s `anchor0` (and its `cfg.size` as `size0`),
  and on each `grab_update` write the returned anchor into that screen's
  `cfg.pose_pos` (convert world→rack-frame via the `world_to_rack_frame` helper),
  and its diag into `cfg.size`. Orientation is held fixed in this feature (the
  vertical-placement feature adds face-the-user on release). Else keep today's
  rack-anchor path.

Selecting a screen seeds its override from its current formula-derived pose (via
`world_to_rack_frame`, so `scene_screen_pose` reproduces the exact same pose),
setting `has_pose_override = true`. There is therefore no jump between
"rack-derived" and "overridden", and every subsequent manipulation has a
well-defined override to write.

### 5. Highlight the active screen — green outside border (decided 2026-07-06)

**Requirement:** the active screen is framed by a green border that sits *on the
outside* of the screen and does **not** obstruct the content inside. This rules
out the tint approach (multiplying the screen's pixels by a color colors the
content) — the highlight is pure additive geometry outside the content rect.

**Mechanism — four coplanar green edge-bars.** In `build_eye`'s per-screen loop,
after the active screen's textured quad is emitted, emit **4 thin solid-color
quads** that share that screen's `smvp` (so they lie in the screen's plane, at
its exact world distance/orientation, and therefore get correct per-eye stereo
for free — same as every other head/world-locked quad). Each bar is placed just
*outside* the content rect `[±w2, ±h2]` (with `w2,h2` the screen's half-extents
in meters, as computed today):

```
  top     rect = ( 0,          +(h2 + b/2),  w2 + b,  b/2 )
  bottom  rect = ( 0,          -(h2 + b/2),  w2 + b,  b/2 )
  left    rect = ( -(w2+b/2),   0,           b/2,     h2  )
  right   rect = ( +(w2+b/2),   0,           b/2,     h2  )
```

Top/bottom extend `±b` wider so the corners fill. Because the four bars are
disjoint from `[±w2, ±h2]`, they **cannot overlap the content** — the inside is
untouched. Being coplanar with the screen they never z-fight it (disjoint
regions; the app has no depth buffer and relies on painter's order). Each bar is
`textured = false`, `circle = false`, so it reuses the existing solid-quad path
— **no shader or pipeline change**.

Emitting the bars *inside* the far→near per-screen loop (right after the active
screen's own quad) keeps painter's order correct: a nearer screen drawn later
still overdraws a farther active screen's border.

**Parameters (tunable on the hardware pass):**

- **Thickness** `b = 0.01 m` (fixed, world-space at the screen plane).
- **Color** = the existing `status_green` `{0.20, 0.90, 0.30, 1.0}` (the same
  green as the pinch-status feedback; reused deliberately for one UI green).

Which screen is active is independent of gaze *after* selection — the border
stays on the selected screen until re-selection/deselect, so `active_screen` is
matched against the sorted draw order by pointer identity
(`order[i].s == &scene[active_screen]`), not by loop index.

**Draw-list cap.** `main.cpp` sizes the per-eye list `QuadDraw draws[2][64]` with
a budget comment summing to 61 ≤ 64. The 4 active-screen bars push the worst case
to 65, so the cap grows to **72** and the budget comment is updated to include
"+4 active-screen border bars". The per-screen `nd` guard uses the new cap.

## Data flow / state machine

Per render frame, per armed hand, in priority order (extends the two-hand
arbitration already in `main.cpp`):

1. Two-hand grab (unchanged priority) — but now writes the **active** screen if
   one is selected, else the rack.
2. `two_up` rising edge → gaze-center pick → set/clear `active_screen`, disarm.
3. `fist`-hold → recenter (unchanged; always rack-global — recenter is a global
   act).
4. One-hand pinch-drag → distance on active screen else rack.

`active_screen` is validated every frame against `scene.size()` (a screen count
can't currently change at runtime, but the floating-window feature will add
screens — clamp defensively).

## Error handling / risks

- **`two_up` vs `point` confusion.** Both extend the index. The ring+pinky-curled
  test separates them; the optional tip-adjacency guard is the escape hatch if
  hardware shows false triggers. Isolated to `classify.py` + its constant.
- **Gaze pick ambiguity when screens overlap in view.** The cone + max-dot picks
  the most-centered; re-doing the gesture re-picks. Acceptable; a future
  refinement could cycle among screens inside the cone on repeat gestures.
- **Selection persistence across recenter.** Overrides are rack-relative, so
  recenter keeps selection and layout coherent. A full VIO reset
  (`Shift+R`) re-seeds the rack; overridden screens keep their rack-relative
  offsets (documented behavior — they move with the rack, not the room).
- **Gestures remain optional.** If the sidecar is unavailable, `active_screen`
  stays -1 and everything is rack-global as today. No new hard dependency.

## Testing

- **Python (pytest):** `two_up` classification (both orientations) + negatives
  vs `point`/`open_palm`/spread-V; adjacency threshold if enabled.
- **C++ pure units:** `pick_gaze_screen` (screen dead-center wins; screen behind
  the head excluded by the cone; empty rack → -1); the world↔rack-frame
  conversion round-trip for the pose override; `scene_screen_pose` honors an
  override and ignores az/el/dist when one is set.
- **Hardware pass:** select each screen by looking at it; confirm the highlight
  is legible through the optics; move one screen's distance and confirm the
  others don't move; deselect (look away + gesture) and confirm gestures return
  to rack-global.

## Future ideas (documented, not built)

- **Cycle-within-cone**: repeated `two_up` while multiple screens are in the
  cone cycles among them (nearest-first).
- **Per-screen layout persistence** to the state file (pos/ori/size per screen),
  restored on launch — folds into M4 layout save/restore.
- **Point-to-select** as an alternative once a stable hand-ray exists (parked in
  favor of gaze-center for v1).

## Files touched

- `spatial-screens/gestures/classify.py` — `two_up` pose (+ optional adjacency).
- `spatial-screens/gestures/tests/test_classify.py` — pose tests.
- `spatial-screens/src/config.h` — `ScreenCfg` pose-override fields.
- `spatial-screens/src/scene.cpp` / `scene.h` — override short-circuit in
  `scene_screen_pose`.
- `spatial-screens/src/main.cpp` — `active_screen` state, gaze-center pick,
  retarget branch in the gesture block, the 4 green border bars in `build_eye`,
  and the `draws[2][64]`→`[2][72]` cap bump (+ budget comment).
- No renderer/shader change — the border reuses the existing solid-quad path
  (`textured = false`). `vk_renderer.*` and `shaders/*` are untouched.
- New pure helper + unit test for `pick_gaze_screen` (e.g. in
  `stereo_math_test` or a new `selection_test`).
- `docs/specs/2026-07-06-screen-selection-design.md` — this document.
- `docs/branches/feat-screen-selection.md` — branch resume doc (created when the
  worktree starts).
