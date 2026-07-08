// Pure timing/prediction math for vsync-timed pose prediction. No SDK/Vulkan/X
// deps; unit-tested by predict_math_test.cpp. Times in SECONDS unless the name
// says otherwise; the render loop uses a steady_clock seconds timebase.
#pragma once
#include <cmath>

// One-Euro low-pass blend factor for a cutoff (Hz) and sample period dt (s).
// alpha in (0,1): larger dt or larger cutoff -> larger alpha -> less smoothing.
// Identical to the old inline form with Te replaced by the real dt:
//   1 / (1 + 1/(2*pi*cutoff*dt)).
inline float one_euro_alpha(float cutoff_hz, float dt) {
    float tau = 1.f / (2.f * float(M_PI) * cutoff_hz);
    return 1.f / (1.f + tau / dt);
}

// EMA of the inter-vsync interval (seconds). prev_interval <= 0 seeds from the
// first delta. Non-positive deltas are ignored (returns prev unchanged).
inline float vsync_interval_update(float prev_interval, float dt_since_last) {
    if (dt_since_last <= 0.f) return prev_interval;
    if (prev_interval <= 0.f) return dt_since_last;   // seed
    const float a = 0.1f;                             // ~10-sample EMA
    return prev_interval + (dt_since_last - prev_interval) * a;
}

// Seconds to predict a pose sampled at `now` forward to the vblank that scans it
// out, plus a fixed scanout term. Returns -1 (sentinel) when vsync data is
// stale/absent (interval<=0, or `now` far past last_vsync) so the caller falls
// back. A returned 0 is a real "predict to now", never "no data".
inline double compute_predict_s(double now, double last_vsync, double interval,
                                double scanout_s, double cap_s) {
    if (interval <= 0.0) return -1.0;
    if (now - last_vsync > 8.0 * interval) return -1.0;   // callback stalled
    double next = last_vsync;
    if (next <= now) {                                    // advance to next vblank
        double k = std::floor((now - next) / interval) + 1.0;
        next += k * interval;
    }
    double target = (next - now) + scanout_s;
    if (target < 0.0) target = 0.0;
    if (target > cap_s) target = cap_s;
    return target;
}

// Prediction strength [0,1] from smoothed linear speed (m/s) and angular speed
// (deg/frame). 0 below the deadband, ramps to 1. Swim is rotation-dominated, so
// the angular term usually drives it. Takes the max of the two normalized terms.
inline float predict_gate(float lin_speed, float ang_speed,
                          float lin_dead, float lin_ramp,
                          float ang_dead, float ang_ramp) {
    float gl = (lin_speed - lin_dead) / lin_ramp;
    float ga = (ang_speed - ang_dead) / ang_ramp;
    float g = gl > ga ? gl : ga;
    if (g < 0.f) g = 0.f; else if (g > 1.f) g = 1.f;
    return g;
}
