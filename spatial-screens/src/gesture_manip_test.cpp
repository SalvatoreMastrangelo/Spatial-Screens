// Standalone unit test for the pure two-hand grab math. Build+run with
//   make gesture-manip-test && ./gesture-manip-test
#include "gesture_manip.h"
#include <cmath>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main() {
    GVec3 anchor0{1.f, 2.f, 3.f};
    GVec3 right0{1.f, 0.f, 0.f};
    GVec3 up0{0.f, 1.f, 0.f};
    // Pinches 0.2 apart horizontally, midpoint (0.5,0.5), size0 = 60 in.
    GrabState g = grab_begin(0.4f, 0.5f, 0.6f, 0.5f, 60.f, anchor0, right0, up0);
    CHECK(g.active);
    CHECK(std::fabs(g.spread0 - 0.2f) < 1e-4f);

    // Spread doubled (0.4), midpoint unchanged -> diag doubles, anchor unchanged.
    GrabResult r1 = grab_update(g, 0.3f, 0.5f, 0.7f, 0.5f,
                                /*distance*/2.f, /*gain*/1.f, /*min*/20.f, /*max*/200.f);
    CHECK(std::fabs(r1.diag - 120.f) < 1e-3f);
    CHECK(std::fabs(r1.anchor.x - 1.f) < 1e-4f);
    CHECK(std::fabs(r1.anchor.y - 2.f) < 1e-4f);

    // Midpoint shifts +0.1 in x, -0.2 in y (image space); spread unchanged (0.2).
    // world dx = dm.x*gain*distance = 0.1*1*2 = 0.2 along right0 (+x)
    // world dy = -dm.y*gain*distance = -(-0.2)*1*2 = 0.4 along up0 (+y)
    // wx (0.2) != wy (0.4) so a right0/up0 transposition would change the result.
    GrabResult r2 = grab_update(g, 0.5f, 0.3f, 0.7f, 0.3f,
                                2.f, 1.f, 20.f, 200.f);
    CHECK(std::fabs(r2.anchor.x - 1.2f) < 1e-4f);
    CHECK(std::fabs(r2.anchor.y - 2.4f) < 1e-4f);
    CHECK(std::fabs(r2.diag - 60.f) < 1e-3f);   // spread unchanged -> size0

    // Clamp: huge spread -> diag capped at max.
    GrabResult r3 = grab_update(g, 0.0f, 0.5f, 1.0f, 0.5f, 2.f, 1.f, 20.f, 200.f);
    CHECK(std::fabs(r3.diag - 200.f) < 1e-3f);

    // Clamp: tiny spread -> diag capped at min.
    GrabResult r4 = grab_update(g, 0.49f, 0.5f, 0.51f, 0.5f, 2.f, 1.f, 20.f, 200.f);
    CHECK(std::fabs(r4.diag - 20.f) < 1e-3f);   // 60*0.02/0.2 = 6 -> clamped to 20

    // z-axis coverage: right0 points along world +z, so an x-midpoint shift
    // must move the anchor in z (exercises the anchor.z term).
    GVec3 anchorZ{1.f, 2.f, 3.f};
    GVec3 right0z{0.f, 0.f, 1.f};
    GVec3 up0z{0.f, 1.f, 0.f};
    GrabState gz = grab_begin(0.4f, 0.5f, 0.6f, 0.5f, 60.f, anchorZ, right0z, up0z);
    // midpoint +0.1 in x, spread unchanged: wx = 0.1*1*2 = 0.2 along right0z (+z)
    GrabResult rz = grab_update(gz, 0.5f, 0.5f, 0.7f, 0.5f, 2.f, 1.f, 20.f, 200.f);
    CHECK(std::fabs(rz.anchor.z - 3.2f) < 1e-4f);   // 3 + 1*0.2
    CHECK(std::fabs(rz.anchor.x - 1.0f) < 1e-4f);   // right0z.x = up0z.x = 0
    CHECK(std::fabs(rz.anchor.y - 2.0f) < 1e-4f);

    // Degenerate grab start (both pinch points coincident): spread0 is floored
    // so grab_update stays finite (no div-by-zero).
    GrabState gd = grab_begin(0.5f, 0.5f, 0.5f, 0.5f, 60.f, anchorZ, right0z, up0z);
    CHECK(gd.spread0 > 0.f);
    GrabResult rd = grab_update(gd, 0.4f, 0.5f, 0.6f, 0.5f, 2.f, 1.f, 20.f, 200.f);
    CHECK(rd.diag == rd.diag);   // not NaN

    if (failures == 0) { printf("gesture_manip_test: all checks passed\n"); return 0; }
    printf("gesture_manip_test: %d failure(s)\n", failures);
    return 1;
}
