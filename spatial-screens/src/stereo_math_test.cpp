// Standalone unit test for the stereo/multi-screen pure logic: config keys,
// scene construction, per-eye math. No framework — CHECK macro, non-zero on
// failure. Build+run: make stereo-math-test && ./stereo-math-test
#include "config.h"
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
}

int main() {
    test_config_keys();
    if (failures == 0) { printf("stereo_math_test: all checks passed\n"); return 0; }
    printf("stereo_math_test: %d failure(s)\n", failures);
    return 1;
}
