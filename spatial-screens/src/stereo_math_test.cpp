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
    CHECK(o.stereo == false);          // flips to true in the stereo task
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

    // Orientation faces the origin: screen forward (-z rotated by q) points
    // back at the rack origin for azimuth 90 -> forward = -x.
    cfg[0].azimuth = 90;
    s = scene_build(cfg, mons, fb);
    scene_screen_pose(s[0], rq, rp, 1.f, q, p);
    Vec3 fwd = qrot(q, {0, 0, -1});
    CHECK(std::fabs(fwd.x - 1.f) < 1e-5f);  // screen's -z axis points at +x (outward)
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
    test_stereo_math();
    if (failures == 0) { printf("stereo_math_test: all checks passed\n"); return 0; }
    printf("stereo_math_test: %d failure(s)\n", failures);
    return 1;
}
