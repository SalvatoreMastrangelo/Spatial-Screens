# VSync-Timed Pose Prediction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reduce motion swim of world-anchored screens by predicting the head pose to the next display scanout (timed off the SDK VSync callback), late-sampling the pose, and making the One-Euro filter use real per-frame dt.

**Architecture:** A new header-only pure-math unit (`predict_math.h`) holds all timing/prediction math (unit-tested with no SDK/Vulkan deps). `main.cpp` wires the SDK's currently-unused `XRVSyncCallback` to an atomic vsync-clock publisher, relocates the pose sample to just before the view-matrix build, dt-corrects the existing One-Euro filter, and computes a speed-gated prediction horizon that it passes to `get_gl_pose_carina`. Config gains `predict-mode`/`scanout-ms`/`predict-cap-ms`. Everything is reversible via `--predict-mode off`, and a final on-glasses tuning campaign dials the constants.

**Tech Stack:** C++17, GNU make, VITURE Carina SDK (`get_gl_pose_carina`, `register_callbacks_carina`), Vulkan direct-mode (unchanged), no new dependencies.

## Global Constraints

- **Platform:** Linux x86_64 only; C++17; **no new third-party dependencies**.
- **Workspace:** all work stays in the worktree `.claude/worktrees/vsync-pose-predict` on branch `feat/vsync-pose-predict`. **Never `cd` out of the worktree; never commit to `main`.** Verify `git branch --show-current` is `feat/vsync-pose-predict` before every commit.
- **Build/test:** pure-logic tests build without the SDK/Vulkan — `cd spatial-screens && make test`. The full app (`make`) needs `../sdk`. Compiler flags are the Makefile's existing `CXXFLAGS` (`-O2 -g -std=c++17 -Wall -Wextra`); **no new warnings**.
- **`--predict-mode off` MUST reproduce today's behavior exactly** apart from the dt fix — it is the hardware A/B control.
- **Single-client SDK:** never launch on hardware while `viture-bridge` or another `spatial-screens` holds the device.
- **Coordinates unchanged:** 6DoF poses stay OpenGL/EUS (x right, y up, z backward); this feature only changes *when/with-what-prediction* the head pose is sampled.
- **Commit trailer:** every commit message ends with exactly:
  ```
  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01Ba5BAYxZtu1Zd48Ch4Vcnm
  ```
  Below, commit steps show the subject line only; append this trailer to each.

---

### Task 1: Pure timing/prediction math unit

**Files:**
- Create: `spatial-screens/src/predict_math.h`
- Test: `spatial-screens/src/predict_math_test.cpp`
- Modify: `spatial-screens/Makefile` (new `predict-math-test` target; add to `test` and `clean`)

**Interfaces:**
- Produces (all pure, header-only):
  - `float one_euro_alpha(float cutoff_hz, float dt)` — One-Euro blend factor.
  - `float vsync_interval_update(float prev_interval, float dt_since_last)` — EMA of the inter-vsync interval (seconds).
  - `double compute_predict_s(double now, double last_vsync, double interval, double scanout_s, double cap_s)` — prediction horizon (seconds) to the next vblank + scanout; returns a **negative sentinel** when vsync data is stale/absent.
  - `float predict_gate(float lin_speed, float ang_speed, float lin_dead, float lin_ramp, float ang_dead, float ang_ramp)` — prediction strength in `[0,1]`.

- [ ] **Step 1: Write `predict_math.h`**

Create `spatial-screens/src/predict_math.h`:

```cpp
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
```

- [ ] **Step 2: Write the failing test**

Create `spatial-screens/src/predict_math_test.cpp`:

```cpp
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
```

- [ ] **Step 3: Add the Makefile target**

In `spatial-screens/Makefile`, after the `stereo-math-test` block (around line 58), add:

```make
src/predict_math_test.o: src/predict_math_test.cpp src/predict_math.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Pure timing/prediction math test — header-only unit, no SDK/Vulkan/X deps.
predict-math-test: src/predict_math_test.o
	$(CXX) $^ -o $@
```

Change the `test` target (lines 60-63) to include it:

```make
test: gesture-parse-test gesture-manip-test stereo-math-test predict-math-test
	./gesture-parse-test
	./gesture-manip-test
	./stereo-math-test
	./predict-math-test
```

Add `predict-math-test` to the `clean` rule's `rm -f` list (line 114).

- [ ] **Step 4: Run the test — verify it passes**

Run: `cd spatial-screens && make predict-math-test && ./predict-math-test`
Expected: `predict_math_test: all checks passed`

- [ ] **Step 5: Confirm the whole suite still passes**

Run: `make test`
Expected: all four `*_test: all checks passed` lines, exit 0.

- [ ] **Step 6: Commit**

```bash
git add spatial-screens/src/predict_math.h spatial-screens/src/predict_math_test.cpp spatial-screens/Makefile
git commit -m "feat(predict): pure vsync/predict/one-euro math unit + tests"
```

---

### Task 2: Config options for prediction

**Files:**
- Modify: `spatial-screens/src/config.h:34-36` (Options struct)
- Modify: `spatial-screens/src/config.cpp:129` (set_option)
- Test: `spatial-screens/src/stereo_math_test.cpp` (extend `test_config_keys`)

**Interfaces:**
- Produces: `Options.predict_mode` (`std::string`, default `"vsync"`), `Options.scanout_ms` (`float`, default `5.f`), `Options.predict_cap_ms` (`float`, default `35.f`). Config keys `predict-mode`, `scanout-ms`, `predict-cap-ms`; key `predict-ms N` additionally sets `predict_mode = "fixed"`.
- Consumes: existing `parse_float` / `set_option` machinery in `config.cpp`.

- [ ] **Step 1: Write the failing test**

In `spatial-screens/src/stereo_math_test.cpp`, inside `test_config_keys()` (after the existing assertions, before its closing brace), add:

```cpp
    // Prediction options (vsync-timed pose prediction).
    Options p;
    CHECK(p.predict_mode == "vsync");                 // default: on
    CHECK(std::fabs(p.scanout_ms - 5.f) < 1e-6f);
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
```

- [ ] **Step 2: Run the test — verify it FAILS to compile/assert**

Run: `cd spatial-screens && make stereo-math-test`
Expected: compile error (`predict_mode` is not a member of `Options`).

- [ ] **Step 3: Add the Options fields**

In `spatial-screens/src/config.h`, right after `float predict_ms = 0.f;` (line 34), add:

```cpp
    std::string predict_mode = "vsync";   // off|fixed|vsync (head-pose prediction)
    float scanout_ms = 5.f;               // extra sample-to-photon term (vsync mode)
    float predict_cap_ms = 35.f;          // clamp on the prediction horizon
```

- [ ] **Step 4: Parse the new keys**

In `spatial-screens/src/config.cpp`, replace the `predict-ms` line (line 129):

```cpp
    else if (k == "predict-ms") parse_float(k, v, o.predict_ms);
```

with:

```cpp
    else if (k == "predict-ms") { parse_float(k, v, o.predict_ms); o.predict_mode = "fixed"; }
    else if (k == "predict-mode") o.predict_mode = v;
    else if (k == "scanout-ms") parse_float(k, v, o.scanout_ms);
    else if (k == "predict-cap-ms") parse_float(k, v, o.predict_cap_ms);
```

- [ ] **Step 5: Surface the keys in usage + `--dump-config`**

In `spatial-screens/src/main.cpp`, extend the usage string (the `printf("usage: ...")` around line 304-315) to mention `[--predict-mode off|fixed|vsync] [--scanout-ms MS] [--predict-cap-ms MS]`. In the `dump_config` block (around line 325-328), add the three keys to the effective-options dump, e.g. after the `predict-ms` line:

```cpp
        printf("predict-mode = %s\nscanout-ms = %.2f\npredict-cap-ms = %.2f\n",
               o.predict_mode.c_str(), o.scanout_ms, o.predict_cap_ms);
```

- [ ] **Step 6: Run the test — verify it passes; build**

Run: `make stereo-math-test && ./stereo-math-test && make`
Expected: `stereo_math_test: all checks passed`, then a clean app build.

- [ ] **Step 7: Commit**

```bash
git add spatial-screens/src/config.h spatial-screens/src/config.cpp spatial-screens/src/stereo_math_test.cpp spatial-screens/src/main.cpp
git commit -m "feat(predict): predict-mode/scanout-ms/predict-cap-ms config options"
```

---

### Task 3: dt-correct the One-Euro filter

**Files:**
- Modify: `spatial-screens/src/main.cpp:1035-1071` (the `else` branch of the pose block) and add a file-scope monotonic-clock helper + `last_pose_now` state.
- Modify: `spatial-screens/src/main.cpp` includes (add `#include "predict_math.h"` near `#include "pose_math.h"`, line 54).

**Interfaces:**
- Consumes: `one_euro_alpha` (Task 1).
- Produces: `mono_now_s()` file-scope helper (also used by Task 4); a `dt` value computed at the pose sample (also used by Task 6).

**Note:** this task keeps prediction at 0 — it is a standalone correctness/quality fix (real dt instead of hardcoded `Te=1/120`). `--predict-mode off` behavior is unaffected except that the filter now tracks the true frame period.

- [ ] **Step 1: Add the include and a file-scope monotonic clock**

In `spatial-screens/src/main.cpp`, add near line 54 (beside `#include "pose_math.h"`):

```cpp
#include "predict_math.h"
```

Add a file-scope helper (near the other `static` helpers around line 108-111):

```cpp
static double mono_now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
```

- [ ] **Step 2: Compute dt at the pose sample and feed the filter**

Locate the pose block at `main.cpp:1024-1073`. Immediately before `float pose[7] = {0};` (line 1025), add the dt computation:

```cpp
        // Real per-frame sample period for the One-Euro filter (and, later,
        // prediction). Clamped so a stall or a burst can't destabilize alpha.
        static double last_pose_now = -1.0;
        double pose_now = mono_now_s();
        float dt = (last_pose_now < 0.0)
                 ? (1.f / 90.f)
                 : std::clamp(float(pose_now - last_pose_now), 1.f / 240.f, 1.f / 30.f);
        last_pose_now = pose_now;
```

In the `else` branch (currently lines 1040-1052), replace:

```cpp
                static float speed_hat = 0;
                const float Te = 1.f / 120.f;
                float dx = rp.x - head_p.x, dy = rp.y - head_p.y, dz = rp.z - head_p.z;
                float speed = std::sqrt(dx * dx + dy * dy + dz * dz) / Te; // m/s
                const float d_cutoff = 1.8f; // Hz — how fast the speed estimate reacts
                float ad = 1.f / (1.f + 1.f / (2.f * float(M_PI) * d_cutoff * Te));
                speed_hat += (speed - speed_hat) * ad;
                float min_cutoff = smooth_pos * 4.f; // default 0.10 → 0.4 Hz at rest
                // Deadband removes the VIO noise floor from the speed signal,
                // allowing a strong motion gain without rest wiggle.
                float speed_eff = std::max(0.f, speed_hat - 0.03f);
                float cutoff = min_cutoff + 9.f * speed_eff;
                float ap = 1.f / (1.f + 1.f / (2.f * float(M_PI) * cutoff * Te));
```

with (using the real `dt` and the extracted alpha helper):

```cpp
                static float speed_hat = 0;
                float dx = rp.x - head_p.x, dy = rp.y - head_p.y, dz = rp.z - head_p.z;
                float speed = std::sqrt(dx * dx + dy * dy + dz * dz) / dt; // m/s
                const float d_cutoff = 1.8f; // Hz — how fast the speed estimate reacts
                float ad = one_euro_alpha(d_cutoff, dt);
                speed_hat += (speed - speed_hat) * ad;
                float min_cutoff = smooth_pos * 4.f; // default 0.10 → 0.4 Hz at rest
                // Deadband removes the VIO noise floor from the speed signal,
                // allowing a strong motion gain without rest wiggle.
                float speed_eff = std::max(0.f, speed_hat - 0.03f);
                float cutoff = min_cutoff + 9.f * speed_eff;
                float ap = one_euro_alpha(cutoff, dt);
```

(The orientation nlerp `a` at line 1064 is a velocity-scaled blend, not a time-constant filter, so it is left unchanged in this task.)

- [ ] **Step 3: Build the app**

Run: `cd spatial-screens && make`
Expected: clean build, no new warnings, produces `spatial-screens`.

- [ ] **Step 4: Confirm pure tests still pass**

Run: `make test`
Expected: all four `*_test: all checks passed`.

- [ ] **Step 5: Commit**

```bash
git add spatial-screens/src/main.cpp
git commit -m "fix(predict): One-Euro filter uses real per-frame dt, not Te=1/120"
```

---

### Task 4: VSync clock + status-line instrumentation

**Files:**
- Modify: `spatial-screens/src/main.cpp` — `on_vsync` callback + atomics (near line 108-111 and the `register_callbacks_carina` call at line 183); status-line print (lines 1413-1428).

**Interfaces:**
- Consumes: `vsync_interval_update` (Task 1), `mono_now_s` (Task 3).
- Produces: `g_vsync_last` / `g_vsync_interval` atomics (seconds), read by Task 6; a `dt_ring` + `dt` stats feeding both the status line and Task 6's fallback.

**Note:** this task only *observes* — it registers the vsync callback and prints the measurement line. Prediction is still 0. This is the "measure first" step and confirms whether vsync fires under a leased display.

- [ ] **Step 1: Add the vsync atomics and callback**

In `spatial-screens/src/main.cpp`, near the other file-scope atomics (around line 100, beside `g_cam_w`), add:

```cpp
static std::atomic<double> g_vsync_last{0.0};      // steady_clock secs of last vsync
static std::atomic<double> g_vsync_interval{0.0};  // EMA of inter-vsync seconds
```

Add the callback (near `on_imu_noop` / `on_pose_noop`, line 110-111):

```cpp
static void on_vsync(double /*sdk_ts*/) {
    // Stamp with OUR monotonic clock, not the SDK timestamp: the render loop
    // compares against mono_now_s(), so both must share one timebase.
    double now = mono_now_s();
    double prev = g_vsync_last.load(std::memory_order_relaxed);
    if (prev > 0.0) {
        float iv = vsync_interval_update(g_vsync_interval.load(std::memory_order_relaxed),
                                         float(now - prev));
        g_vsync_interval.store(iv, std::memory_order_relaxed);
    }
    g_vsync_last.store(now, std::memory_order_relaxed);
}
```

- [ ] **Step 2: Register the callback**

At `main.cpp:183`, replace the `nullptr` (the vsync slot — 2nd callback arg) with `on_vsync`:

```cpp
    if (register_callbacks_carina(g_provider, on_pose_noop, on_vsync, on_imu_noop, on_camera_carina) != 0 ||
```

- [ ] **Step 3: Track frame-dt stats**

Just before the render loop `while (g_running)` (line 709), add a small ring buffer:

```cpp
    static float dt_ring[256]; int dt_ring_n = 0, dt_ring_pos = 0;
    float dt_mean_ema = 1.f / 90.f;
```

Inside the pose block, right after `last_pose_now = pose_now;` (added in Task 3), record the dt:

```cpp
        dt_ring[dt_ring_pos] = dt; dt_ring_pos = (dt_ring_pos + 1) & 255;
        if (dt_ring_n < 256) dt_ring_n++;
        dt_mean_ema += (dt - dt_mean_ema) * 0.02f;
```

- [ ] **Step 4: Print the measurement line**

In the 2 s status-line block (after the existing `printf(...)` that ends at line 1428, i.e. right after `last_fps_t = tnow;` is NOT yet reached — insert before `last_fps_t = tnow;` at line 1428), add:

```cpp
            // Prediction/latency diagnostics: frame-dt mean/p95/max, vsync clock,
            // and the live prediction horizon (0 until Task 6 enables it).
            float dmean = 0, dmax = 0;
            for (int i = 0; i < dt_ring_n; i++) { dmean += dt_ring[i]; if (dt_ring[i] > dmax) dmax = dt_ring[i]; }
            if (dt_ring_n) dmean /= dt_ring_n;
            float dsorted[256];
            for (int i = 0; i < dt_ring_n; i++) dsorted[i] = dt_ring[i];
            std::sort(dsorted, dsorted + dt_ring_n);
            float dp95 = dt_ring_n ? dsorted[(dt_ring_n * 95) / 100] : 0.f;
            double vs_int = g_vsync_interval.load(std::memory_order_relaxed);
            double vs_age = mono_now_s() - g_vsync_last.load(std::memory_order_relaxed);
            bool vs_ok = vs_int > 0.0 && vs_age < 8.0 * vs_int;
            printf("  predict %.1fms  dt %.1f/%.1f/%.1f ms(mean/p95/max)  vsync %s %.2fms\n",
                   g_last_predict_ms, dmean * 1e3f, dp95 * 1e3f, dmax * 1e3f,
                   vs_ok ? "ok" : "ABSENT", vs_int * 1e3);
```

Add a file-scope `static float g_last_predict_ms = 0.f;` near the vsync atomics (Step 1) — Task 6 will write it; here it stays 0.

- [ ] **Step 5: Build and smoke-run without hardware**

Run: `cd spatial-screens && make`
Expected: clean build. (Behavioral vsync check happens in Task 7 on hardware.)

- [ ] **Step 6: Commit**

```bash
git add spatial-screens/src/main.cpp
git commit -m "feat(predict): wire VSync callback + latency/dt status-line instrumentation"
```

---

### Task 5: Late-sample the pose (relocate the pose block)

**Files:**
- Modify: `spatial-screens/src/main.cpp` — move the pose-update block (sample + One-Euro filter + telemetry `send_pose` + 6DoF liveness, currently lines 1024-1097) to just before the render block (line 1151), after the capture tick (ends line 1149).

**Interfaces:** none new. Pure code motion.

**Note:** prediction is still 0, so this is a behavior-preserving reorder that shrinks the fixed CPU portion of sample-to-photon latency. Isolating it here lets a reviewer approve the move separately from the prediction logic.

- [ ] **Step 1: Move the block**

Cut the entire pose-update region — from the `// ---- pose (predicted, then smoothed)` comment and the dt computation (added in Task 3) at ~line 1024, through the end of the 6DoF-liveness block at line 1097 — and paste it immediately after the capture tick block (after line 1149) and immediately before the `// ---- render` comment at line 1151.

Verify the resulting per-frame order is: gestures → capture tick → **pose sample + filter + telemetry + liveness** → render/view-build → draw/present. Gestures continue to read the previous frame's `head_q`/`head_p` (unchanged invariant). Capture does not read the head pose.

- [ ] **Step 2: Build**

Run: `cd spatial-screens && make`
Expected: clean build, no new warnings.

- [ ] **Step 3: Pure tests still green**

Run: `make test`
Expected: all four `*_test: all checks passed`.

- [ ] **Step 4: Commit**

```bash
git add spatial-screens/src/main.cpp
git commit -m "refactor(predict): late-sample the head pose just before view build"
```

---

### Task 6: Speed-gated vsync prediction

**Files:**
- Modify: `spatial-screens/src/main.cpp` — compute the gated prediction horizon and pass it to `get_gl_pose_carina`; honor `predict_mode`/`scanout_ms`/`predict_cap_ms`; add angular-speed EMA; write `g_last_predict_ms`.

**Interfaces:**
- Consumes: `compute_predict_s`, `predict_gate` (Task 1); `g_vsync_last`/`g_vsync_interval`, `dt_mean_ema`, `g_last_predict_ms` (Task 4); `predict_mode`/`scanout_ms`/`predict_cap_ms` locals (from `Options`, aliased near `main.cpp:339-340`).

- [ ] **Step 1: Alias the new options as loop locals**

Near `main.cpp:339-340` where `predict_ms`/`smooth_pos`/`smooth_ori` are pulled from `Options`, add:

```cpp
    std::string predict_mode = o.predict_mode;
    float scanout_ms = o.scanout_ms, predict_cap_ms = o.predict_cap_ms;
```

- [ ] **Step 2: Persist speeds across frames for the gate**

The gate must be evaluated *before* sampling (prediction is an input to `get_gl_pose_carina`), so it uses the previous frame's smoothed speeds. Hoist the two speed estimators to a scope visible before the sample. Change `static float speed_hat = 0;` inside the filter (Task 3) to a loop-visible declaration by adding, just before the `while (g_running)` loop (line 709):

```cpp
    float speed_hat = 0.f;       // smoothed linear speed (m/s), One-Euro
    float ang_speed_hat = 0.f;   // smoothed angular speed (deg/frame)
```

Then in the filter body (Task 3 edit), delete the local `static float speed_hat = 0;` so it refers to this outer one.

In the orientation nlerp section (around `main.cpp:1061-1064`), after `ang` is computed, add:

```cpp
                ang_speed_hat += (ang - ang_speed_hat) * 0.3f;   // deg/frame EMA
```

- [ ] **Step 3: Compute the gated prediction horizon before sampling**

Replace the pose sample call. Currently (after Task 5 relocation) it reads:

```cpp
        float pose[7] = {0};
        if (get_gl_pose_carina(g_provider, pose, double(predict_ms) * 1e6) == 0) {
```

Change to compute `predict_s` first, then pass it (converted to nanoseconds):

```cpp
        // Prediction horizon (seconds), speed-gated so at rest it decays to ~0
        // (the failure mode of the earlier fixed --predict-ms). vsync mode times
        // it to the next scanout; fixed uses the configured predict-ms; off = 0.
        float gate = predict_gate(speed_hat, ang_speed_hat,
                                  /*lin_dead*/0.03f, /*lin_ramp*/0.30f,
                                  /*ang_dead*/2.0f,  /*ang_ramp*/20.0f);
        double predict_s = 0.0;
        if (predict_mode != "off") {
            double cap_s = predict_cap_ms * 1e-3;
            if (predict_mode == "vsync") {
                double p = compute_predict_s(pose_now,
                                             g_vsync_last.load(std::memory_order_relaxed),
                                             g_vsync_interval.load(std::memory_order_relaxed),
                                             scanout_ms * 1e-3, cap_s);
                predict_s = (p >= 0.0) ? p
                          : std::min(double(dt_mean_ema) + scanout_ms * 1e-3, cap_s); // fallback
            } else { // "fixed"
                predict_s = std::min(double(predict_ms) * 1e-3, cap_s);
            }
            predict_s *= gate;
        }
        g_last_predict_ms = float(predict_s * 1e3);

        float pose[7] = {0};
        if (get_gl_pose_carina(g_provider, pose, predict_s * 1e9) == 0) {
```

(`get_gl_pose_carina`'s prediction argument is nanoseconds — `predict_s * 1e9`. The old code multiplied `predict_ms` by `1e6`; same units, now driven by the gated horizon.)

- [ ] **Step 4: Build**

Run: `cd spatial-screens && make`
Expected: clean build, no new warnings.

- [ ] **Step 5: Pure tests still green**

Run: `make test`
Expected: all four `*_test: all checks passed`.

- [ ] **Step 6: Verify the off-path is inert (static check)**

Read the diff and confirm: with `predict_mode == "off"`, `predict_s` stays `0.0`, so `get_gl_pose_carina(..., 0)` — byte-for-byte today's call. Confirm `g_last_predict_ms` reads `0.0` in that mode.

- [ ] **Step 7: Commit**

```bash
git add spatial-screens/src/main.cpp
git commit -m "feat(predict): speed-gated vsync-timed pose prediction with fixed fallback"
```

---

### Task 7: On-glasses tuning campaign

**Goal:** With the glasses on, verify vsync fires, measure real latency, prove swim is reduced, and dial in `scanout-ms` / `predict-cap-ms` / gate thresholds by eye. Per the repo's hardware-test division of labor: **run all commands end-to-end yourself; the user supplies only the eyes-on-glasses feedback.** This is not a code-TDD task — each step is an observation with a decision.

**Pre-flight (every run):**
- Ensure nothing holds the single-client SDK: `ps aux | grep -Ei 'viture-bridge|spatial-screens' | grep -v grep` — stop any holder first.
- Confirm the glasses enumerate: `lsusb | grep -i 35ca`.
- Inhibit idle blanking (NVIDIA DPMS-hang lesson) before a long session.
- Launch: `cd spatial-screens && ./run.sh` (background), then watch the status line in the log.

- [ ] **Step 1: Baseline measurement (predict off)**

Edit `~/.config/spatial-screens.conf`: set `predict-mode = off`. Launch. Read the new diagnostic line for ~30 s and record: `dt mean/p95/max`, and **`vsync ok` vs `vsync ABSENT`** (+ the reported vsync interval). Ask the user to note the baseline swim while turning their head side-to-side. **Decision:** if `vsync ABSENT`, the vsync clock isn't available under the lease — the fallback path (fixed one-frame) will drive prediction; note this and continue (the campaign still tunes `scanout-ms`/cap via the fallback).

- [ ] **Step 2: Turn prediction on, first A/B**

Set `predict-mode = vsync`, `scanout-ms = 5`, `predict-cap-ms = 35`. Relaunch. Ask the user to compare swim on head turns vs the baseline (Step 1). Record: reduced / same / overshoot. Confirm the status line shows a non-zero `predict … ms` that rises during motion and falls to ~0 at rest.

- [ ] **Step 2b: If `vsync ABSENT`, remove the FIFO cushion (optional)**

Only if vsync is absent AND swim is unchanged: try `predict-mode = fixed` with `predict-ms` set to the measured `dt mean` (one frame). Re-A/B. This isolates whether prediction helps at all when the vsync clock is unavailable.

- [ ] **Step 3: Sweep `scanout-ms`**

Hold `predict-mode = vsync`. Try `scanout-ms ∈ {0, 4, 8, 12}` (relaunch each). For each, ask the user: does content sit *on* the world during a steady turn (good), *lag behind* (raise scanout-ms), or *lead/overshoot* at turn-end (lower it)? Record the value that best glues content to the world.

- [ ] **Step 4: Tune overshoot at turn-end (gate + cap), only if needed**

If the user reports overshoot when a turn stops: lower `predict-cap-ms` (e.g. 25, then 20). If prediction feels twitchy starting/stopping small movements, the gate ramp is too eager — widen it (raise `ang-ramp`) or its deadband (raise `ang-dead`) by editing the `predict_gate(...)` call in `main.cpp:Step 3` of Task 6 and rebuilding. Record final `predict-cap-ms` and any gate change.

- [ ] **Step 5: Regression checks**

At the chosen settings, confirm with the user: (a) **no new at-rest shimmer** vs `predict off` (hold still, stare at a screen); (b) gestures unaffected — `two_up` select, fist-hold recenter, both-hand grab/resize; (c) `fps` in the status line unchanged from baseline.

- [ ] **Step 6: Persist chosen defaults**

If the winning `scanout-ms` / `predict-cap-ms` differ from the code defaults (`5` / `35`), update the defaults in `spatial-screens/src/config.h` (and the Task 2 test expectation in `stereo_math_test.cpp` if you change `scanout_ms`/`predict_cap_ms` defaults), and note the chosen values in `~/.config/spatial-screens.conf`. Rebuild + `make test`.

- [ ] **Step 7: Record the campaign result**

Update `docs/branches/feat-vsync-pose-predict.md` (Verification section) with: vsync fired? (y/n), measured `dt` + latency, chosen `scanout-ms`/`predict-cap-ms`/gate, and the user's swim verdict. Update `docs/plan/roadmap.md` to mark the motion-swim item done (or note residual → Approach C).

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "tune(predict): hardware tuning campaign — chosen scanout/cap/gate, campaign log"
```

---

## Self-Review

**Spec coverage:**
- VSync clock → predict target (spec §C1, §C2) → Task 1 (`compute_predict_s`, `vsync_interval_update`) + Task 4 (callback/atomics).
- Late-sample (§Architecture) → Task 5.
- dt-fix (§C3) → Task 1 (`one_euro_alpha`) + Task 3.
- Speed-gated prediction (§C4) → Task 1 (`predict_gate`) + Task 6.
- Measurement/instrumentation + fallback (§C5) → Task 4 (status line, `vsync ok/ABSENT`) + Task 6 (fixed fallback).
- Config (§C6) → Task 2 + Task 6 (loop wiring).
- Default `predict-mode=vsync`, `--scanout-ms` knob, reversible `off` → Task 2 defaults + Task 6 off-path (Step 6).
- Tuning campaign (user request) → Task 7.
- Testing (§Testing) → Task 1 unit cases; Task 7 hardware A/B.

**Placeholder scan:** No TBD/TODO; every code step shows full code; hardware steps state exact config keys/values and the decision rule. Gate-threshold constants are concrete defaults tuned in Task 7.

**Type consistency:** `compute_predict_s`/`predict_gate`/`one_euro_alpha`/`vsync_interval_update` signatures match between Task 1 (definition), the tests, and Task 6 (call sites). `g_vsync_last`/`g_vsync_interval` (`std::atomic<double>`), `g_last_predict_ms` (`float`), `predict_mode`/`scanout_ms`/`predict_cap_ms` names identical across Tasks 2/4/6. `predict_s` in seconds → `*1e9` ns at the SDK call.

**Note on ordering:** Tasks 3-6 all edit `main.cpp` in sequence; execute in order (each builds on the prior). Task 6 Step 2 removes the `static` from `speed_hat` introduced in Task 3 — a deliberate hoist, called out explicitly.
