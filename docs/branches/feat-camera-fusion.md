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

## Hardware session 2026-07-08 — depth ✓ (single hand); NEW blocker surfaced

Launched the fusion hardware pass. Results:

**Fusion depth WORKS end-to-end** (single hand), measured non-invasively by
sniffing the WS telemetry on :8765 (`{"type":"hands",...,"left_depth","right_depth"}`,
`-1.0` = no-depth sentinel). Real depth flowed on 244/244 frames and varied smoothly
0.13–0.18 m as the hand moved near/far — impossible from one camera, so
`planes[1]`=`image_right0` is genuinely real+distinct and triangulation is live.
Absolute depth is ~3× compressed (hand at ~40 cm read ~0.14 m): expected from the
assumed 6 cm baseline / 70° FOV; relative near/far is correct.
**NOT yet tested: the both-hands-present fps go/no-go** (the 2×-inference stall) —
the session diverted into the ergonomics issue below before running it. That test
STILL gates the merge.

**NEW blocker — tracking forces an off-center (left) hand position.** User: "I have
to offset my hands to the left; I want them centered wrt my face." Investigated on
hardware via a temporary `HANDTRACK_DUMP` probe in `hand_tracker.py` (dumped both
planes as PNG + logged per-camera detection counts/wrist-x; **since reverted**,
git-clean). Findings:
- Gesture tracking uses `planes[0]` (LEFT camera) ONLY for landmarks; the right
  camera feeds depth only. Coverage is biased to the left camera's FOV → the offset.
- The tracking cameras are wide FISHEYE aimed DOWN (~35° pitch); the room was very
  dark → MediaPipe only detects in the low, lamp-lit zone. Saved raw frames confirm:
  top-of-frame = ceiling, hands live low near the desk.
- Measured (telemetry present-flag, hand held still): CENTERED at nose = **0%**
  detected (136 frames); same hand at the user's LEFT-offset = **87%** detected +
  85% depth. Vertical sweep nose→belly: detection switches ON as the hand lowers
  into the lit zone; at belly BOTH cameras track (clean stereo pair, wristx
  L≈0.26 / R≈0.27).
- Root cause of "offset left": left-camera-only tracking + that camera's leftward
  FOV. Darkness is a strong secondary factor.

**User goal (decided):** center hands L-R at a comfortable/**flexible height** with
a forgiving zone — NOT insisting on face height. Don't fight the downward angle.

**Direction C probe — DONE 2026-07-08 (autonomous; user away, hardware connected).**
Temporarily dumped all 4 planes from `on_camera_carina` (`--probe-camera`; edit
reverted, git-clean, clean rebuild done). RESULTS:
- **`image_left1` / `image_right1` are NULL every frame** — the SDK does NOT deliver
  a 2nd image pair on this device. No brighter hardware exposure exists; brightening
  must be done in SOFTWARE.
- Raw planes (640×480 GRAY8, no hand needed) are **brutally underexposed**: left0
  mean **12/255** (57% of px <10, 96% <30); right0 mean 10.6. The pair is distinct
  (mean|Δ|=7.9 → real stereo, not a duplicate buffer — re-confirms depth is legit).
- The darkness is **recoverable**: gamma-0.45 lifts mean 12→59 (0% left <30);
  gamma+CLAHE → mean 66 and the whole room resolves from near-black. So software
  brightening is viable and should expand the detectable zone into the dark regions.

### Design APPROVED + IMPLEMENTED 2026-07-08/09 (Tasks 1–4; Task 5 = hardware, pending)
Spec `docs/specs/2026-07-08-centered-hand-tracking-design.md`, plan
`…-centered-hand-tracking-plan.md`. User approved (both parts, gamma_clahe default,
union on-with-fusion, v1 nominal-disparity). Built subagent-driven, **UNCOMMITTED**
(user asked to review first): new `gestures/enhance.py` + `gestures/fuse_hands.py`
(+ tests), `hand_tracker.py` wired (per-thread enhancer — final-review fix), C++
forwards `--enhance`/`--no-both-cam`. `pytest tests/` = 48 pass; `make` links clean.
Final opus review: 1 Important fixed (shared CLAHE across the 2 fusion threads →
per-thread enhancer), #2 union-doubling = spec-anticipated hardware watch-item (see
Task 5 Step 4), #3/#4 minor hardening applied. **Next: run the plan's Task 5 on
glasses** (brightening, horizontal coverage map w/ self-paced protocol, centering,
two-hand grab, and the still-pending both-hands fps gate). Original incremental
framing below (superseded by "spec both parts together"):

### Design (approved; specced together) — INCREMENTAL framing (historical)
1. **Brightening pre-pass FIRST** (simple, validated, low-risk): in the sidecar
   `infer()`, apply gamma (~0.45) and/or CLAHE (clip~3, 8×8) to the GRAY8 plane
   before `cvtColor`→MediaPipe; apply to both planes; config-gated + tunable. This
   alone may largely fix "offset left" by making far more of the frame detectable
   (not just the lamp-lit spot). Re-test on hardware (needs a hand): how much does
   the usable zone widen / does centering improve?
2. **THEN both-camera detection union** ONLY if brightening isn't enough: use the
   right landmarker's detections (fusion already computes them) to extend horizontal
   coverage; disambiguate via the stereo depth-match (one 3D hand entity) — the
   fusion-native cure for the two-hand cross-camera confusion. Higher-risk
   (gesture-position coordinate continuity across cameras); defer until proven
   necessary. YAGNI.

Note: the both-hands fps go/no-go for the fusion branch itself is STILL untested and
still gates merging `feat/camera-fusion` — independent of this centering work.

### Coordination lesson (for hardware tests)
Blind fixed-window captures failed twice — the user watches the glasses, not the
terminal, and can't sync to a timed window. Next time use a self-paced protocol
(user says "now") or a live streaming monitor of the detection log, not timed sweeps.

## Next session — running the hardware pass (cold-start runbook)

Everything below runs from THIS worktree
(`.claude/worktrees/camera-fusion`, branch `feat/camera-fusion`). Fusion is now
default-ON — a plain launch exercises the 2×-inference path.

1. **Build** (later commits may post-date the checked-in binary):
   `cd spatial-screens && make` (and `make test` should stay green).
2. **Free the SDK** — it's single-client. Stop `viture-bridge` and any other
   spatial-screens/bridge process (incl. a parallel `feat/screen-selection`
   session if one is holding the glasses) before launching. Per
   [[hardware-test-division-of-labor]]: on the user's "go", Claude runs all of
   this end-to-end; the user only gives eyes-on-glasses feedback.
3. **Launch (fusion default-ON):** `./run.sh`  →  fallback/compare:
   `./run.sh --no-fusion` (run.sh forwards args). If the 2×-inference send
   backs up and the sidecar trips the 200 ms deadline, gestures get disabled
   mid-session — relaunch with `--no-fusion` to recover the single-inference
   path.
4. **Dashboard (depth numbers):** `cd sensor-viz && npm run dev`, open the URL,
   it connects to `ws://127.0.0.1:8765`; the new **hands card** shows L/R depth
   in meters (`—` when no fused depth).
5. **Observe / the go-no-go:**
   - Depth monotonic near/far (dashboard) + HUD per-hand dot shrinks with
     distance (hand at ~30/50/70 cm).
   - **fps holds with BOTH hands present** — the real risk (drain bounds
     backlog, not per-cycle inference time). If fps collapses with two hands,
     the documented fallback is "send off the render mutex" (design
     §Real-time strategy); do NOT ship default-on until this passes.
   - `--no-fusion` cleanly restores today's behavior (no depth, un-scaled dots).
6. **Environment gotchas** (from memory): inhibit idle blanking during the
   session — [[nvidia-dpms-hang-2026-07-04]] (NVIDIA idle-blank driver hang, not
   ours). A direct-mode crash wedges `DP-1` at `non-desktop=1` — recover with
   `xrandr --output DP-1 --set non-desktop 0 --auto` or replug.
7. **After the pass:** record the verdict here, decide keep-default-on vs
   revert-to-opt-in, update design/plan/roadmap wording to match, then merge to
   `main` (use `superpowers:finishing-a-development-branch`).

## Files (planned — see design §Files touched)

New: `spatial-screens/gestures/stereo.py`, `tests/test_stereo.py`,
`tests/test_hand_tracker.py`. Modified: `hand_tracker.py`, `protocol.py`,
`gesture_client.h`, `gesture_parse.{h,cpp}`, `gesture_parse_test.cpp`,
`main.cpp`, `sensor-viz/`, `classify.py` (optional), `docs/plan/roadmap.md`.

Base: branched off `main`.
