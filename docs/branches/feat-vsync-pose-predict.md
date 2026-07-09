# feat/vsync-pose-predict — branch record

**Status: COMPLETE — hardware PASSED 2026-07-09/10, MERGING to main.** Based on `main` @ `f38058d`.

**Final outcome (session 2):** the SDK-vsync prediction hit a hard ceiling (filter-damp→lag, transparent→shake, shake = SDK velocity noise). Broke it with **`smoothvel`** — predict head orientation from OUR de-noised angular velocity (`quat_delta_rotvec`/`quat_integrate` in `predict_math.h`, TDD'd; low-passed at `--vel-cutoff`). Hardware-tuned best tuple **`smoothvel --scanout-ms 14 --vel-cutoff 11`** = "maybe the best" (less swim, no shake, minimal overshoot); now the **compiled default**. **Approach C reprojection** (`--reproject`, off by default): renderer begin/submit split so the pose is sampled after the vblank-paced acquire — NO change single-screen (GPU not the bottleneck; residual is sensor→fused-pose latency; real fix would be raw-IMU late-latch, deferred); kept for the GPU-bound multi-screen case. **Framerate:** 2×2 stereo was composite-bound (~74fps judder); switched `run.sh` to a **native-res grid** (tile native 2560×1600 instead of upscaling to 3840×2400) → **~88fps at 2×2** with smoothvel+gestures, NPU offload proved unnecessary. Full default config (`./run.sh`, no flags) hardware-validated at 2×2: judder gone, swim reduced, gestures work, 1280×800/screen accepted. Also added `--no-gestures` (fps A/B). Commits: `fa3b4fa` calibration knobs · `f9f98c6` smoothvel · `1de6fb3` reproject · finalization (defaults + native-res grid).

## What it does

Reduces **motion swim** of world-anchored screens — the lag/overshoot on head
turns — by predicting the head pose forward to the next display scanout, timed
off the SDK's `XRVSyncCallback` (previously a `nullptr` slot). Samples the pose
as late as possible in the loop, makes the existing One-Euro filter use real
per-frame `dt`, and gates prediction magnitude by head speed so it decays to ~0
at rest (the failure mode of the earlier `--predict-ms` attempt).

Scope is the confirmed complaint: **swim during motion**, not at-rest shimmer.

## Design decisions (brainstormed 2026-07-09)

- **Approach B** — vsync-timed prediction, single-threaded loop. Async high-rate
  IMU late-latch (Approach C) deferred as the escalation if residual swim remains.
- **`predict-mode = vsync` default (on out of the box)**; reversible via
  `--predict-mode off`.
- **`--scanout-ms` config knob** (default 5 ms), tuned by eye on hardware.
- **Compose order: SDK-predict → One-Euro filter, single sample/frame**;
  speed-gated so at rest predict ≈ 0.
- Spec: `docs/specs/2026-07-09-vsync-timed-pose-prediction-design.md`.
- Plan: `docs/superpowers/plans/2026-07-09-vsync-pose-predict.md`.

## Implementation (6 code tasks, TDD, subagent-driven)

- `spatial-screens/src/predict_math.h` (+ `predict_math_test.cpp`) — pure unit:
  `one_euro_alpha`, `vsync_interval_update`, `compute_predict_s` (negative
  sentinel when vsync stale/absent), `predict_gate`. Makefile `predict-math-test`.
- `spatial-screens/src/config.{h,cpp}` — `predict-mode` (off|fixed|vsync, default
  vsync), `scanout-ms` (5), `predict-cap-ms` (35); `predict-ms` implies `fixed`.
  Surfaced in usage + `--dump-config`.
- `spatial-screens/src/main.cpp`:
  - dt-corrected the One-Euro filter (real per-frame `dt`, not `Te=1/120`) via
    `one_euro_alpha`; added `mono_now_s()`.
  - Registered `on_vsync` (own monotonic-clock stamp, ignores SDK timestamp) →
    atomics `g_vsync_last`/`g_vsync_interval`; frame-dt ring stats; status-line
    diagnostics (`predict … ms`, `dt mean/p95/max`, `vsync ok/ABSENT`).
  - Late-sampled the pose block (relocated below the capture tick, just before
    the view build).
  - Speed-gated vsync prediction: `predict_gate` on carried-over speeds →
    `compute_predict_s` (fixed fallback via `dt_mean_ema` when vsync absent) →
    `get_gl_pose_carina(pose, predict_s * 1e9)`. `off` ⇒ horizon 0.

Commits (on `feat/vsync-pose-predict`, base `f38058d`):
`35fe565` spec+record · `2be0ef5` plan · `3b03089` math unit · `ac502ad` config ·
`609004b` dt-fix · `df81a27` vsync+instrumentation · `29e3eeb` late-sample ·
`6078e8a` prediction · `ab0d6b3` review-fix (gitignore + test asserts).

## Verification

- **Unit:** `make test` → all four suites (`gesture-parse`, `gesture-manip`,
  `stereo-math`, `predict-math`) `all checks passed`.
- **Build:** `make` clean (only the pre-existing `../bridge/ws_server.hpp`
  `-Wmissing-field-initializers` warning, unrelated).
- **Reviews:** per-task spec+quality review on each of Tasks 1–6 (Task 6 on
  opus) — all Approved, 0 Critical/Important. Whole-branch review (opus):
  **Ready to merge with fixes**, 0 Critical, 0 correctness defects; the one
  pre-hardware fix (.gitignore) + two test-assert nits applied in `ab0d6b3`.
- **Hardware pass 2026-07-09: IN PROGRESS** — tuning campaign day 1, see below.

## Tuning campaign — day 1 (2026-07-09)

Ran on the glasses (1920×1200@90, 2×2 stereo workspace). Runs launched from the
worktree via `./run.sh <flags>`; diagnostic status line reports
`predict Xms  dt mean/p95/max  vsync ok|ABSENT`.

**Findings:**
- **VSync FIRES under the leased direct-mode display** — `vsync ok`, interval
  converges to ~11.1 ms = exactly 90 Hz. The big unknown is resolved *positively*:
  the fallback path is not needed on this hardware. Approach B is viable at the
  clock level.
- **Frame pacing:** at rest ~78–80 fps (dt mean ~12.8–13.3 ms) but **p95 ~28 ms,
  max 33.3 ms** — ~5–10% of frames span 2–3 vblank periods (FIFO cliff; baseline
  already sits just over one 11.1 ms vblank). Prediction is **nearly free**
  per-frame: at rest, `vsync` mode holds the same ~80 fps as `off`. The fps drop
  seen mid-turn (~55–65) was the **head-motion + FIFO-cliff confound**, NOT
  prediction compute cost.
- **Perceptual A/B (user eyes-on):**
  - `--predict-mode off` (= late-sample + dt-fix, no prediction): **"good, but a
    bit of that wiggle."** A real, liked improvement.
  - `--predict-mode vsync --scanout-ms 5` (cap 35): **"laggy, crunchy, violent
    zigzag."** Clear net-negative.
  - `--predict-mode vsync --scanout-ms 0 --predict-cap-ms 6` (gentle): **"better,
    but still jittery."** Reduced amplitude, jitter character remains.

**Interpretation:** the win so far is the **late-sample + dt-fix** baseline. The
**SDK's own forward-extrapolation** (`get_gl_pose_carina(predict_time)`) is too
noisy to use directly — predicting a noisy velocity zigzags, and our One-Euro
filter goes near-transparent during motion (by design, to stay snappy) exactly
when prediction is active, so it can't damp the residual. Reducing the horizon
(scanout/cap) lowers the amplitude but not the jitter character.

**Next session — options to try (roughly in order):**
1. **Don't amplify SDK velocity noise.** Try predicting from a *smoothed* velocity
   instead of the SDK's raw prediction: keep the anchor pose filtered and apply a
   low-passed forward-extrapolation ourselves (the deferred design option (ii) /
   the cheap end of Approach C). This is the most promising direction — it
   targets the root cause (noise amplification) rather than the magnitude.
2. **Split position vs orientation prediction.** Orientation is the visible
   zigzag channel for distant screens; try predicting position only, or filtering
   the *predicted* orientation harder during motion.
3. **Even gentler / fractional prediction** (predict only a fraction of the
   horizon) to find any sweet spot — low expectation given the trend.
4. **If none clean up the jitter:** ship the win — flip default to
   `--predict-mode off` (keep vsync as experimental), merging the late-sample +
   dt-fix improvement the user liked; pursue smoothed extrapolation (Approach C)
   as a separate follow-up branch.

**State for resume:** all code committed on `feat/vsync-pose-predict` (HEAD after
this doc commit). Worktree intact; SDK symlinked (`sdk/include`,`sdk/lib` →
main checkout, git-excluded). Rebuild with `cd spatial-screens && make`; relaunch
with `./run.sh --predict-mode <off|vsync> [--scanout-ms N] [--predict-cap-ms N]`.
`main` untouched at `f38058d`.

## Hardware-pass watch items (from the whole-branch review)

1. **A/B interpretation.** `--predict-mode off` is NOT the true pre-feature
   baseline: it already includes the **late-sample** + **dt-fix** (prediction
   disabled). So `off` vs `vsync` isolates the *prediction* increment only. To
   attribute the full swim delta, add a third leg: a **`main`-HEAD build** as the
   genuine pre-feature baseline (build from `main` @ `f38058d`).
2. **At-rest position shimmer.** The dt-fix raises the One-Euro position alpha at
   rest ~0.020→0.027 (~33% more per-frame gain) because real dt (~11.1 ms) >
   old `Te` (8.33 ms). Orientation (the dominant shimmer channel) is UNCHANGED.
   So the "no at-rest shimmer regression" check should specifically compare
   **position** stability at rest — hold still and stare at a screen.
3. **Torn read of the two vsync atomics** is benign (interval slowly-varying,
   result clamped+gated, store order is the harmless direction) — no fix.

## Notes for future

- If vsync does NOT fire under a leased direct-mode display, the feature degrades
  to fixed one-frame prediction (fallback); the status line reports `vsync ABSENT`.
- If B leaves residual swim, Approach C (async IMU late-latch) is the next feature.
- Gate thresholds mix linear (m/s) and angular (deg/frame) terms via `max()`;
  `ang-dead=2`/`ang-ramp=20` are frame-rate-relative — keep in mind when tuning.
