// Standalone unit test for predict_math.h — pure timing/prediction math.
// No framework — CHECK macro, non-zero exit on failure.
// Build+run: make predict-math-test && ./predict-math-test
#include "predict_math.h"
#include <cmath>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void test_one_euro_alpha() {
    // Closed form: 1/(1 + 1/(2*pi*cutoff*dt)).
    float dt = 1.f / 90.f;
    float a1 = one_euro_alpha(1.f, dt);
    float expect1 = 1.f / (1.f + 1.f / (2.f * float(M_PI) * 1.f * dt));
    CHECK(std::fabs(a1 - expect1) < 1e-6f);
    // Monotonic in dt at fixed cutoff: bigger dt -> bigger alpha (less smoothing).
    CHECK(one_euro_alpha(1.f, 1.f / 30.f) > one_euro_alpha(1.f, 1.f / 120.f));
    // In (0,1).
    CHECK(a1 > 0.f && a1 < 1.f);
}

static void test_vsync_interval_update() {
    // Seeds from first delta.
    float iv = vsync_interval_update(0.f, 0.011f);
    CHECK(std::fabs(iv - 0.011f) < 1e-6f);
    // Steady 90 Hz stream converges toward ~11.11 ms.
    for (int i = 0; i < 500; i++) iv = vsync_interval_update(iv, 1.f / 90.f);
    CHECK(std::fabs(iv - 1.f / 90.f) < 1e-4f);
    // Non-positive delta leaves it unchanged (no NaN).
    float held = vsync_interval_update(iv, -0.5f);
    CHECK(std::fabs(held - iv) < 1e-9f);
}

static void test_compute_predict_s() {
    double interval = 1.0 / 90.0;      // ~0.011111
    double scan = 0.005, cap = 0.035;
    // now mid-interval -> (next_vblank - now) + scanout.
    double p = compute_predict_s(0.005, 0.0, interval, scan, cap);
    double next = interval;            // first vblank after 0.005
    CHECK(std::fabs(p - ((next - 0.005) + scan)) < 1e-6);
    // Large scanout clamps at cap.
    CHECK(std::fabs(compute_predict_s(0.0, 0.0, interval, 0.05, cap) - cap) < 1e-9);
    // Stale callback (now far past last_vsync) -> negative sentinel.
    CHECK(compute_predict_s(1.0, 0.0, interval, scan, cap) < 0.0);
    // No interval -> sentinel.
    CHECK(compute_predict_s(0.005, 0.0, 0.0, scan, cap) < 0.0);
    // A legitimate 0 is possible and is NOT the sentinel: now exactly at a vblank
    // with zero scanout predicts to the next whole interval, so use scan=0 and
    // now just before a vblank to get a tiny positive — assert non-negative here.
    CHECK(compute_predict_s(0.005, 0.0, interval, 0.0, cap) >= 0.0);
}

static void test_predict_gate() {
    // At rest (both below deadband) -> 0.
    CHECK(predict_gate(0.0f, 0.0f, 0.03f, 0.3f, 2.f, 20.f) == 0.f);
    // Angular well above ramp -> saturates to 1.
    CHECK(predict_gate(0.0f, 100.f, 0.03f, 0.3f, 2.f, 20.f) == 1.f);
    // Monotonic in the middle.
    float mid = predict_gate(0.0f, 12.f, 0.03f, 0.3f, 2.f, 20.f);
    CHECK(mid > 0.f && mid < 1.f);
}

int main() {
    test_one_euro_alpha();
    test_vsync_interval_update();
    test_compute_predict_s();
    test_predict_gate();
    if (failures == 0) { printf("predict_math_test: all checks passed\n"); return 0; }
    printf("predict_math_test: %d failure(s)\n", failures);
    return 1;
}
