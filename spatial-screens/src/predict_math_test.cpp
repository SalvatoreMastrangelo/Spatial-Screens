// Standalone unit test for predict_math.h — pure timing/prediction math.
// No framework — CHECK macro, non-zero exit on failure.
// Build+run: make predict-math-test && ./predict-math-test
#include "predict_math.h"
#include "pose_math.h"
#include <cmath>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// Two quats are the same rotation if |dot| ~ 1 (accounts for double cover q ~ -q).
static bool quat_close(const Quat& a, const Quat& b, float eps = 1e-4f) {
    float d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    return std::fabs(std::fabs(d) - 1.f) < eps;
}
static float DEG2RAD(float d) { return d * float(M_PI) / 180.f; }

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
    // A legitimate 0 is reachable and distinct from the -1 sentinel: a negative
    // scanout drives target negative, which the lower clamp pins to exactly 0.0.
    CHECK(compute_predict_s(0.005, 0.0, interval, -1.0, cap) == 0.0);
}

static void test_predict_gate() {
    // At rest (both below deadband) -> 0.
    CHECK(predict_gate(0.0f, 0.0f, 0.03f, 0.3f, 2.f, 20.f) == 0.f);
    // Angular well above ramp -> saturates to 1.
    CHECK(predict_gate(0.0f, 100.f, 0.03f, 0.3f, 2.f, 20.f) == 1.f);
    // Monotonic in the middle.
    float mid = predict_gate(0.0f, 12.f, 0.03f, 0.3f, 2.f, 20.f);
    CHECK(mid > 0.f && mid < 1.f);
    // Actually monotonic increasing in angular speed across the ramp.
    CHECK(predict_gate(0.0f, 8.f,  0.03f, 0.3f, 2.f, 20.f) <
          predict_gate(0.0f, 12.f, 0.03f, 0.3f, 2.f, 20.f));
    CHECK(predict_gate(0.0f, 12.f, 0.03f, 0.3f, 2.f, 20.f) <
          predict_gate(0.0f, 16.f, 0.03f, 0.3f, 2.f, 20.f));
}

static void test_quat_delta_rotvec() {
    // Identity -> identity: zero rotation vector.
    Vec3 z = quat_delta_rotvec(Quat{}, Quat{});
    CHECK(std::fabs(z.x) < 1e-6f && std::fabs(z.y) < 1e-6f && std::fabs(z.z) < 1e-6f);
    // Identity -> +10 deg about Y: rotvec ~ (0, 10deg, 0) radians.
    Quat qy = quat_axis_angle(0, 1, 0, 10.f);
    Vec3 rv = quat_delta_rotvec(Quat{}, qy);
    CHECK(std::fabs(rv.x) < 1e-4f && std::fabs(rv.y - DEG2RAD(10.f)) < 1e-4f && std::fabs(rv.z) < 1e-4f);
    // Shortest arc: +350 deg reads as -10 deg (must negate dq when w<0).
    Vec3 rv2 = quat_delta_rotvec(Quat{}, quat_axis_angle(0, 1, 0, 350.f));
    CHECK(std::fabs(rv2.y + DEG2RAD(10.f)) < 1e-4f);
    // World-frame delta from 10deg-Y to 30deg-Y is +20 deg about Y.
    Vec3 rv3 = quat_delta_rotvec(qy, quat_axis_angle(0, 1, 0, 30.f));
    CHECK(std::fabs(rv3.y - DEG2RAD(20.f)) < 1e-4f);
}

static void test_quat_integrate() {
    // Zero angular velocity -> pose unchanged.
    Quat q0 = quat_axis_angle(1, 0, 0, 25.f);
    CHECK(quat_close(quat_integrate(q0, Vec3{0, 0, 0}, 0.011f), q0));
    // From identity, pi/2 rad/s about Y for 1 s -> 90 deg about Y.
    CHECK(quat_close(quat_integrate(Quat{}, Vec3{0, float(M_PI) / 2.f, 0}, 1.0f),
                     quat_axis_angle(0, 1, 0, 90.f)));
    // World-frame pre-multiply: integrating a world-Y rate on top of a 40deg-X pose
    // rotates a vector the same as applying the world-Y delta AFTER the X pose.
    Quat q = quat_axis_angle(1, 0, 0, 40.f);
    Vec3 omega{0, 1.0f, 0}; float h = 0.05f;          // 0.05 rad about world Y
    Quat qi = quat_integrate(q, omega, h);
    Quat dq = quat_axis_angle(0, 1, 0, 0.05f * 180.f / float(M_PI)); // 0.05 rad expressed in deg
    Vec3 v{0, 0, -1};
    Vec3 a = qrot(qi, v), b = qrot(qmul(dq, q), v);
    CHECK(std::fabs(a.x - b.x) < 1e-4f && std::fabs(a.y - b.y) < 1e-4f && std::fabs(a.z - b.z) < 1e-4f);
}

static void test_delta_integrate_roundtrip() {
    // delta_rotvec then integrate over the same dt reconstructs the target pose.
    Quat prev = quat_axis_angle(0, 1, 0, 15.f);
    Quat now = qmul(quat_axis_angle(1, 0, 0, 3.f), quat_axis_angle(0, 1, 0, 23.f));
    float dt = 1.f / 90.f;
    Vec3 rotvec = quat_delta_rotvec(prev, now);
    Vec3 omega{rotvec.x / dt, rotvec.y / dt, rotvec.z / dt};
    CHECK(quat_close(quat_integrate(prev, omega, dt), now, 1e-3f));
}

int main() {
    test_one_euro_alpha();
    test_vsync_interval_update();
    test_compute_predict_s();
    test_predict_gate();
    test_quat_delta_rotvec();
    test_quat_integrate();
    test_delta_integrate_roundtrip();
    if (failures == 0) { printf("predict_math_test: all checks passed\n"); return 0; }
    printf("predict_math_test: %d failure(s)\n", failures);
    return 1;
}
