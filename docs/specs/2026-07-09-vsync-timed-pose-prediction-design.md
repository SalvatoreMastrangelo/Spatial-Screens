# VSync-Timed Pose Prediction (Motion-Swim Reduction) — Design

Date: 2026-07-09
Status: approved, pre-implementation
Branch: `feat/vsync-pose-predict` (worktree at `.claude/worktrees/vsync-pose-predict`)

## Problem

World-anchored screens **swim during head motion**: on a head turn they lag /
overshoot the real world, then settle. This is the classic sample-to-photon
latency effect, not tracking noise. Confirmed with the user: the complaint is
motion swim, *not* at-rest shimmer (which the existing One-Euro filter already
handles well).

Today the render loop **polls** `get_gl_pose_carina(g_provider, pose, predict=0)`
once at the *top* of the loop (`main.cpp:1026`), then builds the view matrix that
actually places the screens ~130 lines later (`main.cpp:1157-1188`), after the
6DoF-liveness pass, the capture upload, and the cursor composite — and only then
does the **FIFO (vsync-locked)** present block until vblank. So every pose is
stale by `(CPU work this frame) + (wait-to-vblank) + (scanout)` — on the order of
15-25 ms at ~85 fps against a 90 Hz panel — by the time its photons reach the eye.
For a screen 3 m away, 20 ms of head-rotation lag is very visibly swim.

The pose is already sampled at panel rate, so **raising the poll rate does
nothing** — we cannot display faster than we render, and sampling the same VIO
estimate more often does not reduce latency. The lever is **predicting the pose
forward to the moment its photons land**, timed off the display's real vsync
clock.

## Summary

Predict the head pose to the next scanout using the SDK's VSync callback as the
display clock, sample it as late as possible in the loop, and make the existing
One-Euro filter use the real per-frame `dt` so prediction and filtering compose
correctly. Prediction magnitude is **gated by head speed**, so at rest it decays
to ~0 (the specific failure mode of the previous prediction attempt — see below).

Approach **B** from brainstorming: vsync-timed prediction, single-threaded loop.
No async pose thread, no IMU latch (that is Approach C, explicitly deferred).

## Why prediction was disabled before, and why this is different

`main.cpp:264-271` records that a prior `--predict-ms` attempt "visibly amplifies
rotation jitter during head turns" and "extrapolation noise feeds the filter's
speed estimate and reads as shake," so the default is `predict_ms = 0`. That
attempt failed for three concrete, fixable reasons this design addresses:

1. **No measured target.** It used a hand-set constant, not the actual
   sample-to-photon interval. → We derive the target from the vsync clock (§C2),
   after a measurement pass (§C5) quantifies it.
2. **Predicted at rest.** A fixed predict extrapolates VIO/gyro noise even when
   the head is still, which reads as shimmer. → Prediction is **speed-gated**
   (§C4): still head → predict ≈ 0.
3. **Filter `dt` was wrong.** The One-Euro filter hardcodes `Te = 1/120` s
   (`main.cpp:1041`) while frames actually arrive at ~85 Hz and *vary* (60-89 in
   the last hardware session). A predicted signal fed through a mistuned,
   timing-blind filter behaves unpredictably. → Use measured `dt` (§C3).

## Decisions (locked during brainstorming)

- **Direction B: vsync-timed prediction.** Predict to the next vblank via the
  SDK `XRVSyncCallback`, not to a guessed constant. Async high-rate IMU latch
  (C) is deferred as the escalation if residual swim remains.
- **Default `predict-mode = vsync` (on out of the box).** `run.sh` shows the
  improvement immediately; reversible via `--predict-mode off`. Hardware-tested
  before merge per the repo workflow.
- **Scanout term is a config knob**, `--scanout-ms` (default ~5 ms), dialed in by
  eye on the hardware A/B pass — same pragmatic path as `pitch-trim` /
  `capture-hz`. Not auto-derived from panel timing (deferred).
- **Compose order: SDK-predict → One-Euro filter, single sample per frame.** The
  filter is near-transparent during motion (so the prediction survives) and heavy
  at rest (where the gate already zeroed the prediction). Anchor / gesture /
  telemetry keep reading this predicted+filtered `head_q` (sub-millidegree
  forward bias at gesture moments — negligible).

## Goals

- Motion swim during head turns measurably reduced (photons track the real world
  within the residual filter lag), with **no at-rest shimmer regression**.
- Prediction target derived from the real display clock, with a safe fallback
  when the vsync callback is unavailable.
- The One-Euro filter uses real per-frame `dt`.
- Fully reversible on hardware via config (`--predict-mode off` reproduces
  today's behavior exactly), for clean A/B.
- Pure prediction/estimator math is unit-tested (no SDK/Vulkan/X deps).

## Non-goals

- **No async pose thread / IMU late-latch** (Approach C). If B leaves residual
  swim, that is the next feature, not this one.
- **No change to the noise filter's intent** — orientation stays light, position
  stays One-Euro; we fix `dt` and add prediction, we do not re-tune the noise
  floor.
- **No auto-derived scanout latency** from panel mode lines/refresh (config knob
  instead).
- **No prediction of hand / gesture landmarks** — head pose only.
- **No telemetry-dashboard UI** for the new stats beyond the console status line
  (dashboard wiring is optional future polish).

## Architecture

One new pure-logic unit (`predict_math.h` + test) plus localized edits to
`main.cpp` and the config unit. The SDK's VSync callback — currently `nullptr` at
`main.cpp:183` — is wired to a tiny atomic publisher; all timing math is pure and
testable.

```
SDK vsync thread ──on_vsync(ts)──▶ g_vsync{last_ts, interval_ema}   (atomics, main.cpp)
                                            │  (render loop reads only)
render loop, per frame:                     ▼
  … gestures (use last frame's head_q) …
  … capture tick (upload; pose-independent) …
  ┌─ LATE POSE UPDATE (relocated here, just before view build) ─────────────┐
  │ dt          = clamp(now - last_now, 1/240, 1/30)                          │
  │ gate        = speed_gate(speed_hat, ang_speed)          // 0 at rest → 1  │
  │ predict_ns  = compute_predict_ns(now, g_vsync, scanout_s, cap_s) * gate   │
  │ get_gl_pose_carina(pose, predict_ns)                    // SDK extrapolates│
  │ One-Euro(pose, dt) → head_q / head_p                    // dt-corrected    │
  │ telemetry.send_pose ; 6DoF-liveness window                                │
  └───────────────────────────────────────────────────────────────────────────┘
  build view matrix (head_q/head_p) → draw list → FIFO present (vblank)
```

`predict-mode off` ⇒ `predict_ns = 0` and the block behaves exactly as today
(save the `dt` fix, which stands on its own).

## Components

### 1. VSync clock — `main.cpp` (+ pure estimator in `predict_math.h`)

- Register a real vsync callback in place of the `nullptr` at `main.cpp:183`:
  `register_callbacks_carina(g_provider, on_pose_noop, on_vsync, on_imu_noop,
  on_camera_carina)`. (`on_imu_noop`/`on_pose_noop` stay no-ops — we still poll
  the fused pose; only vsync is newly consumed.)
- `on_vsync(double ts)` runs on the SDK callback thread. It updates two atomics
  (`std::atomic<double> g_vsync_last_ts`, `std::atomic<uint64_t>` bit-cast
  interval, or a small mutexless struct): `last_ts = ts` and an EMA of the
  inter-vsync interval. The EMA update itself is a pure function
  `vsync_interval_update(prev_interval, prev_ts, ts)` in `predict_math.h`.
- The render loop reads a snapshot each frame.

### 2. Prediction-target math — `predict_math.h` (pure)

```
// Nanoseconds to predict a pose sampled at `now` forward to the vblank that
// will scan it out. Returns a NEGATIVE sentinel (e.g. -1) — distinct from a
// legitimate 0 (predict-to-now) — when vsync data is stale/absent, so the
// caller can branch to the fallback (§C5).
double compute_predict_ns(double now, double last_vsync, double interval,
                          double scanout_s, double cap_s /* clamp */);
```

- Phase: advance `last_vsync` by whole `interval`s until `> now`; that is the
  next vblank. Target = `(next_vblank - now) + scanout_s`.
- Clamp the valid result to `[0, cap_s]` (default cap ~35 ms) so a stalled
  callback can never produce runaway extrapolation.
- Staleness: if `interval <= 0` or `now - last_vsync > k*interval` (k≈8), return
  the negative sentinel so the caller uses the **fallback** (§C5) instead. A
  return of exactly 0 means "predict to now" (a real, in-range value), never
  "no data."

### 3. `dt`-corrected One-Euro filter — `main.cpp:1035-1071` (+ helper in header)

- Compute `dt = clamp(now - last_pose_now, 1/240.f, 1/30.f)` each frame (loop
  already reads `now_s()` via `steady_clock`, `main.cpp:667`); keep a
  `last_pose_now`.
- Replace the hardcoded `const float Te = 1.f/120.f;` (`main.cpp:1041`) with this
  `dt`, feeding both the speed-estimate smoothing (`ad`, `main.cpp:1045`) and the
  position/orientation alphas (`ap`, `main.cpp:1052`; `a`, `main.cpp:1064`).
- Extract the alpha formula to `one_euro_alpha(cutoff_hz, dt)` in `predict_math.h`
  so it is unit-tested; `main.cpp` calls it. No change to cutoffs / deadband /
  `smooth_pos` / `smooth_ori` semantics — only the timebase becomes real.

### 4. Speed-gated prediction — `main.cpp` (uses §C2, §C3 state)

- Reuse the filter's smoothed linear speed `speed_hat` (`main.cpp:1040-1046`) and
  add a smoothed **angular** speed from the frame's quaternion delta angle `ang`
  (already computed at `main.cpp:1061`) — swim is rotation-dominated, so the gate
  is driven mainly by angular speed.
- `gate = clamp((max(lin_term, ang_term) - deadband) / ramp, 0, 1)` (a
  smoothstep is fine); pure helper `predict_gate(speed_hat, ang_speed, ...)` in
  the header. Still head → `gate = 0` → `predict_ns = 0`.
- Final `predict_ns = compute_predict_ns(...) * gate` (fixed mode multiplies the
  configured constant by the same gate).

### 5. Measurement / instrumentation — `main.cpp` (status line)

Fold into the existing ~2 s `fps … cap … pose …` status line
(`main.cpp` render-stats print): frame-`dt` **mean / p95 / max**, the vsync
**interval estimate and a firing flag** (`vsync ok`/`vsync ABSENT`), and the live
`predict_ns`. This is what turns "guess a predict value" into "read the number,"
and it tells us on the first run whether the vsync callback fires under a leased
direct-mode display at all. **Fallback wiring:** when §C2 reports stale/absent
vsync, `predict-mode vsync` degrades to `fixed` using `interval ≈ mean dt` (i.e.
predict one frame), so the feature never depends on vsync actually arriving.

### 6. Config & flags — `config.h` / `config.cpp` / `main.cpp`

- `Options` (`config.h:34`, beside `predict_ms`): add
  `std::string predict_mode = "vsync";` (`off|fixed|vsync`),
  `float scanout_ms = 5.f;`, `float predict_cap_ms = 35.f;`.
- `set_option` (`config.cpp:118-131`): parse `predict-mode`, `scanout-ms`,
  `predict-cap-ms`. `predict-ms N` implies `fixed` at N (back-compat: a user who
  sets `predict-ms` gets fixed prediction, not vsync).
- `main.cpp` arg loop (`:304-317`) + `dump-config` (`:320-331`): usage text and
  effective-value dump for the three new keys.
- `--predict-mode off` ⇒ prediction fully disabled (today's behavior); the `dt`
  fix (§C3) still applies.

## Data flow

`head_q` / `head_p` remain the single source of truth for the view matrix,
gestures, telemetry, and liveness — the change is only *when* they are sampled
(later) and *with what prediction* (vsync-timed, speed-gated). Screen poses
(`scene_screen_pose`, rack frame) are untouched; prediction lives entirely in the
head pose. Stereo is unaffected: both eyes already derive from `head_q`/`head_p`,
so both inherit the prediction identically.

## Error handling / risks

- **VSync may not fire under a leased direct-mode display** (unverified). Mitigated
  by §C5 fallback (degrade to fixed = one frame) and surfaced by the `vsync
  ABSENT` flag. If absent, the feature still improves swim via fixed prediction.
- **Overshoot on sudden stops** (predict says "still turning"): bounded by the
  speed gate decaying, the `predict_cap_ms` clamp, and the One-Euro damping.
  Watch for it on the hardware pass at end-of-turn.
- **SDK internal prediction quality** (its velocity estimate) is unknown; the
  measurement pass reveals it. If poor at useful horizons, escalate to Approach C
  (own IMU extrapolation) — out of scope here.
- **Late-sampling vs gestures:** gestures (`main.cpp:800-1021`) already read the
  *previous* frame's `head_q`; relocating the sample below them keeps that
  invariant. The forward-predicted bias on the pose gestures read is
  sub-millidegree at gesture cadence — no behavioral change expected; confirm
  two_up pick / fist-hold recenter still feel right on hardware.
- **`predict-mode off` must be byte-for-byte today's behavior** apart from the
  `dt` fix — this is the A/B control; verify the status line shows `predict 0`.

## Testing

- **C++ pure unit** — new `spatial-screens/src/predict_math_test.cpp` (Makefile
  `test` target, alongside `stereo-math-test`):
  - `vsync_interval_update`: a steady 90 Hz timestamp stream converges the EMA to
    ~11.11 ms; a gap does not NaN.
  - `compute_predict_ns`: given `now` between vblanks, returns
    `(next_vblank - now) + scanout`; clamps at `cap`; returns the stale sentinel
    when `now - last_vsync` exceeds the threshold.
  - `predict_gate`: 0 at rest (speeds below deadband), saturates to 1 above the
    ramp, monotonic between.
  - `one_euro_alpha`: matches the closed form for representative `(cutoff, dt)`;
    larger `dt` ⇒ larger alpha (less smoothing) at fixed cutoff.
- **Hardware A/B pass** (run end-to-end on the glasses; user supplies eyes-on
  feedback): run with
  `--predict-mode vsync` vs `off`; head-turn side to side and confirm screens
  swim less / stay glued; confirm **no** new at-rest shimmer; confirm the status
  line reports `vsync ok` and a sane `predict_ns` (and check the `ABSENT`
  fallback path by forcing `fixed` too); confirm fps unchanged and gestures
  (two_up select, fist-hold recenter, both-hand grab) still work.

## Files touched

- `spatial-screens/src/predict_math.h` — **new**: pure vsync-estimator, predict
  target, speed gate, One-Euro alpha helpers.
- `spatial-screens/src/predict_math_test.cpp` — **new**: unit tests for the above.
- `spatial-screens/src/main.cpp` — register `on_vsync` (`:183`); atomic vsync
  publisher; relocate the pose-update block below the capture tick (late-sample);
  `dt`-correct the One-Euro filter (`:1035-1071`); speed-gated `predict_ns`;
  status-line instrumentation + fallback.
- `spatial-screens/src/config.h` / `config.cpp` — `predict_mode`, `scanout_ms`,
  `predict_cap_ms` options + parsing; `predict-ms` implies `fixed`.
- `spatial-screens/Makefile` — `predict-math-test` target; add to `test`.
- `docs/specs/2026-07-09-vsync-timed-pose-prediction-design.md` — this document.
- `docs/branches/feat-vsync-pose-predict.md` — branch resume doc.
- `docs/plan/roadmap.md` — add the motion-swim / prediction item.
