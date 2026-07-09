# feat/vsync-pose-predict — branch record

**Status: IMPLEMENTATION COMPLETE, reviewed clean. PENDING the on-glasses tuning campaign (Task 7).** Worktree at `.claude/worktrees/vsync-pose-predict`, based on `main` @ `f38058d`.

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
- **Hardware pass 2026-07-09: PENDING** — the Task 7 tuning campaign below.

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
