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
  ├─ highlight the active ScreenInst in the overlay + tint uniform
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

`scene_screen_pose` gains a short-circuit at the top:

```cpp
if (s.cfg.has_pose_override) {
    out_q = qmul(rack_q, s.cfg.pose_ori);
    out_p = { rack_p.x + qrot(rack_q, s.cfg.pose_pos).x, ... };
    return;
}
// else: existing yaw/pitch az-el-dist formula
```

The override is stored **relative to the rack origin** so recenter still moves
overridden screens as a group (they follow `rack_q`/`rack_p` like every other
screen). This keeps recenter coherent.

### 4. Retarget manipulations — `spatial-screens/src/main.cpp`

Where the gesture block currently mutates rack-global scales, branch on
`active_screen`:

- **One-hand pinch-drag distance:** if `active_screen>=0`, adjust that screen's
  `cfg.distance` (clamped as today); else keep `rack_dist_scale`.
- **Size hotkeys / two-hand spread:** if `active_screen>=0`, adjust that
  screen's `cfg.size`; else keep `rack_size_scale`.
- **Two-hand grab reposition:** if `active_screen>=0`, feed the *active screen's*
  current world pos as `grab_begin`'s `anchor0`, and on each `grab_update` write
  the returned anchor into that screen's `cfg.pose_pos` with
  `has_pose_override = true` (convert world→rack-frame with `qconj(rack_q)`).
  Orientation is still held fixed in this feature (the vertical-placement
  feature adds face-the-user on release). Else keep today's rack-anchor path.

Selecting a screen for the first time seeds its override from its current
formula-derived pose, so there's no jump between "rack-derived" and "overridden".

### 5. Highlight the active screen — renderer + overlay

- Pass a per-screen `highlighted` flag into the draw. Two cheap options; pick
  one on hardware:
  - **Tint uniform / push-constant**: `quad.frag` multiplies by a highlight
    color (subtle warm tint or a brightened border band computed from UV). One
    new push-constant float; no new geometry.
  - **Border quad**: the overlay path (already draws hand dots / cursor quads)
    draws a thin frame around the active screen's quad in view space.
- Default to the tint (no geometry, works identically in mono and stereo). The
  overlay border is the fallback if the tint reads poorly through the optics.

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
  retarget branch in the gesture block, highlight flag.
- `spatial-screens/src/vk_renderer.*` + `shaders/quad.frag` — highlight tint
  (or overlay border).
- New pure helper + unit test for `pick_gaze_screen` (e.g. in
  `stereo_math_test` or a new `selection_test`).
- `docs/specs/2026-07-06-screen-selection-design.md` — this document.
- `docs/branches/feat-screen-selection.md` — branch resume doc (created when the
  worktree starts).
