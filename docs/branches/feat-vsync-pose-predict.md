# feat/vsync-pose-predict — branch record

**Status: SPEC APPROVED, pre-implementation.** Worktree at
`.claude/worktrees/vsync-pose-predict`, based on `main` @ `f38058d`.

## What it does

Reduces **motion swim** of world-anchored screens — the lag/overshoot on head
turns — by predicting the head pose forward to the next display scanout, timed
off the SDK's `XRVSyncCallback` (currently a `nullptr` slot). Sample the pose as
late as possible in the loop, make the existing One-Euro filter use real
per-frame `dt`, and gate prediction magnitude by head speed so it decays to ~0 at
rest (the failure mode of the earlier `--predict-ms` attempt).

Scope is the confirmed complaint: **swim during motion**, not at-rest shimmer
(the One-Euro filter already handles that).

## Design decisions (brainstormed 2026-07-09)

- **Approach B** — vsync-timed prediction, single-threaded loop. The async
  high-rate IMU late-latch (Approach C) is deferred as the escalation if residual
  swim remains.
- **`predict-mode = vsync` default (on out of the box)**; reversible via
  `--predict-mode off` (which reproduces today's behavior for clean A/B).
- **`--scanout-ms` config knob** (default ~5 ms), tuned by eye on hardware.
- **Compose order: SDK-predict → One-Euro filter, single sample/frame**;
  speed-gated so at rest predict ≈ 0.
- Spec: `docs/specs/2026-07-09-vsync-timed-pose-prediction-design.md`.
- Plan: `docs/superpowers/plans/2026-07-09-vsync-pose-predict.md` (TBD — next).

## Implementation

_(to be filled in during execution)_

Planned surface:
- `spatial-screens/src/predict_math.h` (+ `predict_math_test.cpp`) — pure vsync
  estimator, predict-target, speed gate, One-Euro alpha.
- `spatial-screens/src/main.cpp` — register `on_vsync`; atomic vsync publisher;
  late-sample the pose block; `dt`-correct the One-Euro filter; speed-gated
  `predict_ns`; status-line instrumentation + fallback.
- `spatial-screens/src/config.{h,cpp}` — `predict_mode`, `scanout_ms`,
  `predict_cap_ms`.
- `spatial-screens/Makefile` — `predict-math-test`.

## Verification

_(to be filled in)_ — unit (`make predict-math-test`), build (`make`), reviews,
then hardware A/B pass (`--predict-mode vsync` vs `off`): confirm reduced swim on
head turns, no at-rest shimmer regression, `vsync ok` in the status line (and the
`ABSENT` fallback path), fps unchanged, gestures unaffected.

## Notes for future

- If vsync does **not** fire under a leased direct-mode display, the feature
  degrades to fixed one-frame prediction (fallback), still improving swim; the
  status line reports `vsync ABSENT`.
- If B leaves residual swim, Approach C (async IMU late-latch) is the next
  feature.
