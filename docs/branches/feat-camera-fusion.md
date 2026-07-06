# feat/camera-fusion — stereo camera fusion for per-hand depth

Branch resume doc. Update this as the branch evolves; it must always be enough
to pick the work back up cold.

Worktree: `.claude/worktrees/camera-fusion` · **branched from `main`** @ `80e3a23`
(NOT from `feat/screen-selection` — independent track).

## Goal

Fuse the glasses' two grayscale tracking cameras (`planes[0]`=left,
`planes[1]`=right, already both forwarded) via stereo triangulation to recover
each hand's true 3D position, and publish **per-hand depth** (+ per-landmark z)
on the existing gesture wire and dashboard telemetry.

**v1 = a data channel, not a gesture.** No new gesture behavior. The
push/pull-to-move-in-Z gesture this unlocks is a deliberate follow-up branch.

## Design of record

`docs/specs/2026-07-06-camera-fusion-depth-design.md` — approved 2026-07-06
(brainstormed this session). Builds on the two-hand sidecar
(`docs/specs/2026-07-06-two-hand-gestures-design.md`, "Future ideas" is where
this was surfaced) and the stereo-3d renderer.

## Scope decisions (brainstorming, 2026-07-06)

- **Deliverable:** per-hand depth on wire + dashboard. No gesture. (Middle
  increment — not a throwaway spike, not a full depth-gesture feature.)
- **Calibration:** assumed intrinsics (focal from FOV) + assumed ~6 cm baseline.
  No checkerboard/ritual in v1 → rough-scaled depth (reliable near/far, not
  metric cm). Checkerboard self-calibration = documented future upgrade, slots
  into `stereo.py` with no caller change.
- **Depth method:** landmark triangulation, **2× inference** (MediaPipe on both
  planes, match hands across the pair, triangulate matched landmarks).
- **Real-time strategy:** **B — threaded dual inference at full cadence.**

## Key decisions / open risks

- **THE risk — the 2×-inference backpressure stall that killed the first
  dual-camera two-hand build.** `maybe_send_frame` runs under the render-loop
  `mutex_`, sends a ~614 KB two-plane frame at `GESTURE_INFER_HZ=15`, and
  **disables gestures** if it can't drain within a 200 ms deadline. Safeguards
  (see design §Real-time strategy):
  1. **Latest-frame-wins drain** in the sidecar (the missing piece) — drain the
     socket to the newest frame before each inference so the C++ send never
     nears its deadline.
  2. **Two threaded landmarkers** (~1.74× single-inference wall time).
  3. **Re-measured rate cap** (start at hardware-proven 15 Hz; raise only with a
     fresh on-glasses measurement).
  - Fallback if the drain is insufficient: move the C++ send off the render
     mutex (dedicated sender thread + latest-frame mailbox). Not built in v1.
  - **Hardware pass MUST test with both hands present** (the exact stall case),
    not just idle.
- **No calibration from the SDK** — `XRCameraCallback` gives raw buffers +
  width/height only (no intrinsics/baseline/extrinsics). v1 assumes params;
  depth is rough-scaled. Accept for a data channel; checkerboard is the upgrade.
- **Cross-branch conflict surface with `feat/screen-selection`:** both edit the
  `main.cpp` gesture block and `classify.py`/tests. Textual merge risk only, no
  functional dependency (obstruction analysis in roadmap item #4). Whoever lands
  second does a gesture-block integration pass (like two-hand × stereo).
- **Dependency note:** the two-hand review backlog's "drop `planes[1]` to halve
  bandwidth" optimization must NOT be applied — fusion needs both planes. This
  branch supersedes that backlog item.

## Current state / next step

- [x] Design doc written + approved (2026-07-06).
- [x] Roadmap backlog item #4 added; worktree + branch created from `main`.
- [x] Branch resume doc (this file).
- [x] Implementation plan — `docs/specs/2026-07-06-camera-fusion-depth-plan.md`
      (8 tasks, TDD). NOTE: `stereo.py` renamed `depth_fusion.py` in the plan to
      avoid clashing with the C++ `src/stereo.h` (SBS rendering); `landmarks_z`
      wire field deferred (per-hand `depth` only in v1).
- [x] Implementation — **all 8 tasks done, subagent-driven (implementer +
      spec/quality review per task), every unit test green.** Commits
      `49317bc`(depth_fusion.py) `6a8edcf`(wire depth) `e08520e`(C++ parse)
      `9b80875`(pure helpers) `1eb5b28`(dual-inference + drain) `97bd8b2`(--fusion
      plumbing) `3200bf8`(telemetry + HUD dot) `36f4f38`(dashboard). SDD ledger:
      `.superpowers/sdd/progress.md`.
- [x] Final whole-branch review (opus) — **READY TO MERGE (code)**, no Critical;
      5 Minors all triaged DEFER (verified safe); 2 Importants were conscious
      scope decisions, both resolved by the default-on flip below.
- [x] **DEFAULT FLIPPED TO FUSION-ON** (`ab5f750`) at user request, to stress the
      strategy-B edge cases on-glasses before deciding opt-in-vs-default. Escape
      hatch: **`--no-fusion`** falls back to the known-good single-inference path
      (use it if the 2×-inference send-deadline stall disables gestures
      mid-session). `--fusion` still force-enables (now redundant). **Final
      default (keep on vs revert to opt-in) is DEFERRED to after the hardware
      pass.** Design/plan docs still describe the original opt-in framing — update
      them once the default is decided.
- [ ] **Hardware verification pass — THE go/no-go.** Launch default `./run.sh`
      (now fusion-ON). Confirm: (1) depth monotonic near/far on the dashboard +
      HUD dot shrinks with distance; (2) **fps holds with BOTH hands present**
      (the exact case that stalled the first dual-camera build — the drain bounds
      backlog, NOT per-cycle inference time, so this is the real risk); (3)
      `--no-fusion` cleanly restores today's single-inference behavior. If (2)
      fails, the documented fallback is "send off the render mutex" (design
      §Real-time strategy).
- [ ] Decide final default (keep fusion-on vs revert to opt-in) + update
      design/plan/roadmap wording; then merge to `main`.

## Files (planned — see design §Files touched)

New: `spatial-screens/gestures/stereo.py`, `tests/test_stereo.py`,
`tests/test_hand_tracker.py`. Modified: `hand_tracker.py`, `protocol.py`,
`gesture_client.h`, `gesture_parse.{h,cpp}`, `gesture_parse_test.cpp`,
`main.cpp`, `sensor-viz/`, `classify.py` (optional), `docs/plan/roadmap.md`.

Base: branched off `main`.
