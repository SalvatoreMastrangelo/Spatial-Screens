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

    // Midpoint shifts +0.1 in x, -0.1 in y (image space); spread unchanged.
    // world dx = dm.x*gain*distance = 0.1*1*2 = 0.2 along right0 (+x)
    // world dy = -dm.y*gain*distance = -(-0.1)*1*2 = 0.2 along up0 (+y)
    GrabResult r2 = grab_update(g, 0.5f, 0.4f, 0.7f, 0.4f,
                                2.f, 1.f, 20.f, 200.f);
    CHECK(std::fabs(r2.anchor.x - 1.2f) < 1e-4f);
    CHECK(std::fabs(r2.anchor.y - 2.2f) < 1e-4f);
    CHECK(std::fabs(r2.diag - 60.f) < 1e-3f);   // spread unchanged -> size0

    // Clamp: huge spread -> diag capped at max.
    GrabResult r3 = grab_update(g, 0.0f, 0.5f, 1.0f, 0.5f, 2.f, 1.f, 20.f, 200.f);
    CHECK(std::fabs(r3.diag - 200.f) < 1e-3f);

    if (failures == 0) { printf("gesture_manip_test: all checks passed\n"); return 0; }
    printf("gesture_manip_test: %d failure(s)\n", failures);
    return 1;
}
