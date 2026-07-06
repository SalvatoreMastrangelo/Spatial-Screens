# feat/two-hand-gestures — two-hand gesture support

Branch resume doc. Update this as the branch evolves; it must always be enough
to pick the work back up cold.

## Goal

Extend the `spatial-screens` gesture system from one tracked hand to two:

- Either hand performs today's single-hand gestures (open-palm arm, fist-hold
  recenter, pinch-drag distance) — symmetry.
- New **two-hand grab**: both hands armed + pinching → spread resizes the screen
  (diagonal), midpoint repositions it laterally. Depth stays on the one-handed
  drag.
- **Per-hand arming**: each open palm arms its own hand.
- **Dual camera, per-hand**: left hand from the left tracking camera, right hand
  from the right camera. No stereo fusion (parked as a future idea).

## Design of record

`docs/specs/2026-07-06-two-hand-gestures-design.md` — approved 2026-07-06.
Builds on `docs/specs/2026-07-03-hand-gesture-control-design.md` (one-hand
sidecar) and the hand-overlay work.

## Key decisions / open risks

- **2× inference cost** is the biggest risk. Two MediaPipe passes at 15 Hz.
  Fallback if too heavy: one frame, `num_hands=2` (both hands from left camera).
  Decided on the hardware pass by measuring inference latency.
- **Handedness mirror**: MediaPipe Left/Right labels invert on a forward-facing
  camera. Isolated to a `MIRROR_HANDEDNESS` constant; pinned on hardware.
- **Cross-camera parallax** on the two-hand spread: mitigated by using the
  ratio-from-grab-start. Fallback (not built): source both grab pinch points
  from the left frame during a grab.
- v1 scope: translation + scale only (no rotation), lateral reposition only (no
  z), no stereo fusion.

## Current state / next step

- [x] Design doc written + approved (2026-07-06)
- [ ] Implementation plan (writing-plans) — next
- [ ] Implementation
- [ ] Hardware verification pass (handedness value, inference latency, grab feel)
- [ ] Merge to master

Base: branched off `master`.
