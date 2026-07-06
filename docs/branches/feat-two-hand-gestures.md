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
- **Hand detection**: single camera frame, `num_hands=2` — see the pivot below.

## Hardware bring-up (2026-07-06) — pivoted to single-frame

The original design used **dual camera, per-hand** (left hand from the left
tracking camera, right hand from the right). On-glasses testing killed it:

1. **Routing by MediaPipe handedness label** → the same physical hand is labelled
   differently by the two stereo views, so a lone hand appeared in BOTH panels.
2. Switched to **routing by spatial x-position** → still doubled: the ~6 cm
   stereo baseline means the two cameras disagree on which side a *near-center*
   hand is on (parallax), so it's claimed by both. This is **inherent** to any
   two-independent-cameras approach — not tunable.
3. Also **2× inference lag**: threading the two inferences (1.74× faster) plus a
   rate-cap bump got 25 Hz hand-free, but with hands the sender out-drove the
   sidecar and its mutex stalled the render loop (fps → single digits).

**Decision: single-frame `num_hands=2`** — detect both hands in ONE image; the
left/right split comes from one consistent x-axis, so a hand is left OR right,
never both. Also one inference (no lag). `MIRROR_HANDEDNESS=False` (True read
inverted on hardware). The C++ side still sends both planes but the sidecar uses
only the left; the second plane is reserved for **camera fusion for depth**
(design doc "Future ideas") — the SDK exposes no stereo calibration, so real
fusion is a multi-day self-calibration project, deferred.

Also added on this pass: a SIGSEGV/SIGABRT **crash-backtrace handler** in
`main.cpp` (built with `-g -rdynamic`) to catch the intermittent direct-mode
crash. Testing gotcha: `--window` mode dies from an Xlib "connection broken"
compositor quirk after ~30 s; **direct mode is the stable path** (a crash there
wedges DP-1 at `non-desktop=1` — recover with `xrandr --output DP-1
--set non-desktop 0 --auto`, or replug).

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
- [x] Implementation plan — `docs/specs/2026-07-06-two-hand-gestures-plan.md`
- [x] Implementation — **all 10 tasks + follow-ups done, every unit test green,
      clean build.** Executed subagent-driven (per-task independent review;
      opus review on the state machine). SDD ledger: `.superpowers/sdd/progress.md`.
- [ ] **Hardware verification pass** (needs the glasses — single SDK client, no
      viture-bridge / other spatial-screens session running; launch `./run.sh`):
    - [ ] **Confirm `MIRROR_HANDEDNESS`** (`gestures/classify.py`): does the left
          hand light up the LEFT status dot / left overlay panel? If swapped,
          flip the constant.
    - [x] **Inference throughput** — RESOLVED autonomously 2026-07-06. Findings:
          downscaling the camera stream does NOT help (MediaPipe resizes to fixed
          internal sizes; inference flat 24–27 ms from 640×480→160×120). Fix was
          (a) run the two landmarker inferences concurrently (1.74×, ~52→30 ms;
          commit `15c7917`) and (b) raise `GESTURE_INFER_HZ` 15→30 to un-alias
          the 25 Hz camera's 40 ms grid (commit `65673a5`). Live rate went
          **12.5 → stable 25 Hz**, no backpressure. Hand-free measured; confirm
          it holds with two hands actively tracked (threaded cycle ~40 ms is near
          the 40 ms budget — may dip gracefully, drain-loop absorbs). Single-frame
          `num_hands=2` fallback remains available but is no longer needed.
    - [ ] **Tune grab feel**: `GRAB_REPOSITION_GAIN`, `GRAB_DIAG_MIN/MAX` in
          `main.cpp`.
    - [ ] **Check resize parallax**: if the two-hand resize drifts, source both
          grab pinch points from the left frame during a grab.
    - [ ] Sanity-check the HUD: three bottom dots (left-hand / VO-center /
          right-hand), left/right overlay panels, per-hand grey/amber/blue/green.
- [ ] Final whole-branch review (recommended before merge — can run
      `/code-review` or ultra; per-task reviews already passed)
- [ ] Merge to master

## Implementation map (commits on this branch)

| Task | Commit(s) | What |
|------|-----------|------|
| 1 | `03bb192` | multi-plane frame protocol (Python) |
| 2 | `0846d45` | two-hand event schema (left/right sub-objects) |
| 3 | `8775444` | handedness selection + `MIRROR_HANDEDNESS` |
| 4 | `a5f0658` `b9c07d7` | `HandState`/`GestureEvent` C++ parser (+legacy-view test cover) |
| 5 | `1234efa` `b4cb262` | pure grab math + hardened tests |
| 6 | `cd68f4d` | dual-landmarker sidecar (route by handedness) |
| 7 | `edc61bb` | forward both camera planes (C++ sender) |
| 8 | `aee3ad7` | per-hand arming + two-hand grab state machine |
| 9 | `dda8998` | two-hand overlay + per-hand status dots + centered VO dot; drop legacy view |
| 9b | `cd5fc93` | split overlay into left/right panels |
| docs | `421d9ce` `28e2260` | plan/design fixes + downscaled-inference future idea + status-dot plan |

Base: branched off `master`.
