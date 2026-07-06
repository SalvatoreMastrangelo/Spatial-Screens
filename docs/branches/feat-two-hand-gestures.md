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
- [x] **Hardware verification pass — PASSED (2026-07-06, single-frame build).**
      Direct mode on the glasses, whole session stable (fps ~115, 6DoF LIVE, no
      SIGSEGV). User feedback:
    - [x] **`MIRROR_HANDEDNESS` confirmed** (`=False`): a lone hand lights up the
          correct side and only ONE side — no more doubling.
    - [x] **Both hands cleanly separated**, each on its correct side.
    - [x] **Less laggy** than the dual-camera build (one inference, not two).
    - [x] **All functionalities work** (arm, recenter, pinch-distance, two-hand
          grab). Two-hand grab is usable but has a learning curve — "a bit hard,
          need to get used to it." No numeric tuning requested; grab-feel
          constants (`GRAB_REPOSITION_GAIN`, `GRAB_DIAG_MIN/MAX`) left at
          defaults, revisit only if asked.
    - [~] **Known quirk (accepted for v1):** a lone hand moved across image
          center jumps to the other side/dot (spatial split at x=0.5). Harmless
          for two-hand grab (hands stay on their sides) and symmetric single-hand
          gestures. Logged as a future idea (split hysteresis + head-motion
          compensation) in the design doc, not fixed here.
- [x] **Final whole-branch review — done (2026-07-06, opus reviewer).** Verdict
      "ready with fixes": **no Critical, no functional or wire-format issues** —
      pivot code clean (no dead threading refs), C++↔Python wire byte-compatible
      across all three files, per-hand arm / two-hand grab state machine correct
      (both-hands-disarm safety intact), overlay draw bounds safe, tests + HW
      pass green. The two "should-fix-before-merge" items were **doc/comment
      staleness only** and are FIXED: (a) design doc now carries a SUPERSEDED
      pivot banner + the "threaded dual-inference now implemented" claim
      corrected to note it was removed in the pivot; (b) `GESTURE_INFER_HZ`
      comment rewritten for the single-frame reality; (c) `main.cpp` header +
      runtime gesture-help text now describe either-hand + per-hand arm +
      two-hand grab.
- [x] **Integration merge with `main` — done (2026-07-06, commit `3b566ea`).**
      `main` had moved far past the branch point: it absorbed `feat/stereo-3d`
      (multi-screen **rack** model + **stereo SBS** per-eye rendering) and the
      public-release restructure (SDK **fetched not vendored**). Both features
      rewrote the same `main.cpp` gesture/screen/render subsystem, so this was a
      real integration (not a fast-forward): 5 `main.cpp` conflict regions plus
      auto-merged incompatible API changes. Resolved by re-expressing the
      two-hand feature against the rack/stereo model:
    - **Two-hand grab → rack**: reposition moves the rack origin `rack_p`;
      resize sets `diag_in`/`scene[0].cfg.size` in single-screen mode and scales
      `rack_size_scale` uniformly in multi-screen (rack) mode (`grab_scale0`
      snapshot; ratio = `gr.diag/grab.size0`). **NEW v1 behavior — two-hand grab
      in a multi-screen rack + stereo was unspecified by either design.** Chose a
      conservative mapping; **needs hardware confirmation** (esp. rack-mode
      resize feel and stereo per-eye alignment of the grabbed screen).
    - **Single-hand pinch-drag** made multi-aware (`rack_dist_scale` vs
      `distance`+`scene[0].cfg.distance`), matching main's keyboard path;
      dropped the dead `anchor_p` forward-walk; `place_screen`→`place_rack`.
    - **HUD into per-eye `build_eye`**: VO dot → bottom-center; single status
      dot → two per-hand dots (bottom corners); single-hand overlay → two L/R
      landmark panels. All head-locked elements carry `eye_off` (render in both
      stereo eyes). Draw cap `draws[2][40]`→`[2][64]`.
    - Clean build; **19 py + gesture-parse + gesture-manip + stereo-math green**
      (main's stereo tests still pass — no regression).
- [x] **Combined hardware pass (stereo × two-hand) — PASSED (2026-07-06).**
      Launched default `./run.sh` = **stereo SBS + 4-screen 2×2 rack + gesture
      sidecar** (the hardest combined case), direct mode 3840×1200@90, `render:
      stereo (SBS), eye 1920x1200`, `scene: 4 screen(s) (rack)`, sidecar
      connected, HUD `rack-dist/rack-size` live. User verdict: **"everything
      works"** — per-eye HUD (3 dots + overlay in both eyes), two-hand grab on
      the rack (resize/reposition), single-hand gestures, and handedness all
      good. Clean SIGINT shutdown; workspace scaling + SBS panel mode restored,
      0 leftover VS monitors, DP-1 back to non-desktop=0.
- [ ] **Fast-forward `main` — HELD (user decision 2026-07-06).** Branch is
      verified and ready at **`6b5bbb9`** (integration merge `3b566ea` + the
      final-review doc fixes + a docs-only merge-up of main's `5943801`
      roadmap commit). FF is valid (`main` is an ancestor). Held because the
      primary worktree — where `main` is checked out — has **active uncommitted
      work** (a `roadmap.md` edit + 3 new untracked design docs:
      floating-window-screens, screen-selection, vertical-placement). Land it
      once that work is committed/stashed, with:
      `git -C /home/salvatore/Desktop/code/viture merge --ff-only feat/two-hand-gestures`
      The FF delta does not touch `roadmap.md` or the 3 new docs, so it won't
      clobber them — the hold is only to avoid moving `main` under a live
      session. (master deleted 2026-07-06; main is the sole mainline.)

### Review backlog (non-blocking — deferred to follow-up, not fixed pre-merge)

- **1-plane vs 2-plane send + rate cap** (efficiency): C++ still sends BOTH
  camera planes though the single-frame sidecar uses only `planes[0]`
  (~614→307 KB/frame if halved); and `GESTURE_INFER_HZ=15` was chosen for the
  slower dual-camera sidecar. Dropping to one plane and re-measuring the cap
  toward ~25 Hz needs a fresh on-hardware pass — do together.
- **`read_frame` plane-size validation** (`protocol.py`): no check that
  `len(body)` divides evenly by `n_planes` or that `plane_size == width*height`;
  a malformed frame silently truncates (then `reshape` would raise and drop the
  sidecar). Add a guard + a malformed-frame / `n_planes==0` test.
- **Symmetric encode test** (`test_protocol.py`): add a right-present/left-absent
  `encode_event` case (C++ parser side already covers this asymmetry).
- **Defensive null-check** on the second plane in `maybe_send_frame`
  (`gesture_client.cpp`) — Carina is always stereo, so low priority.
- PEP8 2-blank-line nits across Python test files.

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
