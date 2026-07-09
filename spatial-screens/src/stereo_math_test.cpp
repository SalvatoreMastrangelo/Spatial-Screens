// Standalone unit test for the stereo/multi-screen pure logic: config keys,
// scene construction, per-eye math. No framework — CHECK macro, non-zero on
// failure. Build+run: make stereo-math-test && ./stereo-math-test
#include "config.h"
#include "scene.h"
#include "pose_math.h"
#include "stereo.h"
#include <cmath>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void test_config_keys() {
    Options o;
    CHECK(o.stereo == true);           // default: SBS 3D on the glasses
    CHECK(std::fabs(o.ipd_mm - 63.f) < 1e-6f);
    CHECK(o.workspace == "2x2");
    CHECK(o.screens.empty());

    CHECK(set_option(o, "stereo", "true"));
    CHECK(o.stereo == true);
    CHECK(set_option(o, "ipd-mm", "61.5"));
    CHECK(std::fabs(o.ipd_mm - 61.5f) < 1e-6f);
    CHECK(set_option(o, "workspace", "3x1"));
    CHECK(o.workspace == "3x1");

    // screen.N.* creates/fills 1-based slots; sparse set leaves defaults.
    CHECK(set_option(o, "screen.2.monitor", "VS2"));
    CHECK(o.screens.size() == 2);
    CHECK(o.screens[1].monitor == "VS2");
    CHECK(o.screens[0].monitor.empty());
    CHECK(std::fabs(o.screens[1].distance - 0.75f) < 1e-6f);  // default
    CHECK(set_option(o, "screen.1.azimuth", "-35"));
    CHECK(set_option(o, "screen.1.elevation", "10"));
    CHECK(set_option(o, "screen.1.distance", "1.2"));
    CHECK(set_option(o, "screen.1.size", "32"));
    CHECK(std::fabs(o.screens[0].azimuth + 35.f) < 1e-6f);
    CHECK(std::fabs(o.screens[0].elevation - 10.f) < 1e-6f);
    CHECK(std::fabs(o.screens[0].distance - 1.2f) < 1e-6f);
    CHECK(std::fabs(o.screens[0].size - 32.f) < 1e-6f);

    CHECK(!set_option(o, "screen.1.bogus", "1"));   // unknown sub-key
    CHECK(!set_option(o, "screen.0.monitor", "x")); // N is 1-based
    CHECK(!set_option(o, "screen.17.monitor", "x")); // cap 16
    CHECK(!set_option(o, "screen.monitor", "x"));   // missing index

    // Regression: unknown field must not resize o.screens.
    Options o2;
    CHECK(!set_option(o2, "screen.5.bogus", "1"));
    CHECK(o2.screens.empty());
    CHECK(!set_option(o2, "screen.0.monitor", "x"));
    CHECK(o2.screens.empty());
    CHECK(!set_option(o2, "screen.17.monitor", "x"));
    CHECK(o2.screens.empty());

    // Prediction options. Defaults ship the hardware-tuned smoothvel tuple
    // (predict from a de-noised angular velocity; see predict_math.h).
    Options p;
    CHECK(p.predict_mode == "smoothvel");             // default: on, ships the win
    CHECK(std::fabs(p.scanout_ms - 14.f) < 1e-6f);
    CHECK(std::fabs(p.vel_cutoff - 11.f) < 1e-6f);
    CHECK(std::fabs(p.predict_cap_ms - 35.f) < 1e-6f);
    CHECK(set_option(p, "predict-mode", "off"));
    CHECK(p.predict_mode == "off");
    CHECK(set_option(p, "scanout-ms", "8"));
    CHECK(std::fabs(p.scanout_ms - 8.f) < 1e-6f);
    CHECK(set_option(p, "predict-cap-ms", "25"));
    CHECK(std::fabs(p.predict_cap_ms - 25.f) < 1e-6f);
    // predict-ms implies fixed mode (back-compat).
    CHECK(set_option(p, "predict-ms", "20"));
    CHECK(std::fabs(p.predict_ms - 20.f) < 1e-6f);
    CHECK(p.predict_mode == "fixed");
}

static void test_scene_build() {
    // 3840x2400 framebuffer split 2x2 into VS1..VS4 (1920x1200 tiles).
    MonRect fb{"eDP-1", 0, 0, 3840, 2400};
    std::vector<MonRect> mons = {
        {"VS1", 0, 0, 1920, 1200}, {"VS2", 1920, 0, 1920, 1200},
        {"VS3", 0, 1200, 1920, 1200}, {"VS4", 1920, 1200, 1920, 1200}};

    // Empty config -> default rack over the monitors in order:
    // center 0°@0.75m, sides ±35°@1.2m, fourth +25° elevation @1.5m.
    auto scene = scene_build({}, mons, fb);
    CHECK(scene.size() == 4);
    CHECK(scene[0].cfg.monitor == "VS1");
    CHECK(std::fabs(scene[0].cfg.azimuth) < 1e-6f);
    CHECK(std::fabs(scene[0].cfg.distance - 0.75f) < 1e-6f);
    CHECK(std::fabs(scene[1].cfg.azimuth + 35.f) < 1e-6f);   // left
    CHECK(std::fabs(scene[1].cfg.distance - 1.2f) < 1e-6f);
    CHECK(std::fabs(scene[2].cfg.azimuth - 35.f) < 1e-6f);   // right
    CHECK(std::fabs(scene[3].cfg.elevation - 25.f) < 1e-6f); // top
    CHECK(std::fabs(scene[3].cfg.distance - 1.5f) < 1e-6f);

    // UV rects: VS4 is the bottom-right quadrant.
    CHECK(std::fabs(scene[3].uv[0] - 0.5f) < 1e-6f);
    CHECK(std::fabs(scene[3].uv[1] - 0.5f) < 1e-6f);
    CHECK(std::fabs(scene[3].uv[2] - 1.0f) < 1e-6f);
    CHECK(std::fabs(scene[3].uv[3] - 1.0f) < 1e-6f);
    CHECK(std::fabs(scene[0].aspect - 1920.f / 1200.f) < 1e-4f);

    // Explicit config: monitor matched by name; unknown monitor skipped with
    // a warning but the rest of the rack survives.
    std::vector<ScreenCfg> cfg(2);
    cfg[0].monitor = "VS2"; cfg[0].azimuth = 10.f;
    cfg[1].monitor = "NOPE";
    auto scene2 = scene_build(cfg, mons, fb);
    CHECK(scene2.size() == 1);
    CHECK(scene2[0].cfg.monitor == "VS2");
    CHECK(std::fabs(scene2[0].uv[0] - 0.5f) < 1e-6f);  // right half, top row
    CHECK(std::fabs(scene2[0].uv[3] - 0.5f) < 1e-6f);

    // One monitor, no config -> single centered screen (mono-compatible).
    auto scene3 = scene_build({}, {mons[0]}, fb);
    CHECK(scene3.size() == 1);
    CHECK(std::fabs(scene3[0].cfg.azimuth) < 1e-6f);
}

static void test_scene_pose() {
    MonRect fb{"eDP-1", 0, 0, 1920, 1200};
    std::vector<MonRect> mons = {{"VS1", 0, 0, 1920, 1200}};
    std::vector<ScreenCfg> cfg(1);
    cfg[0].monitor = "VS1";

    Quat rq;                       // identity rack
    Vec3 rp{0, 0, 0};
    Quat q; Vec3 p;

    // azimuth 0, distance 1 -> 1m straight ahead (-z).
    cfg[0].azimuth = 0; cfg[0].elevation = 0; cfg[0].distance = 1.f;
    auto s = scene_build(cfg, mons, fb);
    scene_screen_pose(s[0], rq, rp, 1.f, q, p);
    CHECK(std::fabs(p.x) < 1e-5f && std::fabs(p.y) < 1e-5f && std::fabs(p.z + 1.f) < 1e-5f);

    // azimuth +90 -> to the user's RIGHT (+x in EUS).
    cfg[0].azimuth = 90;
    s = scene_build(cfg, mons, fb);
    scene_screen_pose(s[0], rq, rp, 1.f, q, p);
    CHECK(std::fabs(p.x - 1.f) < 1e-5f && std::fabs(p.z) < 1e-5f);

    // elevation +90 -> straight UP (+y).
    cfg[0].azimuth = 0; cfg[0].elevation = 90;
    s = scene_build(cfg, mons, fb);
    scene_screen_pose(s[0], rq, rp, 1.f, q, p);
    CHECK(std::fabs(p.y - 1.f) < 1e-5f && std::fabs(p.z) < 1e-5f);

    // dist_scale multiplies distance; rack translation adds.
    cfg[0].elevation = 0;
    s = scene_build(cfg, mons, fb);
    scene_screen_pose(s[0], rq, {1, 2, 3}, 2.f, q, p);
    CHECK(std::fabs(p.x - 1.f) < 1e-5f && std::fabs(p.y - 2.f) < 1e-5f &&
          std::fabs(p.z - 1.f) < 1e-5f);   // 3 + (-2)

    // Orientation: for azimuth 90 the screen's local -z axis points outward (+x),
    // so its +z face (the rendered front) looks back at the rack origin.
    cfg[0].azimuth = 90;
    s = scene_build(cfg, mons, fb);
    scene_screen_pose(s[0], rq, rp, 1.f, q, p);
    Vec3 fwd = qrot(q, {0, 0, -1});
    CHECK(std::fabs(fwd.x - 1.f) < 1e-5f);  // screen's -z axis points at +x (outward)

    // Compound angle: azimuth 90 AND elevation 45, distance 1.
    // Pitch applied first, then yaw: x ≈ 0.70711, y ≈ 0.70711, z ≈ 0.
    cfg[0].azimuth = 90; cfg[0].elevation = 45; cfg[0].distance = 1.f;
    s = scene_build(cfg, mons, fb);
    scene_screen_pose(s[0], rq, rp, 1.f, q, p);
    CHECK(std::fabs(p.x - 0.70711f) < 1e-5f && std::fabs(p.y - 0.70711f) < 1e-5f &&
          std::fabs(p.z) < 1e-5f);
}

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

static void test_stereo_math() {
    // Camera shifted +x (right eye) moves world points -x in view space:
    // right eye gets a NEGATIVE view-space offset, left eye positive.
    CHECK(std::fabs(stereo_eye_offset(0.063f, 0) - 0.0315f) < 1e-6f);
    CHECK(std::fabs(stereo_eye_offset(0.063f, 1) + 0.0315f) < 1e-6f);

    // Per-eye frustum: same 52° diagonal FOV math main.cpp uses, with per-eye
    // pixel dims. 1920x1200 @ near 0.1: diag=2264.7, half=tan(26°)=0.48773.
    float r = 0, t = 0;
    stereo_eye_frustum(1920, 1200, 52.f, 0.1f, r, t);
    CHECK(std::fabs(r - 0.48773f * 1920.f / 2264.7f * 0.1f) < 1e-4f);
    CHECK(std::fabs(t - 0.48773f * 1200.f / 2264.7f * 0.1f) < 1e-4f);
    CHECK(r > t);                        // landscape
    // Same aspect at mono full-width would double r; per-eye must NOT.
    float rf = 0, tf = 0;
    stereo_eye_frustum(3840, 1200, 52.f, 0.1f, rf, tf);
    CHECK(rf > r * 1.1f);                // sanity: full-width frustum is wider
}

int main() {
    test_config_keys();
    test_scene_build();
    test_scene_pose();
    test_pose_override();
    test_head_delta_orient();
    test_pick_gaze_screen();
    test_stereo_math();
    if (failures == 0) { printf("stereo_math_test: all checks passed\n"); return 0; }
    printf("stereo_math_test: %d failure(s)\n", failures);
    return 1;
}
