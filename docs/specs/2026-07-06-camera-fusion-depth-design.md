# Camera Fusion for Depth — per-hand 3D via stereo triangulation — Design

Date: 2026-07-06
Status: approved, pre-implementation
Branch: `feat/camera-fusion` (worktree `.claude/worktrees/camera-fusion`, branched from `main`)

> **Independent track.** This does *not* build on the per-screen-selection
> dependency chain (roadmap items #1–#3) and does *not* block it. The only shared
> surface with `feat/screen-selection` is the `main.cpp` gesture block and the
> Python `classify.py`/tests — a textual merge-conflict risk resolved at
> integration, not a functional dependency. See the obstruction analysis in the
> roadmap backlog (item #4). Surfaced by the two-hand gestures feature's "Future
> ideas" (`docs/specs/2026-07-06-two-hand-gestures-design.md`).

## Summary

Recover each hand's true 3D position by fusing the glasses' two grayscale
tracking cameras via stereo triangulation, and publish the resulting **per-hand
depth** (plus per-landmark z) on the existing gesture wire and dashboard
telemetry.

**v1 is a data channel, not a gesture.** No new gesture behavior ships here. The
push/pull-to-move-in-Z gesture that this depth unlocks is a deliberate follow-up
branch that *consumes* this channel. Shipping depth as an observable, verifiable
signal first lets us prove depth quality on hardware before committing to any
gesture design on top of it.

## Scope decisions (from brainstorming, 2026-07-06)

- **Deliverable:** real per-hand depth on the wire + dashboard. No gesture. (Not
  a throwaway spike, not a full depth-gesture feature — the middle increment.)
- **Calibration:** assumed intrinsics (focal length from camera FOV) + assumed
  ~6 cm baseline (measured on the two-hand hardware pass). **No calibration
  ritual, no checkerboard in v1.** Depth is roughly scaled (reliable *near/far*,
  not trustworthy metric cm). Checkerboard self-calibration is a documented
  future upgrade that slots into `stereo.py` with no caller change.
- **Depth method:** landmark triangulation, **2× inference** — run MediaPipe on
  both the left and right images, match each physical hand across the pair, and
  triangulate corresponding landmarks into 3D. (Chosen over 1× disparity-map
  lookup for full per-landmark 3D.)
- **Real-time strategy:** **B — threaded dual inference at full cadence**, with
  the safeguards below.

## Non-goals (v1)

- **No depth-driven gesture** (push/pull-Z) — the follow-up branch.
- **No checkerboard / online self-calibration** — assumed params only; future.
- **No dense stereo (StereoSGBM) disparity** — landmark triangulation only.
- **No use of the SDK's second stereo pair** (`image_left1`/`image_right1`, see
  Architecture) — only `image_left0`/`image_right0` are fused. Investigating the
  second pair is a future idea.

## Preconditions / current pipeline

- `feat/two-hand-gestures` and `feat/stereo-3d` merged to `main` (this branches
  from `main` @ `80e3a23`). The current sidecar runs **one** `HandLandmarker`
  (`num_hands=2`) on the **left** plane only and splits hands by image
  x-position (`classify.select_hand`); depth today is faked from vertical hand
  motion because single-camera depth is too noisy. See
  `docs/branches/feat-two-hand-gestures.md`.
- The C++ side already forwards **both** camera planes every frame:
  `on_camera_carina(image_left0, image_right0, …)` →
  `GestureClient::maybe_send_frame(left, right, …)` sends `n_planes=2`
  (`planes[0]`=left/`image_left0`, `planes[1]`=right/`image_right0`). The sidecar
  currently ignores `planes[1]`. **This branch consumes `planes[1]`.**
  → **Dependency note:** the two-hand review backlog's "drop `planes[1]` to
  halve bandwidth" optimization must **NOT** be applied — fusion needs both
  planes. This design supersedes that backlog item.

The SDK camera callback (`sdk/include/viture_device_carina.h`) delivers raw
buffers only — `XRCameraCallback(image_left0, image_right0, image_left1,
image_right1, timestamp, width, height)` — with **no intrinsics, no baseline, no
extrinsics**. Hence assumed-parameter (or, later, self-) calibration is
mandatory. Note it hands over *two* stereo pairs (frame 0 and frame 1); only
pair 0 is used.

## Architecture — additive, zero regression to the working gesture path

The single-frame **left**-camera path stays exactly as-is: it remains the source
of truth for hand identity (user-left / user-right x-split), pose, pinch, and
every existing gesture. Fusion *adds* a second inference on the right plane whose
landmarks are used **only** to triangulate depth for the hands the left path
already found and labelled.

```
                ┌─ planes[0] LEFT  → HandLandmarker_L(num_hands=2) → x-split → hands[L/R]
 frame          │                                          (identity/pose/pinch/gesture — UNCHANGED)
 (2 planes) ────┤                                                     │
                └─ planes[1] RIGHT → HandLandmarker_R(num_hands=2) ─┐  │
                                                                    ▼  ▼
                                          match_hands(left_hand, right_hands)   ← per physical hand
                                          (handedness + rectified-row / epipolar proximity)
                                                        │ matched landmark pairs (xL,yL)/(xR,yR)
                                                        ▼
                                     stereo.triangulate(assumed K, baseline) → per-landmark 3D
                                                        ▼
                             per-hand depth = robust aggregate (median Z)  (+ landmarks_z[])
                                                        ▼
                                        encode_event(... depth, landmarks_z)   (null when unavailable)
```

`match_hands` matches the authoritative left-image hand to a right-image hand,
not the reverse — the left path owns identity. Matching key: handedness label
plus vertical (row) proximity, which under a nominally-rectified horizontal
stereo pair should be near-equal for corresponding landmarks. Disparity is then
the horizontal offset `xL − xR`.

## Real-time strategy — B: threaded dual inference, full cadence

**The hazard, precisely.** `GestureClient::maybe_send_frame` runs under `mutex_`
— the same mutex the render loop's `poll()` takes — and pushes a ~614 KB
two-plane frame (2 × 640×480 GRAY8) rate-capped to `GESTURE_INFER_HZ = 15`. The
socket send buffer is ~208 KB, so a frame spans multiple `send()` calls; on
`EAGAIN` it `poll(POLLOUT)`s against a single **200 ms aggregate deadline** and,
if the sidecar hasn't drained by then, sets `enabled_ = false` — **gesture
control is disabled for the rest of the process**. So a sidecar that stops
reading for too long (e.g. blocked in a slow 2× inference) both (a) stalls the
render loop via the shared mutex and (b) can permanently kill gestures. This is
exactly what sank the first dual-camera two-hand build.

**Three safeguards make B safe:**

1. **Latest-frame-wins drain (sidecar) — the piece that was missing.** Before
   each inference the sidecar drains the socket to the newest complete frame and
   discards any backlog (non-blocking `select`/`recv` until it would block,
   keeping only the last frame). The socket buffer is emptied every loop, so the
   C++ `send()` completes fast and never approaches its 200 ms deadline. This
   turns "sender out-drives sidecar → permanent disable" into "sender out-drives
   sidecar → sidecar silently skips stale frames," which is benign for a depth
   channel.
2. **Two persistent threaded landmarkers.** `HandLandmarker_L` and
   `HandLandmarker_R` each run on their own worker thread, launched per frame and
   joined before matching → ~1.74× single-inference wall time (the measured
   figure), not 2×. Each landmarker keeps its own monotonic VIDEO-mode timestamp
   sequence.
3. **Re-measured rate cap.** `GESTURE_INFER_HZ` re-tuned on hardware for the
   dual-inference + triangulation cost (the two-hand review backlog already
   flagged this re-measure). Start at the hardware-proven 15 Hz; only raise with
   a fresh on-glasses measurement.

**Fallback if the drain proves insufficient on hardware** (documented, not built
in v1): move the C++ `send()` off the render-loop mutex onto a dedicated sender
thread with a single-slot "latest frame" mailbox, so backpressure can never
touch the render loop. Preferred only if safeguard #1 doesn't hold at 15 Hz with
both hands present.

**Acknowledged risk:** this is the configuration that stalled before. The
hardware pass **must** specifically confirm fps holds with **both hands
present** (the case that broke), not just idle.

## Components

### Python sidecar (all CV lives here)

**1. `spatial-screens/gestures/stereo.py` (new) — pure, unit-testable.**
- Assumed camera model: `K` (focal `f` in px from FOV + width, principal point
  at image center), baseline `B` (~0.06 m). Treats the pair as nominally
  rectified/parallel (a strong assumption made explicit; the checkerboard
  upgrade replaces it with real intrinsics + rectification maps).
- `triangulate(pts_left, pts_right) -> pts_3d`: for matched landmark pairs,
  either the closed form `Z = f·B / (xL − xR); X = (xL−cx)·Z/f; Y = (yL−cy)·Z/f`,
  or equivalently `cv2.triangulatePoints(P_left, P_right, …)` with
  `P_left = K[I|0]`, `P_right = K[I|t]`, `t = (−B,0,0)`. Returns `None`/skips
  landmarks with non-positive disparity (diverged / behind).
- `robust_depth(pts_3d) -> float`: median Z over valid landmarks (robust to a
  few bad triangulations), or `None` if too few valid.
- Params live in a module constant now; `load_calibration(path)` reading a
  `stereo_calib.yml` is the future hook (absent → assumed params).

**2. `spatial-screens/gestures/hand_tracker.py` — wiring + threading.**
- Build a **second** `HandLandmarker` for the right plane (mirror of
  `build_landmarker`, own instance).
- Run both inferences on two threads, join (safeguard #2).
- `match_hands(left_hand, right_hands) -> right_hand|None` (safeguard: handedness
  + rectified-row proximity; extract as a pure helper for tests).
- Triangulate matched landmarks via `stereo.py`, attach `depth` + `landmarks_z`
  to the hand dict.
- **Latest-frame-wins drain** in the read loop (safeguard #1).
- Behind a **`--fusion` flag / config**; when off, or `planes` < 2, or no right
  match → behaves exactly as today (depth `None`). Degrades to the single-frame
  path with zero behavior change.

**3. `spatial-screens/gestures/protocol.py` — wire schema (additive).**
- `_hand_obj` / `encode_event` gain per-hand `"depth": float|null` and
  `"landmarks_z": [float,…]|null` (aligned index-for-index with the existing
  `landmarks` array). Backward-compatible: a consumer that ignores them is
  unaffected; `read_frame`/`encode_frame` (the C++→sidecar direction) are
  unchanged.

`classify.py` is expected **unchanged** (pose is still classified from left-image
2D landmarks); a small palm-centroid pixel helper may be added if useful for
choosing the aggregate depth point.

### C++ consumer + UI

**4. `spatial-screens/src/gesture_client.h` / `gesture_parse.{h,cpp}`.**
- `HandState` gains `bool has_depth = false; float depth = 0.f;` (and optionally
  a `landmarks_z` array parallel to the existing landmarks). Parsed additively in
  `gesture_parse.cpp` via the existing hand-rolled `json_find_*` scanners (add a
  `json_find_depth` and, if carried, a `json_find_landmarks_z`).

**5. `spatial-screens/src/main.cpp` — surfacing only, no gesture change.**
- HUD: a small per-hand depth readout near the existing per-hand dots (drawn in
  both stereo eyes via the existing `eye_off` mechanism).
- WS telemetry: include per-hand `depth` in the JSON already streamed to the
  phase-1 dashboard on 8765.
- Keep forwarding both planes (already does; see the dependency note above).

**6. `sensor-viz/` dashboard — per-hand depth readout** in the telemetry panel
(small additive display of the new WS field).

## Data flow / wire schema change

- **Frame (C++ → sidecar):** unchanged — already `n_planes=2`.
- **Event (sidecar → C++):** each `left`/`right` hand sub-object gains `depth`
  and `landmarks_z`. `null` whenever fusion is off, `planes[1]` is absent, the
  right hand isn't matched, or triangulation is degenerate.

## Error handling / degradation

- Fusion off · `planes` < 2 · right hand unmatched · non-positive disparity /
  too few valid landmarks → `depth = null`; **everything else behaves exactly as
  today.** Never a hard dependency (mirrors the existing "gestures are optional"
  rule — sidecar absent ⇒ app runs fine).
- Wrong assumed params → depth mis-scaled but monotonic near/far. Documented;
  the checkerboard upgrade is the fix.
- Right-inference worker throws/crashes → catch, log once, disable fusion for
  the session, continue the single-frame gesture path (no process death).
- Timestamp monotonicity per landmarker preserved (VIDEO mode requirement).

## Testing

- **`stereo.py` (pytest):** synthetic 3D points → project into both assumed
  cameras (`P_left`, `P_right`) → recover via `triangulate` → assert within
  tolerance; degenerate cases (zero/negative disparity → skipped); `robust_depth`
  median behavior + too-few-valid → `None`.
- **`match_hands` (pytest):** two hands correctly paired L↔R; one hand present;
  no plausible match → `None`; handedness-mirror consistency.
- **Drain (pytest):** feed a fake reader several queued frames → assert only the
  newest is processed (latest-frame-wins).
- **`protocol.py` (pytest):** `encode_event` round-trip with `depth`/
  `landmarks_z` present and `null`.
- **C++ `gesture_parse_test`:** parse events with and without `depth` (and
  `landmarks_z` if carried); absent → `has_depth == false`.
- **Hardware pass:** hand held at ~30 / 50 / 70 cm → HUD + dashboard depth
  increases monotonically and is roughly right; **fps and gesture feel unchanged
  with both hands present** (proves safeguards #1–#3 hold); toggling `--fusion`
  off returns to today's behavior.

## Future ideas (documented, not built)

- **Checkerboard offline calibration** — a `calibrate.py` capturing checkerboard
  pairs → `cv2.calibrateCamera` + `cv2.stereoCalibrate` → `stereo_calib.yml` with
  real intrinsics + extrinsics + rectification maps; `stereo.py` loads it when
  present. Upgrades rough-scaled depth to trustworthy metric cm. Cameras are
  rigidly mounted, so one calibration holds across sessions.
- **Online self-calibration** — estimate the essential matrix from natural
  feature matches, anchor scale with the known baseline; no board.
- **Dense disparity (StereoSGBM)** as an alternative/adjunct to landmark
  triangulation, sampled at the palm ROI.
- **Depth-driven gestures** — push/pull-to-move-in-Z and other depth-aware
  interactions; the follow-up branch that consumes this channel. Inherent hand
  de-duplication (one 3D entity, not one-per-camera) also becomes possible.
- **Second stereo pair** — the SDK also delivers `image_left1`/`image_right1`;
  investigate what it is (temporal? exposure?) and whether it helps.
- **Downscaled inference / send** — downscaling planes before send would cut both
  bandwidth and inference time (noted as a two-hand future idea), easing the
  real-time budget if the rate cap proves tight.
- **Send off the render mutex** — the strategy-B fallback (dedicated sender
  thread + latest-frame mailbox) if the drain is insufficient at 15 Hz.

## Files touched

- `spatial-screens/gestures/stereo.py` — **new**: assumed-param model +
  triangulation + robust depth.
- `spatial-screens/gestures/hand_tracker.py` — second landmarker, threading,
  `match_hands`, triangulation wiring, latest-frame-wins drain, `--fusion` gate.
- `spatial-screens/gestures/protocol.py` — `depth` / `landmarks_z` in
  `encode_event` / `_hand_obj`.
- `spatial-screens/gestures/tests/test_stereo.py` — **new**.
- `spatial-screens/gestures/tests/test_protocol.py` — depth round-trip.
- `spatial-screens/gestures/tests/test_hand_tracker.py` — **new**: `match_hands`
  + drain (pure-logic slices).
- `spatial-screens/gestures/classify.py` — optional palm-centroid helper.
- `spatial-screens/src/gesture_client.h` — `HandState` depth fields.
- `spatial-screens/src/gesture_parse.{h,cpp}` — parse `depth` / `landmarks_z`.
- `spatial-screens/src/gesture_parse_test.cpp` — depth parse cases.
- `spatial-screens/src/main.cpp` — HUD depth readout + WS telemetry field.
- `sensor-viz/` — dashboard per-hand depth readout.
- `docs/specs/2026-07-06-camera-fusion-depth-design.md` — this document.
- `docs/branches/feat-camera-fusion.md` — branch resume doc.
- `docs/plan/roadmap.md` — backlog item #4 (added).
