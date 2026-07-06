# feat/screen-selection — per-screen selection & independent manipulation

Branch resume doc. Update this as the branch evolves; it must always be enough
to pick the work back up cold.

## Goal

Add the concept of an **active screen** to the `spatial-screens` multi-screen
stereo rack: a discrete gaze-based select gesture picks one screen out of the
rack, highlights it, and retargets the existing one-hand distance / two-hand
grab / size hotkey manipulations to that single screen instead of the whole
rack. With nothing selected, every existing path behaves exactly as it does on
`main` today (rack-global) — fully backward compatible.

This is a **foundation feature**: it is the enabler for two roadmap items that
both need "act on ONE screen, not the whole rack" — vertical placement
(face-the-user orientation) and floating-window screens. Both are designed to
build on the per-screen pose override introduced here.

## Design of record

- `docs/specs/2026-07-06-screen-selection-design.md` — approved 2026-07-06.
- `docs/specs/2026-07-06-screen-selection-plan.md` — task-by-task implementation
  plan, executed subagent-driven per the plan's own recommendation.

Builds on `feat/two-hand-gestures` (per-hand `HandState`, per-hand arming,
one-hand pinch-drag distance, two-hand grab in `gesture_manip.{h,cpp}`) and
`feat/stereo-3d` (multi-screen rack model, stereo SBS rendering,
`scene_screen_pose`, `rack_q`/`rack_p`) — both already on `main`.

## Key decisions

- **Green outside border** = 4 solid-color quads sharing the active screen's
  `smvp`, placed outside the content rect `[±w2,±h2]` (top/bottom widened by
  `b` to fill the corners). Coplanar with the screen (correct per-eye stereo
  for free, same as any head/world-locked quad), reuses the existing
  solid-quad path (`textured = false`) — no shader or pipeline change — and by
  construction (disjoint from `[±w2,±h2]`) never obstructs the content. Color
  = the existing pinch-status green `{0.20, 0.90, 0.30, 1.0}` (deliberately
  reused, one UI green). Thickness `SELECT_BORDER_M = 0.01 m`, fixed.
- **Per-screen pose override supersedes the az/el/dist formula.** Once a
  screen carries `has_pose_override = true`, `scene_screen_pose` short-circuits
  to `pose_pos`/`pose_ori` and `azimuth`/`elevation`/`distance`/`dist_scale` go
  dormant for that screen. The override is stored **rack-relative** (via
  `world_to_rack_frame`) so recenter still moves it coherently with the rest of
  the rack, not with the room.
- **Selection seeds the override** from the screen's current formula-derived
  pose at the moment of selection (`world_to_rack_frame` on the formula's
  output), so there is an exact round-trip and no visual jump between
  "rack-derived" and "overridden" — every subsequent retarget gesture has a
  well-defined pose to write.
- **Gaze pick**: cone half-angle `SELECT_CONE_DEG = 40°`, scored by
  `dot(normalize(screen_pos - head_p), head_forward)` in the recentered head
  frame (so it matches the screens' world frame). Picking with nothing in the
  cone (e.g. looking away from every screen) returns `-1`, which doubles as
  **deselect** — no separate deselect gesture needed.
- **`active_screen` matched by pointer identity** (`order[i].s ==
  &scene[active_screen]`) against the distance-sorted per-eye draw order, not
  by loop index, so the border stays pinned to the right screen regardless of
  draw-order changes.
- **Fully backward compatible.** `active_screen == -1` leaves every
  rack-global code path byte-for-byte unchanged from `main`. Gestures remain
  optional — no sidecar connected ⇒ `active_screen` stays `-1` forever, no new
  hard dependency.
- **Selection is a discrete, armed action**: `two_up` only fires selection on
  the *rising edge* of an armed hand (open palm arms; firing disarms), so a
  held pose can't repeatedly reselect, and it takes top priority over
  fist/pinch in the per-hand branch.

## Tunable constants (all in `spatial-screens/src/main.cpp`, beside the other
gesture constants near `GRAB_DIAG_MAX`)

| Constant | Value | Meaning |
|---|---|---|
| `SELECT_CONE_DEG` | `40.f` | Gaze cone half-angle for `pick_gaze_screen`. |
| `SELECT_BORDER_M` | `0.01f` | Green border bar thickness, world-space meters at the screen plane. |
| selection green | `{0.20, 0.90, 0.30, 1.0}` | Reuses `status_green` (pinch-status feedback color) — not a new constant, deliberately shared. |

An optional index/middle tip-adjacency guard for `two_up` (to reject a spread
V) was designed but **not built** — start without it; add only if hardware
shows false triggers against a spread hand (see design doc §Error handling).

## Implementation map (commits on this branch)

Base: `08a4766` (design + plan commit), branched from `main` @ `80e3a23`.

| Task | Commit | What |
|------|--------|------|
| 1 | `e0294c7` | `two_up` select pose in `gestures/classify.py` (+ fixed the `MIXED_CURL` fixture, which was itself `two_up`) |
| 2 | `5261c79` | `ScreenCfg` pose override (`has_pose_override`/`pose_pos`/`pose_ori`) + `scene_screen_pose` override short-circuit + `world_to_rack_frame` helper (`scene.{h,cpp}`, `config.h`, Makefile) |
| 3 | `7d93c05` | `pick_gaze_screen` gaze-center pick helper (`scene.{h,cpp}`) |
| 4 | `33b574c` | `active_screen` state + `two_up` rising-edge gaze-select + seed override + disarm (`main.cpp`); `SELECT_CONE_DEG`/`SELECT_BORDER_M` constants |
| 5 | `3363e6c` | retarget distance/size hotkeys + one-hand pinch-drag + two-hand grab to the active screen's override/size (`main.cpp`) |
| 6 | `bae4fb9` | green outside border: 4 coplanar edge bars around the active screen; draw cap `64→72` (`main.cpp`) |
| 7 | `1ceea36` | branch resume doc (this file) |
| fix | `9e02f7e` | final-review fixes: gaze pick applies `pitch_trim` (`head_pick = qmul(head_rc, trim)`); **full size-detach** of the selected screen (fold `rack_size_scale` into `cfg.size` once at select → no jump, then render overridden screens without it); de-dup the selection green into one hoisted `status_green` |

## Deferred / next-feature notes

- **Orientation is held fixed on grab** in this feature — the two-hand grab
  writes the active screen's position and size, not its orientation.
  Face-the-user orientation on release is the *next* feature
  (vertical-placement design), which builds on the pose-override substrate
  landed here.
- **Per-screen layout persistence across runs is not written** (v1 non-goal;
  the override is in-memory only, the state file untouched here). Full
  save/restore is M4 roadmap.
- Cycle-within-cone (repeated `two_up` cycling among overlapping screens) and
  point-to-select are documented future ideas in the design doc, not built.

## Current state / next step

- [x] Design doc written + approved (2026-07-06) —
      `docs/specs/2026-07-06-screen-selection-design.md`
- [x] Implementation plan —
      `docs/specs/2026-07-06-screen-selection-plan.md`
- [x] Implementation — **all 6 code tasks done**, every unit test green, clean
      build. Executed subagent-driven per-task with independent review.
- [x] **Full clean-build verification (2026-07-06):** `make clean && make &&
      make test` — clean build (only the known pre-existing unrelated
      `bridge/ws_server.hpp` `-Wmissing-field-initializers` warning, from
      `wsrv::Server::accept_new()`, present on `main` before this branch);
      `gesture_parse_test`, `gesture_manip_test`, `stereo_math_test` all "all
      checks passed"; `pytest tests/test_classify.py -v` — 15/15 passed.
- [x] **Whole-branch review + fix wave (2026-07-06, opus).** Verdict: merge with
      fixes — backward-compat airtight, no Critical. Two Important fixed in
      `9e02f7e` (pitch_trim gaze pick; full size-detach per user choice); shared
      green de-duped. Re-verified sound. Open **backlog** (non-blocking): grab
      diag clamp `[20,200]` vs size-hotkey `[10,400]` mismatch (now in
      visible-size space after the detach); no "reset screen to rack" affordance
      yet; exact-cone-boundary unit test.
- [ ] **Hardware pass (next step, user-driven on the glasses).** In a
      multi-screen rack + stereo session, confirm:
    - [ ] **Vertical select under `pitch_trim = 16`:** looking at a screen
          above/below center selects the one you're actually aiming at (this is
          what the `pitch_trim` pick fix targets — the reason to test it live).
    - [ ] **Selecting in a globally-scaled rack** (after `-`/`=` global resize)
          produces **no size pop** on the selected screen, and subsequent per-
          screen resizes feel 1:1.
    - [ ] `two_up` while looking at a screen selects it; a **green border
          appears outside** that screen and the content inside is untouched;
          both eyes show it.
    - [ ] One-hand pinch-drag / `[` `]` moves only the active screen's
          distance; the other screens don't move. `-` `=` resizes only the
          active screen. Two-hand grab repositions/resizes only the active
          screen.
    - [ ] `two_up` while looking away from all screens (or re-selecting)
          deselects → gestures return to rack-global control.
    - [ ] Recenter keeps the selection + layout coherent (overridden screens
          stay put relative to the rack, not the room).
