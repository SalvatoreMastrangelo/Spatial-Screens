# Head-Anchored Reorientation on Reposition — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** While the active screen is two-hand-grabbed, weld its orientation to the head's full rotation delta (yaw+pitch+roll) and world-lock it on release — the orientation twin of the grab's existing head-anchored position.

**Architecture:** Add one pure quaternion helper (`head_delta_orient`) to `pose_math.h`, unit-test it, then call it each frame in the active-screen branch of the two-hand grab in `main.cpp`, writing `pose_ori`/`pose_pos` together via the existing `world_to_rack_frame`. No new files, no look-at math, no singularity handling.

**Tech Stack:** C++17, no framework. Custom `CHECK`-macro unit tests built by the `Makefile` (`make stereo-math-test`). Build from `spatial-screens/`.

## Global Constraints

- **Working directory for all commands:** `spatial-screens/`.
- **C++ style:** snake_case functions; header-only inline helpers in `pose_math.h`.
- **Quaternion convention (verified against `scene.cpp:65`):** `qmul(parent, child)` = parent ∘ child; a world-frame rotation `D` applied to orientation `q` is `qmul(D, q)`.
- **Screen facing convention:** the quad faces along its local **−Z** (matches `scene_screen_pose`).
- **Spec of record:** `docs/specs/2026-07-08-head-anchored-reorient-design.md`.
- **Branching:** build on branch `feat/head-anchored-reorient` in an isolated worktree; merge to `main` only after the hardware pass. Never develop on `main`.
- **Hardware verification is manual** (per `CLAUDE.md`): the C++ has no runtime test harness for the gesture loop; Task 2's runtime proof is a glasses-on pass.

---

### Task 1: `head_delta_orient` pure helper + unit tests

**Files:**
- Modify: `spatial-screens/src/pose_math.h` (add helper after `qrot`, before `mat_from_pose` — i.e. after line 38)
- Test: `spatial-screens/src/stereo_math_test.cpp` (add `test_head_delta_orient`, register it in `main()`)

**Interfaces:**
- Consumes: `Quat`, `qmul`, `qconj`, `qrot` (existing, `pose_math.h`); `world_to_rack_frame`, `scene_screen_pose`, `scene_build`, `ScreenCfg`, `MonRect` (existing, `scene.h`/`config.h`).
- Produces: `Quat head_delta_orient(const Quat& start_ori, const Quat& head_start, const Quat& head_now)` — returns `start_ori` re-expressed under the head's world-frame rotation from `head_start` to `head_now`. Consumed by Task 2.

- [ ] **Step 1: Write the failing test**

Add this function to `spatial-screens/src/stereo_math_test.cpp` (e.g. immediately after `test_pose_override`, before `test_pick_gaze_screen`):

```cpp
static void test_head_delta_orient() {
    // Screen's start world orientation and the head's start orientation — both
    // non-trivial so the test would catch a wrong multiply order.
    Quat sq0 = qmul(quat_axis_angle(0, 1, 0, 20.f), quat_axis_angle(1, 0, 0, 10.f));
    Quat hq0 = quat_axis_angle(0, 1, 0, -15.f);

    // Zero head motion -> orientation unchanged (no drift).
    Quat same = head_delta_orient(sq0, hq0, hq0);
    Vec3 f0 = qrot(sq0, {0, 0, -1}), fs = qrot(same, {0, 0, -1});
    Vec3 u0 = qrot(sq0, {0, 1, 0}),  us = qrot(same, {0, 1, 0});
    CHECK(std::fabs(fs.x - f0.x) < 1e-5f && std::fabs(fs.y - f0.y) < 1e-5f &&
          std::fabs(fs.z - f0.z) < 1e-5f);
    CHECK(std::fabs(us.x - u0.x) < 1e-5f && std::fabs(us.y - u0.y) < 1e-5f &&
          std::fabs(us.z - u0.z) < 1e-5f);

    // A head rotation delta D (yaw + roll) applies to the screen in world space:
    // result must equal D * sq0, roll carried through (full delta, not yaw-only).
    Quat D   = qmul(quat_axis_angle(0, 1, 0, 40.f), quat_axis_angle(0, 0, 1, 12.f));
    Quat hq1 = qmul(D, hq0);
    Quat got = head_delta_orient(sq0, hq0, hq1);
    Quat want = qmul(D, sq0);
    Vec3 fg = qrot(got, {0, 0, -1}), fw = qrot(want, {0, 0, -1});
    Vec3 ug = qrot(got, {0, 1, 0}),  uw = qrot(want, {0, 1, 0});
    CHECK(std::fabs(fg.x - fw.x) < 1e-4f && std::fabs(fg.y - fw.y) < 1e-4f &&
          std::fabs(fg.z - fw.z) < 1e-4f);
    CHECK(std::fabs(ug.x - uw.x) < 1e-4f && std::fabs(ug.y - uw.y) < 1e-4f &&
          std::fabs(ug.z - uw.z) < 1e-4f);   // up axis matches -> roll carried through

    // Compose exactly as main.cpp does: the head-delta world orientation, stored
    // rack-relative via world_to_rack_frame, must re-expand through
    // scene_screen_pose to the same world orientation (and position = anchor).
    MonRect fb{"eDP-1", 0, 0, 1920, 1200};
    std::vector<MonRect> mons = {{"VS1", 0, 0, 1920, 1200}};
    std::vector<ScreenCfg> cfg(1); cfg[0].monitor = "VS1";
    auto s = scene_build(cfg, mons, fb);
    Quat rack_q = quat_axis_angle(0, 1, 0, 25.f);
    Vec3 rack_p{0.5f, 1.f, -0.5f};
    Vec3 anchor{0.2f, 1.3f, -1.4f};
    world_to_rack_frame(rack_q, rack_p, got, anchor,
                        s[0].cfg.pose_ori, s[0].cfg.pose_pos);
    s[0].cfg.has_pose_override = true;
    Quat q; Vec3 p;
    scene_screen_pose(s[0], rack_q, rack_p, 1.f, q, p);
    Vec3 fq = qrot(q, {0, 0, -1}), fgot = qrot(got, {0, 0, -1});
    CHECK(std::fabs(fq.x - fgot.x) < 1e-4f && std::fabs(fq.y - fgot.y) < 1e-4f &&
          std::fabs(fq.z - fgot.z) < 1e-4f);
    CHECK(std::fabs(p.x - anchor.x) < 1e-4f && std::fabs(p.y - anchor.y) < 1e-4f &&
          std::fabs(p.z - anchor.z) < 1e-4f);
}
```

Register it by adding one line in `main()` (after `test_pose_override();`, around `stereo_math_test.cpp:242`):

```cpp
    test_head_delta_orient();
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `make stereo-math-test`
Expected: **compile FAILS** with an error like `'head_delta_orient' was not declared in this scope` (the helper doesn't exist yet).

- [ ] **Step 3: Write the minimal implementation**

Add to `spatial-screens/src/pose_math.h`, immediately after the `qrot` function (after line 38, before `mat_from_pose`):

```cpp
// A grabbed screen "welds" to the head: re-express start_ori (a WORLD
// orientation, captured at grab start) under the head's full rotation delta
// from head_start to head_now (yaw+pitch+roll). The orientation twin of the
// head-local position anchor (grab_rel0). Pure quaternion product of the head
// pose and constants — no look-at, no roll lock, no overhead singularity.
inline Quat head_delta_orient(const Quat& start_ori,
                              const Quat& head_start, const Quat& head_now) {
    return qmul(qmul(head_now, qconj(head_start)), start_ori);
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `make stereo-math-test && ./stereo-math-test`
Expected: PASS — final line `stereo_math_test: all checks passed`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add src/pose_math.h src/stereo_math_test.cpp
git commit -m "feat(reorient): head_delta_orient helper + unit tests"
```

---

### Task 2: Wire head-delta orientation into the active-screen grab

**Files:**
- Modify: `spatial-screens/src/main.cpp` — declare two loop-scope locals (~line 679); capture them at grab-start (~lines 819-834); write `pose_ori` each frame in the active-screen grab branch (~lines 850-867)

**Interfaces:**
- Consumes: `head_delta_orient` (Task 1); `world_to_rack_frame` (existing, `scene.h`, already called by the fist-hold recenter at `main.cpp:953`); existing loop locals `grab_rel0`/`grab_scale0` and in-scope `head_rc`, `anchor`, `active_screen`, `rack_q`, `rack_p`, `gr.diag`.
- Produces: no new symbols; runtime behavior change only. Verified by the hardware pass.

- [ ] **Step 1: Declare the two orientation locals beside `grab_rel0`**

In `spatial-screens/src/main.cpp`, the two-hand grab locals are declared before the event loop at lines 678-679:

```cpp
    float grab_scale0 = 1.f;   // rack_size_scale snapshot at grab start (rack mode)
    Vec3 grab_rel0;            // grabbed anchor's offset in the head-local frame at grab start — grab follows head pose
```

Add two lines immediately after `grab_rel0`:

```cpp
    float grab_scale0 = 1.f;   // rack_size_scale snapshot at grab start (rack mode)
    Vec3 grab_rel0;            // grabbed anchor's offset in the head-local frame at grab start — grab follows head pose
    Quat grab_ori0;            // active screen's WORLD orientation at grab start — weld reference (orientation twin of grab_rel0)
    Quat grab_head_q0;         // head orientation (recentered frame) at grab start — the head-delta baseline
```

- [ ] **Step 2: Capture `grab_ori0` at grab-start (active-screen branch)**

At `main.cpp:819-825`, the grab-start block computes the active screen's pose but uses only its position (`sp`); the orientation (`sq`) is discarded. Current code:

```cpp
                if (active_screen >= 0) {
                    Quat sq; Vec3 sp;
                    scene_screen_pose(scene[active_screen], rack_q, rack_p,
                                      rack_dist_scale, sq, sp);
                    anchor0 = sp;
                    size0 = scene[active_screen].cfg.size;
                }
```

Replace with (add the `grab_ori0 = sq;` capture):

```cpp
                if (active_screen >= 0) {
                    Quat sq; Vec3 sp;
                    scene_screen_pose(scene[active_screen], rack_q, rack_p,
                                      rack_dist_scale, sq, sp);
                    anchor0 = sp;
                    size0 = scene[active_screen].cfg.size;
                    grab_ori0 = sq;   // weld reference: screen's world orientation now
                }
```

- [ ] **Step 3: Capture `grab_head_q0` at grab-start**

At `main.cpp:834-837`, the position anchor computes `head_rc0`. Current code:

```cpp
                Quat head_rc0 = qmul(qconj(ori_offset), head_q);
                Vec3 hp0 = qrot(qconj(ori_offset), head_p);
                Vec3 off0 = { anchor0.x - hp0.x, anchor0.y - hp0.y, anchor0.z - hp0.z };
                grab_rel0 = qrot(qconj(head_rc0), off0);
```

Replace with (add the `grab_head_q0 = head_rc0;` capture):

```cpp
                Quat head_rc0 = qmul(qconj(ori_offset), head_q);
                grab_head_q0 = head_rc0;   // head-delta baseline (orientation twin of grab_rel0)
                Vec3 hp0 = qrot(qconj(ori_offset), head_p);
                Vec3 off0 = { anchor0.x - hp0.x, anchor0.y - hp0.y, anchor0.z - hp0.z };
                grab_rel0 = qrot(qconj(head_rc0), off0);
```

- [ ] **Step 4: Write `pose_ori` each frame in the grab-update active branch**

At `main.cpp:850-852`, replace the "orientation held fixed" comment. Current:

```cpp
                    // Orientation is held fixed — the screen follows head POSITION,
                    // not facing. Head-anchoring is gated to the active-screen branch
                    // because the rack origin sits ON the head (d0->0 there).
```

Replace with:

```cpp
                    // Orientation is head-anchored too: the screen welds to the head's
                    // full rotation delta (yaw+pitch+roll) and world-locks on release —
                    // the orientation twin of the head-local position anchor above.
                    // Head-anchoring is gated to the active-screen branch because the
                    // rack origin sits ON the head (d0->0 there).
```

Then at `main.cpp:865-867`, replace the position-only write. Current:

```cpp
                    Vec3 d = { anchor.x - rack_p.x, anchor.y - rack_p.y, anchor.z - rack_p.z };
                    scene[active_screen].cfg.pose_pos = qrot(qconj(rack_q), d);
                    scene[active_screen].cfg.has_pose_override = true;  // already true; explicit
                    scene[active_screen].cfg.size = gr.diag;
```

Replace with (position via `world_to_rack_frame` is identical to the old `d` math; orientation is added):

```cpp
                    Quat world_ori = head_delta_orient(grab_ori0, grab_head_q0, head_rc);
                    world_to_rack_frame(rack_q, rack_p, world_ori, anchor,
                                        scene[active_screen].cfg.pose_ori,
                                        scene[active_screen].cfg.pose_pos);
                    scene[active_screen].cfg.has_pose_override = true;  // already true; explicit
                    scene[active_screen].cfg.size = gr.diag;
```

- [ ] **Step 5: Build the full app to verify it compiles**

Run: `make`
Expected: builds `spatial-screens` with no errors. (`head_delta_orient` from `pose_math.h` and `world_to_rack_frame` from `scene.h` are both already included by `main.cpp`.)

- [ ] **Step 6: Re-run the unit tests (regression)**

Run: `make stereo-math-test && ./stereo-math-test`
Expected: PASS — `stereo_math_test: all checks passed`.

- [ ] **Step 7: Commit**

```bash
git add src/main.cpp
git commit -m "feat(reorient): weld active screen orientation to head during grab"
```

- [ ] **Step 8: Hardware pass (manual, glasses-on)**

Launch per `CLAUDE.md` (`./run.sh`, never with the bridge running). Verify on the glasses:
1. Select a screen (`two_up` gaze pose), two-hand-grab it, and while grabbing **turn/lift your head** to place it to the left / right / overhead. The screen comes with you during the grab and, on release, **world-locks facing the direction you were looking** (overhead → tilted down toward you).
2. A slight head roll during placement leaves the screen rolled (expected — full delta; re-grab to fix). Not a bug.
3. Regression: one-hand pinch-drag distance, fist-hold per-screen recenter, and rack-global grab (nothing selected) behave exactly as before.

Record the result in `docs/branches/feat-head-anchored-reorient.md`.

---

## Self-Review

**Spec coverage** (against `2026-07-08-head-anchored-reorient-design.md`):
- Full head-delta incl. roll, continuous during grab, world-lock on release → Task 2 Step 4 (`head_delta_orient` each frame) + the no-op release (last frame's write persists; `has_pose_override` already true). ✅
- No new helper beyond one pure function; reuse `qmul`/`qconj`/`world_to_rack_frame` → Task 1 helper + Task 2 reuse. ✅
- No drift at zero head rotation → Task 1 Step 1 first CHECK block. ✅
- Untouched paths (one-hand distance, fist-hold recenter, rack-global grab) → not modified; Task 2 Step 8.3 regression check. ✅
- Deferred (nudge hotkeys, elevation clamp) → intentionally absent. ✅

**Placeholder scan:** none — every code and command step is concrete.

**Type consistency:** `head_delta_orient(start_ori, head_start, head_now)` signature is identical in Task 1 (definition) and Task 2 Step 4 (call, `head_delta_orient(grab_ori0, grab_head_q0, head_rc)`). `grab_ori0`/`grab_head_q0` are `Quat`, matching the helper's parameters and the `sq`/`head_rc0` sources. `world_to_rack_frame(rack_q, rack_p, world_q, world_p, out_ori, out_pos)` matches its use in Task 2 Step 4.
