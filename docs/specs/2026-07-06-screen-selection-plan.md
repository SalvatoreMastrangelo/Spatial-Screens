# Per-Screen Selection & Independent Manipulation — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an "active screen" you select by looking at it and making an
index+middle "two_up" pose, framed by a green outside border, so one-hand
distance / two-hand grab / size+distance hotkeys retarget to that one screen
instead of the whole rack.

**Architecture:** One new sidecar pose (`two_up`, pure landmark math). One new
per-screen world **pose override** on `ScreenCfg`, consumed by
`scene_screen_pose` (pure). One pure gaze-pick helper. The render loop gains an
`active_screen` int, a rising-edge select + seed-override + disarm branch, a
retarget branch on the existing gesture/hotkey paths, and 4 green border quads in
`build_eye`. No socket/protocol/shader/pipeline changes — the pose rides the
existing `HandState.pose` string and the border reuses the solid-quad path.

**Tech Stack:** C++17 (`spatial-screens/src`, no-framework `CHECK`-macro unit
tests), Python 3.10 + pytest (`spatial-screens/gestures`), Vulkan push-constant
quad renderer, GNU Make.

Design of record: [`2026-07-06-screen-selection-design.md`](2026-07-06-screen-selection-design.md).

## Global Constraints

- **Backward compatible:** `active_screen == -1` (nothing selected) must behave
  exactly as `main` does today (rack-global). Gestures stay optional — no sidecar
  ⇒ `active_screen` stays `-1`.
- **Pure logic is unit-tested; `main.cpp` wiring is build- + review- + hardware-
  verified.** Follow the existing split: math lives in `scene.{h,cpp}` /
  `classify.py` with tests; the render-loop integration in `main.cpp` has no unit
  harness (SDK/X11/Vulkan) and is covered by `make` + a whole-branch review + a
  glasses pass, exactly as the two-hand feature was.
- **Override supersedes the formula:** once `has_pose_override`, that screen's
  `azimuth/elevation/distance` are inert; position/orientation come only from
  `pose_pos`/`pose_ori`. `dist_scale` is ignored for overridden screens.
- **Tunable constants** (pin on the hardware pass): `SELECT_CONE_DEG = 40`,
  `SELECT_BORDER_M = 0.01`, selection green = `status_green` `{0.20,0.90,0.30,1}`.
- **Every commit** ends with the repo's required trailer lines
  (`Co-Authored-By:` + `Claude-Session:` per the environment). Subjects shown per
  task; append the trailers when committing.
- **Test commands** (run from `spatial-screens/`):
  - Python: `cd gestures && python3 -m pytest tests/test_classify.py -v`
  - C++ pure logic: `make stereo-math-test && ./stereo-math-test`
  - Full C++ suite: `make test`
  - Build the app: `make` (must stay warning-clean).

---

## File Structure

- `gestures/classify.py` — add the `two_up` pose to `classify_pose`.
- `gestures/tests/test_classify.py` — `two_up` tests; fix the `MIXED_CURL` fixture
  (it IS `two_up` now).
- `src/config.h` — `ScreenCfg` pose-override fields; `#include "pose_math.h"`.
- `src/scene.h` / `src/scene.cpp` — override short-circuit in `scene_screen_pose`;
  new pure helpers `world_to_rack_frame` and `pick_gaze_screen`.
- `src/stereo_math_test.cpp` — tests for the override round-trip and the gaze pick.
- `src/main.cpp` — `active_screen` state + constants; `two_up` rising-edge select
  (gaze pick, seed override, disarm); retarget the distance/size gestures +
  hotkeys + two-hand grab; 4 green border quads in `build_eye`; draw-cap
  `64 → 72`.
- `Makefile` — add `src/pose_math.h` to the `config.o` prerequisites.

---

## Task 1: `two_up` pose classification (Python)

**Files:**
- Modify: `spatial-screens/gestures/classify.py:57-72` (`classify_pose`)
- Test: `spatial-screens/gestures/tests/test_classify.py`

**Interfaces:**
- Consumes: existing `classify_pose(landmarks)`, the `curled` list
  `[index, middle, ring, pinky]`.
- Produces: `classify_pose` returns the new string `"two_up"` for index+middle
  extended, ring+pinky curled. Consumed later only as a `HandState.pose` string
  (no C++ change needed).

> **Why the fixture fix:** `test_classify.py`'s `MIXED_CURL` (index+middle
> extended, ring+pinky curled) currently asserts `"none"` — that landmark set is
> exactly `two_up`. Adding the pose makes that assertion wrong, so this task both
> adds `two_up` and repoints that fixture at a still-genuinely-`none` pose.

- [ ] **Step 1: Write the failing tests** (append to `tests/test_classify.py`)

Add a `two_up` fixture and its tests, and repoint the old `MIXED_CURL` test at a
pose that is still `none` (index+middle+ring extended, only pinky curled):

```python
# two_up: index + middle extended, ring + pinky curled (the "select" pose).
TWO_UP = make_landmarks({
    WRIST: (0.5, 0.9),
    MIDDLE_MCP: (0.5, 0.6),
    INDEX_PIP: (0.45, 0.5), INDEX_TIP: (0.45, 0.2),      # extended
    MIDDLE_PIP: (0.50, 0.5), MIDDLE_TIP: (0.50, 0.15),   # extended
    RING_PIP: (0.55, 0.65), RING_TIP: (0.55, 0.78),      # curled
    PINKY_PIP: (0.60, 0.68), PINKY_TIP: (0.60, 0.80),    # curled
    THUMB_TIP: (0.35, 0.6),
})

# Still genuinely "none": index+middle+ring extended, only pinky curled — not
# fist / open_palm / point / two_up.
THREE_UP = make_landmarks({
    WRIST: (0.5, 0.9),
    MIDDLE_MCP: (0.5, 0.6),
    INDEX_PIP: (0.45, 0.5), INDEX_TIP: (0.45, 0.2),      # extended
    MIDDLE_PIP: (0.50, 0.5), MIDDLE_TIP: (0.50, 0.15),   # extended
    RING_PIP: (0.55, 0.5), RING_TIP: (0.55, 0.2),        # extended
    PINKY_PIP: (0.60, 0.68), PINKY_TIP: (0.60, 0.80),    # curled
    THUMB_TIP: (0.35, 0.6),
})


def test_two_up_classified_correctly():
    assert classify_pose(TWO_UP) == "two_up"


def test_two_up_is_not_point():
    # point = index only; two_up additionally extends the middle finger.
    assert classify_pose(POINT) == "point"
    assert classify_pose(TWO_UP) != "point"


def test_two_up_is_not_open_palm():
    assert classify_pose(TWO_UP) != "open_palm"


def test_three_extended_fingers_is_none():
    assert classify_pose(THREE_UP) == "none"
```

Then delete the now-wrong `MIXED_CURL` fixture (lines ~47-58) and its test
`test_mixed_curl_classified_as_none` (lines ~73-74) — `THREE_UP` replaces its
role of exercising the `none` fallthrough.

- [ ] **Step 2: Run tests to verify the new ones fail**

Run: `cd spatial-screens/gestures && python3 -m pytest tests/test_classify.py -v`
Expected: `test_two_up_classified_correctly` FAILS (`assert 'none' == 'two_up'`);
`test_three_extended_fingers_is_none` PASSES already; others pass.

- [ ] **Step 3: Add the `two_up` branch to `classify_pose`**

In `classify.py`, insert after the `point` case and before `return "none"`:

```python
    if not curled[0] and all(curled[1:]):
        return "point"
    if not curled[0] and not curled[1] and curled[2] and curled[3]:
        return "two_up"
    return "none"
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd spatial-screens/gestures && python3 -m pytest tests/test_classify.py -v`
Expected: PASS (all, including the pre-existing pose/pinch/select_hand tests).

- [ ] **Step 5: Commit**

```bash
git add spatial-screens/gestures/classify.py spatial-screens/gestures/tests/test_classify.py
git commit -m "feat(gestures): add two_up select pose to classify_pose"
```

---

## Task 2: Per-screen pose override + world↔rack helper (C++ pure)

**Files:**
- Modify: `spatial-screens/src/config.h:12-17` (`ScreenCfg`)
- Modify: `spatial-screens/src/scene.h`, `spatial-screens/src/scene.cpp`
- Modify: `spatial-screens/Makefile` (`config.o` prereqs)
- Test: `spatial-screens/src/stereo_math_test.cpp`

**Interfaces:**
- Consumes: `Quat`, `Vec3`, `qmul`, `qconj`, `qrot` from `pose_math.h`;
  `ScreenInst`, `scene_screen_pose` from `scene.h`.
- Produces:
  - `ScreenCfg` gains `bool has_pose_override = false; Vec3 pose_pos; Quat pose_ori;`
  - `void world_to_rack_frame(const Quat& rack_q, const Vec3& rack_p, const Quat& world_q, const Vec3& world_p, Quat& out_ori, Vec3& out_pos);`
  - `scene_screen_pose` short-circuits on `has_pose_override` (ignoring `dist_scale`).

- [ ] **Step 1: Write the failing tests** (add to `stereo_math_test.cpp`)

Add a new test function and call it from `main()`:

```cpp
static void test_pose_override() {
    MonRect fb{"eDP-1", 0, 0, 1920, 1200};
    std::vector<MonRect> mons = {{"VS1", 0, 0, 1920, 1200}};
    std::vector<ScreenCfg> cfg(1); cfg[0].monitor = "VS1";
    auto s = scene_build(cfg, mons, fb);

    // A non-trivial rack (yaw 30°, translated) and an arbitrary world pose.
    Quat rack_q = quat_axis_angle(0, 1, 0, 30.f);
    Vec3 rack_p{1.f, 2.f, 3.f};
    Quat world_q = quat_axis_angle(0, 1, 0, 90.f);
    Vec3 world_p{4.f, 5.f, 6.f};

    // world_to_rack_frame then scene_screen_pose must round-trip to the world
    // pose, and dist_scale must be IGNORED once the override is set.
    world_to_rack_frame(rack_q, rack_p, world_q, world_p,
                        s[0].cfg.pose_ori, s[0].cfg.pose_pos);
    s[0].cfg.has_pose_override = true;

    Quat q; Vec3 p;
    scene_screen_pose(s[0], rack_q, rack_p, /*dist_scale*/7.f, q, p);  // scale ignored
    CHECK(std::fabs(p.x - 4.f) < 1e-4f);
    CHECK(std::fabs(p.y - 5.f) < 1e-4f);
    CHECK(std::fabs(p.z - 6.f) < 1e-4f);
    // Orientation round-trips (compare as rotated forward axes to avoid sign).
    Vec3 f_out = qrot(q, {0, 0, -1});
    Vec3 f_want = qrot(world_q, {0, 0, -1});
    CHECK(std::fabs(f_out.x - f_want.x) < 1e-4f &&
          std::fabs(f_out.y - f_want.y) < 1e-4f &&
          std::fabs(f_out.z - f_want.z) < 1e-4f);

    // Without the override flag, the az/el/dist formula still runs (regression).
    s[0].cfg.has_pose_override = false;
    s[0].cfg.azimuth = 0; s[0].cfg.elevation = 0; s[0].cfg.distance = 1.f;
    scene_screen_pose(s[0], Quat{}, Vec3{0,0,0}, 1.f, q, p);
    CHECK(std::fabs(p.z + 1.f) < 1e-4f);  // 1 m straight ahead, formula path
}
```

Add `test_pose_override();` to `main()` before the results print.

- [ ] **Step 2: Run to verify it fails to compile**

Run: `cd spatial-screens && make stereo-math-test`
Expected: FAIL — `world_to_rack_frame` undeclared and `ScreenCfg` has no
`pose_pos`/`pose_ori`/`has_pose_override`.

- [ ] **Step 3: Add the override fields to `ScreenCfg`**

In `config.h`, add the include and fields:

```cpp
#pragma once
#include <string>
#include <vector>
#include "pose_math.h"   // Vec3, Quat for the per-screen pose override
```

```cpp
struct ScreenCfg {
    std::string monitor;
    float azimuth = 0.f, elevation = 0.f;
    float distance = 0.75f;
    float size = 24.f;
    // Free-placement override (set once a gesture selects/moves this screen).
    // Stored relative to the rack origin so recenter moves it with the rack.
    // When set, scene_screen_pose ignores azimuth/elevation/distance/dist_scale.
    bool has_pose_override = false;
    Vec3 pose_pos;   // world position in the rack frame
    Quat pose_ori;   // world orientation in the rack frame
};
```

- [ ] **Step 4: Declare + implement the helper and the short-circuit**

In `scene.h`, add after `scene_screen_pose`'s declaration:

```cpp
// Inverse of scene_screen_pose's override branch: express a world pose as a
// rack-relative (pose_ori, pose_pos) so scene_screen_pose reproduces it.
void world_to_rack_frame(const Quat& rack_q, const Vec3& rack_p,
                         const Quat& world_q, const Vec3& world_p,
                         Quat& out_ori, Vec3& out_pos);
```

In `scene.cpp`, add the short-circuit at the top of `scene_screen_pose`:

```cpp
void scene_screen_pose(const ScreenInst& s, const Quat& rack_q, const Vec3& rack_p,
                       float dist_scale, Quat& out_q, Vec3& out_p) {
    if (s.cfg.has_pose_override) {
        out_q = qmul(rack_q, s.cfg.pose_ori);
        Vec3 w = qrot(rack_q, s.cfg.pose_pos);
        out_p = { rack_p.x + w.x, rack_p.y + w.y, rack_p.z + w.z };
        return;
    }
    // yaw(-azimuth): +azimuth = user's right = -y rotation in EUS.
    Quat rot = qmul(quat_axis_angle(0, 1, 0, -s.cfg.azimuth),
                    quat_axis_angle(1, 0, 0, s.cfg.elevation));
    // ... unchanged ...
}
```

And append the helper implementation to `scene.cpp`:

```cpp
void world_to_rack_frame(const Quat& rack_q, const Vec3& rack_p,
                         const Quat& world_q, const Vec3& world_p,
                         Quat& out_ori, Vec3& out_pos) {
    Quat inv = qconj(rack_q);
    Vec3 d = { world_p.x - rack_p.x, world_p.y - rack_p.y, world_p.z - rack_p.z };
    out_pos = qrot(inv, d);
    out_ori = qmul(inv, world_q);
}
```

In `Makefile`, add the header to the `config.o` prerequisites:

```makefile
src/config.o: src/config.cpp src/config.h src/pose_math.h
```

- [ ] **Step 5: Run to verify it passes**

Run: `cd spatial-screens && make stereo-math-test && ./stereo-math-test`
Expected: PASS — `stereo_math_test: all checks passed`.

- [ ] **Step 6: Commit**

```bash
git add spatial-screens/src/config.h spatial-screens/src/scene.h spatial-screens/src/scene.cpp spatial-screens/src/stereo_math_test.cpp spatial-screens/Makefile
git commit -m "feat(scene): per-screen pose override + world_to_rack_frame helper"
```

---

## Task 3: `pick_gaze_screen` gaze-center pick (C++ pure)

**Files:**
- Modify: `spatial-screens/src/scene.h`, `spatial-screens/src/scene.cpp`
- Test: `spatial-screens/src/stereo_math_test.cpp`

**Interfaces:**
- Consumes: `Vec3`, `Quat`, `qrot` from `pose_math.h`.
- Produces:
  `int pick_gaze_screen(const std::vector<Vec3>& screen_pos, const Vec3& head_p, const Quat& head_q, float cone_deg);`
  — index of the screen whose direction from the head is closest to head-forward
  (`-z` rotated by `head_q`) and within `cone_deg`; `-1` if none qualify or the
  list is empty.

- [ ] **Step 1: Write the failing tests** (add to `stereo_math_test.cpp`)

```cpp
static void test_pick_gaze_screen() {
    Quat head_q;                 // identity → forward = (0,0,-1)
    Vec3 head_p{0, 0, 0};

    // Dead-center screen wins over an off-axis one.
    std::vector<Vec3> two = { {0, 0, -2},      // straight ahead, dot = 1
                              {2, 0, -2} };     // 45° off, dot ≈ 0.707
    CHECK(pick_gaze_screen(two, head_p, head_q, 40.f) == 0);

    // The 45° screen alone is outside a 40° cone → nothing selected.
    std::vector<Vec3> side = { {2, 0, -2} };
    CHECK(pick_gaze_screen(side, head_p, head_q, 40.f) == -1);
    // ...but a wider cone admits it.
    CHECK(pick_gaze_screen(side, head_p, head_q, 50.f) == 0);

    // A screen behind the head is excluded (dot < 0).
    std::vector<Vec3> behind = { {0, 0, 2} };
    CHECK(pick_gaze_screen(behind, head_p, head_q, 40.f) == -1);

    // Empty rack → -1.
    CHECK(pick_gaze_screen({}, head_p, head_q, 40.f) == -1);

    // Degenerate: a screen exactly at the head position is skipped, not NaN.
    std::vector<Vec3> at_head = { {0, 0, 0}, {0, 0, -1} };
    CHECK(pick_gaze_screen(at_head, head_p, head_q, 40.f) == 1);
}
```

Add `test_pick_gaze_screen();` to `main()`.

- [ ] **Step 2: Run to verify it fails to compile**

Run: `cd spatial-screens && make stereo-math-test`
Expected: FAIL — `pick_gaze_screen` undeclared.

- [ ] **Step 3: Declare + implement**

In `scene.h`:

```cpp
// Gaze-center pick: index of the screen whose direction from head_p is most
// aligned with head-forward (head_q · -z) and within cone_deg; -1 if none.
// A screen coincident with the head (zero direction) is skipped.
int pick_gaze_screen(const std::vector<Vec3>& screen_pos,
                     const Vec3& head_p, const Quat& head_q, float cone_deg);
```

In `scene.cpp` (add `#include <cmath>` if not already present — it is via headers;
keep it explicit):

```cpp
int pick_gaze_screen(const std::vector<Vec3>& screen_pos,
                     const Vec3& head_p, const Quat& head_q, float cone_deg) {
    Vec3 fwd = qrot(head_q, {0, 0, -1});
    float cos_cone = std::cos(cone_deg * float(M_PI) / 180.f);
    int best = -1;
    float best_dot = cos_cone;   // must beat the cone edge to qualify
    for (size_t i = 0; i < screen_pos.size(); i++) {
        Vec3 d = { screen_pos[i].x - head_p.x,
                   screen_pos[i].y - head_p.y,
                   screen_pos[i].z - head_p.z };
        float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (len < 1e-6f) continue;                 // screen at the head — skip
        float dot = (d.x * fwd.x + d.y * fwd.y + d.z * fwd.z) / len;
        if (dot > best_dot) { best_dot = dot; best = int(i); }
    }
    return best;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd spatial-screens && make stereo-math-test && ./stereo-math-test`
Expected: PASS — `stereo_math_test: all checks passed`.

- [ ] **Step 5: Commit**

```bash
git add spatial-screens/src/scene.h spatial-screens/src/scene.cpp spatial-screens/src/stereo_math_test.cpp
git commit -m "feat(scene): pick_gaze_screen gaze-center pick helper"
```

---

## Task 4: Wire selection into the render loop (`main.cpp`)

**Files:**
- Modify: `spatial-screens/src/main.cpp` (constants ~101-105; gesture block
  ~740-835; per-frame validation)

**Interfaces:**
- Consumes: `pick_gaze_screen`, `world_to_rack_frame`, `scene_screen_pose`
  (Tasks 2-3); existing `scene`, `rack_q`, `rack_p`, `rack_dist_scale`,
  `armed[2]`, `head_q`, `head_p`, `ori_offset`, `gev`.
- Produces: render-loop state `int active_screen = -1;`, `bool was_two_up[2]`,
  and a `select_active` behavior consumed by Task 5 (retarget) and Task 6
  (highlight). No new function signatures.

> No unit test: this is SDK/X11/render-loop wiring. Verified by `make` (clean
> build) + the Task 2-3 unit tests of the helpers it calls + the hardware pass.

- [ ] **Step 1: Add the selection constants**

After `GRAB_DIAG_MAX` (~line 105) add:

```cpp
static constexpr float SELECT_CONE_DEG = 40.f;  // gaze cone half-angle for pick
static constexpr float SELECT_BORDER_M = 0.01f; // green border thickness (m)
```

- [ ] **Step 2: Add the active-screen state**

Beside `armed`/`was_pinching` (~line 644-648) add:

```cpp
    int active_screen = -1;              // -1 = none (rack-global); else scene idx
    bool was_two_up[2] = {false, false}; // rising-edge latch per hand for select
```

- [ ] **Step 3: Add the rising-edge gaze-select branch**

In the single-hand loop (the `else` of `grab_now`, ~line 797), make `two_up`
the FIRST per-hand branch so it takes priority over fist/pinch. Insert at the top
of the `for (int i = 0; i < 2; i++)` body:

```cpp
                HandState& h = *hands[i];
                if (armed[i] && h.pose == "two_up") {
                    if (!was_two_up[i]) {           // rising edge only
                        // Recentered head frame — matches the screens' world frame.
                        Quat head_rc = qmul(qconj(ori_offset), head_q);
                        Vec3 hp = qrot(qconj(ori_offset), head_p);
                        std::vector<Vec3> spos(scene.size());
                        for (size_t k = 0; k < scene.size(); k++) {
                            Quat sq; Vec3 sp;
                            scene_screen_pose(scene[k], rack_q, rack_p,
                                              rack_dist_scale, sq, sp);
                            spos[k] = sp;
                        }
                        int pick = pick_gaze_screen(spos, hp, head_rc, SELECT_CONE_DEG);
                        active_screen = pick;        // -1 doubles as deselect
                        if (pick >= 0 && !scene[pick].cfg.has_pose_override) {
                            // Seed the override from the formula pose so retarget
                            // gestures have a well-defined pose and there's no jump.
                            Quat wq; Vec3 wp;
                            scene_screen_pose(scene[pick], rack_q, rack_p,
                                              rack_dist_scale, wq, wp);
                            world_to_rack_frame(rack_q, rack_p, wq, wp,
                                                scene[pick].cfg.pose_ori,
                                                scene[pick].cfg.pose_pos);
                            scene[pick].cfg.has_pose_override = true;
                        }
                        printf("gesture select -> screen %d\n", active_screen);
                        tele.log("info", pick >= 0 ? "screen selected" : "screen deselected");
                        armed[i] = false;            // one action per open-hand arm
                    }
                    was_two_up[i] = true;
                    was_pinching[i] = false;
                    fist_start_s[i] = -1;
                    break;                            // this hand owns the frame
                }
                was_two_up[i] = false;
                if (armed[i] && h.pose == "fist") {
                    // ... existing fist branch unchanged ...
```

Note the existing loop body opened with `HandState& h = *hands[i];` — remove that
now-duplicate line further down (keep the single copy added at the top).

- [ ] **Step 4: Clear the latch when a hand leaves; validate `active_screen`**

In the per-hand arming loop (~line 747), extend the "hand gone" reset:

```cpp
        for (int i = 0; i < 2; i++) {
            if (!hands[i]->present) { armed[i] = false; was_two_up[i] = false; }
            if (hands[i]->pose == "open_palm") armed[i] = true;
        }
        // Defensive: a screen count can shrink (future feature) — never index OOB.
        if (active_screen >= int(scene.size())) active_screen = -1;
```

- [ ] **Step 5: Build to verify it compiles clean**

Run: `cd spatial-screens && make`
Expected: links `spatial-screens`, no warnings. (Selection has no visible effect
yet — retarget is Task 5, highlight Task 6.)

- [ ] **Step 6: Run the full pure suite (no regression)**

Run: `cd spatial-screens && make test`
Expected: all three suites pass.

- [ ] **Step 7: Commit**

```bash
git add spatial-screens/src/main.cpp
git commit -m "feat(spatial): two_up gaze-select sets active_screen + seeds override"
```

---

## Task 5: Retarget manipulations to the active screen (`main.cpp`)

**Files:**
- Modify: `spatial-screens/src/main.cpp` (hotkeys ~687-704; two-hand grab
  ~756-782; one-hand pinch-drag ~811-823)

**Interfaces:**
- Consumes: `active_screen`, `scene`, `rack_q`, `rack_p`, `world_to_rack_frame`,
  `scene_screen_pose`, `grab_begin`/`grab_update`.
- Produces: with `active_screen >= 0`, distance/size hotkeys, one-hand pinch-drag
  distance, and the two-hand grab all write the active screen's override /
  `cfg.size` instead of the rack globals. `active_screen == -1` keeps every
  existing path byte-for-byte.

> No unit test — render-loop wiring. Verified by `make` + hardware. The override
> math it writes is already unit-tested (Task 2).

- [ ] **Step 1: Add a local override-distance helper (once, near `place_rack`)**

After the `place_rack` lambda (~line 621) add a small closure so the pinch-drag
and the `[` `]` hotkeys share one clamp:

```cpp
    // Scale the active screen's override distance from the rack origin by `f`,
    // clamped to a comfortable range. No-op if nothing is selected.
    auto scale_active_distance = [&](float f) {
        if (active_screen < 0) return;
        Vec3& pp = scene[active_screen].cfg.pose_pos;
        float len = std::sqrt(pp.x * pp.x + pp.y * pp.y + pp.z * pp.z);
        if (len < 1e-6f) return;
        float nlen = std::clamp(len * f, 0.25f, 10.f);
        float s = nlen / len;
        pp.x *= s; pp.y *= s; pp.z *= s;
    };
```

(The active screen always has `has_pose_override` true — Task 4 seeds it on
select — so writing `pose_pos` takes effect immediately.)

- [ ] **Step 2: Retarget the distance/size hotkeys**

Replace the four hotkey bodies (`[`, `]`, `-`, `=`, ~lines 687-704) so an active
screen is targeted first:

```cpp
            else if (ks == XK_bracketleft) {
                if (active_screen >= 0) scale_active_distance(0.9f);
                else if (multi) rack_dist_scale = std::max(0.25f, rack_dist_scale * 0.9f);
                else { distance = std::max(0.5f, distance - 0.25f); scene[0].cfg.distance = distance; }
                place_rack();
            }
            else if (ks == XK_bracketright) {
                if (active_screen >= 0) scale_active_distance(1.1f);
                else if (multi) rack_dist_scale = std::min(4.f, rack_dist_scale * 1.1f);
                else { distance = std::min(10.f, distance + 0.25f); scene[0].cfg.distance = distance; }
                place_rack();
            }
            else if (ks == XK_minus) {
                if (active_screen >= 0)
                    scene[active_screen].cfg.size = std::max(10.f, scene[active_screen].cfg.size - 10.f);
                else if (multi) rack_size_scale = std::max(0.4f, rack_size_scale * 0.9f);
                else { diag_in = std::max(10.f, diag_in - 10.f); scene[0].cfg.size = diag_in; }
            }
            else if (ks == XK_equal) {
                if (active_screen >= 0)
                    scene[active_screen].cfg.size = std::min(400.f, scene[active_screen].cfg.size + 10.f);
                else if (multi) rack_size_scale = std::min(3.f, rack_size_scale * 1.1f);
                else { diag_in = std::min(400.f, diag_in + 10.f); scene[0].cfg.size = diag_in; }
            }
```

- [ ] **Step 3: Retarget the two-hand grab (begin + update)**

In the grab block (~lines 756-782), branch `grab_begin` and `grab_update` on
`active_screen`. Replace the `if (!grab.active) { ... } else { ... }` body with:

```cpp
            if (!grab.active) {
                Vec3 rr = qrot(rack_q, { 1, 0, 0 });
                Vec3 uu = qrot(rack_q, { 0, 1, 0 });
                float size0 = diag_in;
                Vec3 anchor0 = { rack_p.x, rack_p.y, rack_p.z };
                if (active_screen >= 0) {
                    Quat sq; Vec3 sp;
                    scene_screen_pose(scene[active_screen], rack_q, rack_p,
                                      rack_dist_scale, sq, sp);
                    anchor0 = sp;
                    size0 = scene[active_screen].cfg.size;
                }
                grab = grab_begin(gev.left.pinch_x, gev.left.pinch_y,
                                  gev.right.pinch_x, gev.right.pinch_y,
                                  size0, { anchor0.x, anchor0.y, anchor0.z },
                                  { rr.x, rr.y, rr.z }, { uu.x, uu.y, uu.z });
                grab_scale0 = rack_size_scale;   // baseline for rack-mode resize
            } else {
                GrabResult gr = grab_update(grab, gev.left.pinch_x, gev.left.pinch_y,
                                            gev.right.pinch_x, gev.right.pinch_y,
                                            distance, GRAB_REPOSITION_GAIN,
                                            GRAB_DIAG_MIN, GRAB_DIAG_MAX);
                if (active_screen >= 0) {
                    // Reposition + resize the ONE active screen (override world pose).
                    Vec3 d = { gr.anchor.x - rack_p.x, gr.anchor.y - rack_p.y,
                               gr.anchor.z - rack_p.z };
                    scene[active_screen].cfg.pose_pos = qrot(qconj(rack_q), d);
                    scene[active_screen].cfg.has_pose_override = true;  // already true; explicit
                    scene[active_screen].cfg.size = gr.diag;
                } else {
                    // Reposition: move the rack origin in its own right/up plane.
                    rack_p = { gr.anchor.x, gr.anchor.y, gr.anchor.z };
                    if (multi) {
                        float ratio = gr.diag / std::max(1e-3f, grab.size0);
                        rack_size_scale = std::clamp(grab_scale0 * ratio, 0.4f, 3.f);
                    } else {
                        diag_in = gr.diag;
                        scene[0].cfg.size = diag_in;
                    }
                }
            }
```

- [ ] **Step 4: Retarget the one-hand pinch-drag distance**

In the pinch-drag branch (~lines 814-823), target the active screen first:

```cpp
                    if (was_pinching[i]) {
                        float dy = h.pinch_y - pinch_prev_y[i]; // image space: +y down
                        if (active_screen >= 0) {
                            scale_active_distance(1.f - dy * PINCH_DISTANCE_SENSITIVITY * 0.5f);
                        } else if (multi) {
                            rack_dist_scale = std::clamp(
                                rack_dist_scale * (1.f - dy * PINCH_DISTANCE_SENSITIVITY * 0.5f),
                                0.25f, 4.f);
                        } else {
                            distance = std::clamp(distance - dy * PINCH_DISTANCE_SENSITIVITY, 0.5f, 10.f);
                            scene[0].cfg.distance = distance;
                        }
                    }
```

- [ ] **Step 5: Build + full suite**

Run: `cd spatial-screens && make && make test`
Expected: clean build, all pure suites pass. Retarget has no unit surface; the
override writes are exercised by Task 2's round-trip test.

- [ ] **Step 6: Commit**

```bash
git add spatial-screens/src/main.cpp
git commit -m "feat(spatial): retarget distance/size/grab gestures to active screen"
```

---

## Task 6: Green outside border on the active screen (`main.cpp`)

**Files:**
- Modify: `spatial-screens/src/main.cpp` (draw-cap ~966-968, 1007; per-screen loop
  ~1006-1023; landmark guard ~1108)

**Interfaces:**
- Consumes: `active_screen`, `scene`, the per-screen `smvp`/`w2`/`h2` already
  computed in `build_eye`, `SELECT_BORDER_M`, the `QuadDraw` solid-quad path.
- Produces: 4 green edge-bars framing the active screen in both eyes. No new
  functions.

> No unit test — pure rendering. Verified by `make` (clean build) + the hardware
> pass (border legible, outside the content, both eyes).

- [ ] **Step 1: Grow the draw-list cap and its budget comment**

Replace the cap comment + array (~lines 965-968):

```cpp
        // Draw-list caps tie together, per eye: config's 16-screen max
        // + 4 active-screen border bars + VO dot + 2 per-hand status dots
        // + 2 hands x 21 landmarks = 65 <= 72. Bump any cap and this grows too.
        QuadDraw draws[2][72];
```

Update the per-screen guard (~line 1007) `if (nd >= 64) break;` → `if (nd >= 72) break;`
and the landmark-loop guard (~line 1108) `i < 21 && nd < 64` → `i < 21 && nd < 72`.

- [ ] **Step 2: Emit the 4 border bars after the active screen's quad**

Inside the per-screen loop, immediately after the textured screen `QuadDraw` is
filled (`d.textured = true;`, ~line 1022), add:

```cpp
                    d.textured = true;
                    // Selected-screen highlight: a green frame OUTSIDE the
                    // content rect [±w2,±h2], coplanar with the screen (shares
                    // smvp) so it gets correct per-eye stereo and never overlaps
                    // the content. order[] is the sorted draw order; match the
                    // active screen by identity, not index.
                    if (active_screen >= 0 && order[i].s == &scene[active_screen]) {
                        const float sel_green[4] = { 0.20f, 0.90f, 0.30f, 1.f };
                        const float b = SELECT_BORDER_M;
                        // top, bottom, left, right (top/bottom widened by b to fill corners)
                        const float bars[4][4] = {
                            { 0.f,        h2 + b * 0.5f, w2 + b,      b * 0.5f },
                            { 0.f,      -(h2 + b * 0.5f), w2 + b,      b * 0.5f },
                            { -(w2 + b * 0.5f), 0.f,      b * 0.5f,    h2       },
                            {  (w2 + b * 0.5f), 0.f,      b * 0.5f,    h2       },
                        };
                        for (int e = 0; e < 4 && nd < 72; e++) {
                            QuadDraw& bd = dl[nd++];
                            memcpy(bd.mvp, smvp, sizeof(smvp));
                            memcpy(bd.color, sel_green, 4 * sizeof(float));
                            bd.rect[0] = bars[e][0]; bd.rect[1] = bars[e][1];
                            bd.rect[2] = bars[e][2]; bd.rect[3] = bars[e][3];
                            bd.textured = false;
                            bd.circle = false;
                        }
                    }
```

- [ ] **Step 3: Build to verify it compiles clean**

Run: `cd spatial-screens && make`
Expected: links `spatial-screens`, no warnings.

- [ ] **Step 4: Full suite (no regression)**

Run: `cd spatial-screens && make test`
Expected: all three suites pass.

- [ ] **Step 5: Commit**

```bash
git add spatial-screens/src/main.cpp
git commit -m "feat(spatial): green outside border on the selected screen"
```

---

## Task 7: Branch resume doc + final verification

**Files:**
- Create: `docs/branches/feat-screen-selection.md`

**Interfaces:** none (docs + verification only).

- [ ] **Step 1: Write the branch resume doc**

Create `docs/branches/feat-screen-selection.md` recording: goal, the design/plan
paths, key decisions (green outside border via 4 coplanar bars; override
supersedes az/el/dist; gaze-cone 40°), the tunable constants and where they live,
the implementation map (commit per task), and a "next step = hardware pass"
checklist mirroring the two-hand branch doc's format.

- [ ] **Step 2: Full clean build + all tests, captured**

Run:
```bash
cd spatial-screens && make clean && make && make test
cd gestures && python3 -m pytest tests/test_classify.py -v
```
Expected: clean build (no warnings); `gesture_manip_test`, `gesture_parse_test`,
`stereo_math_test` all "all checks passed"; pytest green.

- [ ] **Step 3: Commit**

```bash
git add docs/branches/feat-screen-selection.md docs/specs/2026-07-06-screen-selection-design.md docs/specs/2026-07-06-screen-selection-plan.md
git commit -m "docs(screen-selection): branch resume doc + finalized design/plan"
```

- [ ] **Step 4: Hardware pass (user-driven, on the glasses)**

Not a code step — the glasses verification, run per the memory's hardware-test
division of labor. Confirm, in a multi-screen rack + stereo:
- `two_up` while looking at a screen selects it; a **green border appears
  outside** that screen and the content inside is untouched; both eyes show it.
- One-hand pinch-drag / `[` `]` moves only the active screen's distance; the
  others don't move. `-` `=` resizes only the active screen. Two-hand grab
  repositions/resizes only the active screen.
- `two_up` while looking away from all screens (or re-selecting) deselects →
  gestures return to rack-global. Recenter keeps the selection + layout coherent.

---

## Self-Review (against the design doc)

**Spec coverage:**
- §Goals "discrete select gesture" → Task 1 (`two_up`) + Task 4 (rising-edge arm-gated select).
- §Goals "gaze-center pick" → Task 3 (`pick_gaze_screen`) + Task 4 (wiring).
- §Goals "highlight" → Task 6 (green outside border).
- §Goals "retarget one-hand distance + two-hand grab" → Task 5.
- §Goals "per-screen pose override consumed by scene_screen_pose" → Task 2.
- §Goals "deselect path" → Task 4 (`pick == -1` sets `active_screen = -1`).
- §5 border mechanism + `draws[2][64]→[2][72]` cap → Task 6.
- §Testing (Python two_up + negatives; C++ pick_gaze_screen + override round-trip;
  hardware pass) → Tasks 1, 2, 3, 7.
- §Non-goals honored: no new motion axes (only retarget), no cross-run layout
  persistence (override is in-memory only — the state file is untouched here),
  gaze-only selection (no ray/pointer).

**Placeholder scan:** none — every step carries real code/commands.

**Type consistency:** `world_to_rack_frame(rack_q, rack_p, world_q, world_p, out_ori, out_pos)`
and `pick_gaze_screen(screen_pos, head_p, head_q, cone_deg)` are declared in Task 2/3
and called with the same signatures in Task 4/5. `has_pose_override` / `pose_pos` /
`pose_ori` names match across config.h (Task 2), scene.cpp (Task 2), and main.cpp
(Tasks 4-6). `active_screen` int, `-1` sentinel, and the `order[i].s == &scene[active_screen]`
identity check are consistent across Tasks 4-6.
