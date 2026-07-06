# Two-Hand Gesture Support — Design

Date: 2026-07-06
Status: approved, pre-implementation

> **⚠️ SUPERSEDED IN PART BY THE HARDWARE PIVOT (2026-07-06).** This document
> describes the *original approved* design: **dual-camera, per-hand** (left hand
> from the left tracking camera, right hand from the right, routed by MediaPipe
> handedness). On-glasses testing killed that approach (stereo parallax doubled
> a near-center hand; 2× inference lag), and the shipped code uses **single-frame
> `num_hands=2`** instead: both hands detected in ONE camera image, split
> left/right by spatial x-position. So the Summary/Architecture/Components below
> (two `HandLandmarker` instances, per-camera streams, handedness routing) are
> the *design intent*, **not what ships**. For the as-built system and the full
> pivot rationale, read `docs/branches/feat-two-hand-gestures.md` and the
> "Future ideas" section at the end of this file. The wire protocol, event
> schema, per-hand arming, grab math, and HUD described here all shipped as-is —
> only the hand-detection/source layer changed.
Branch: `feat/two-hand-gestures`

## Summary

Extend the spatial-screens hand-gesture system from one tracked hand to two.
Either hand can perform the existing single-hand gestures (symmetry), and a new
**two-hand grab** lets you resize and reposition the virtual screen at once. To
give each hand better edge-of-frame coverage, the left hand is sourced from the
left tracking camera and the right hand from the right camera — no stereo
fusion (that is parked as a future idea).

This builds directly on
`docs/specs/2026-07-03-hand-gesture-control-design.md` (the original one-hand
pinch/pose sidecar) and the hand-overlay work; read that first for the socket
topology and wire-protocol rationale.

## Goals

- Track both hands simultaneously.
- Either hand performs today's single-hand gestures: open-palm = arm, fist-hold
  (~0.5 s) = recenter, pinch-drag vertical = screen distance.
- New two-hand grab: with both hands armed and pinching, the **spread** between
  the two pinch points resizes the screen (diagonal inches) and their
  **midpoint** repositions it laterally. Depth stays on the one-handed drag; the
  two complement each other.
- Per-hand arming: each open palm arms *its own* hand.
- Left hand from the left camera, right hand from the right camera.

## Non-goals (v1)

- **No stereo fusion / triangulation** — no depth-from-disparity, no camera
  calibration. See "Future ideas".
- **No rotation** in the two-hand grab — translation + scale only.
- **No depth (z) in the two-hand reposition** — depth stays on the one-handed
  pinch-drag. The two-hand grab moves the screen laterally (in the head's
  right/up plane) and scales it.

## Architecture

Same socket topology as the one-hand system (spatial-screens spawns the Python
sidecar over a Unix domain socket; frames flow down, JSON gesture events flow
up). Three layers change:

```
SDK camera cb ──(L+R gray8 planes)──► sidecar ──(one event, both hands)──► C++ consumer
 on_camera_carina                    2× HandLandmarker                   per-hand state machine
 (forwards BOTH planes)              (L frame → left hand,                + two-hand grab
                                      R frame → right hand)               + two-hand overlay
```

## Components

### 1. Sidecar — `spatial-screens/gestures/hand_tracker.py`

- Build **two** `HandLandmarker` instances. VIDEO running-mode is stateful per
  stream (it tracks temporal state and requires strictly-increasing
  per-stream timestamps), so the left-frame and right-frame streams cannot
  share one landmarker. Each instance keeps its own `last_ts_ms`.
- Each cycle: read one two-plane frame, run the left landmarker on the left
  plane and the right landmarker on the right plane.
- **Handedness routing:** from the left frame keep the user's **left** hand;
  from the right frame keep the user's **right** hand, selecting by MediaPipe's
  reported handedness. Run each landmarker with `num_hands=2` (both hands can
  appear in either camera) and pick the one matching the target handedness; if
  the target handedness is absent in that frame, that hand is reported
  `present:false`.
- **Mirror flag:** MediaPipe's Left/Right labels assume a mirrored (selfie)
  image; the tracking cameras face forward, so the labels are typically
  inverted. A single module-level `MIRROR_HANDEDNESS` constant flips the
  interpretation. Its correct value is pinned during the hardware pass (the
  overlay already carries an analogous mirror note in `main.cpp`).
- Emit one combined event per cycle (schema below).

The slow import/model-load-before-connect ordering from the one-hand design
still applies and must be preserved (building two landmarkers before
`connect()` — see `build_landmarker`'s docstring).

### 2. Frame protocol — `spatial-screens/gestures/protocol.py` + `maybe_send_frame`

Extend the length-prefixed frame header to carry a plane count:

```
[u32 len][f64 timestamp][i32 width][i32 height][u8 format][u8 n_planes][raw bytes × n_planes]
```

- C++ `maybe_send_frame` gains a second image pointer and packs 2 equal-size
  planes (left then right). Its existing partial-write / EAGAIN drain loop is
  unchanged apart from the larger payload (~2×307 KB for 640×480 GRAY8).
- Sidecar `read_frame` returns a list/tuple of planes.
- Both ends are owned and versioned together; no backward compatibility needed.

### 3. Event protocol — `protocol.py` `encode_event` + `gesture_parse.cpp`

One event carries up to two hands as named sub-objects:

```json
{"type":"hand","t":1.5,
 "left":{"present":true,"handedness":"left","pinch_norm":0.30,"pinch_pos":[0.40,0.50],"pose":"open_palm","landmarks":[[x,y], …21]},
 "right":{"present":false}}
```

- An absent hand is emitted as `{"present":false}` (other fields defaulted) so
  the parser always finds the sub-object.
- The hand-rolled C++ parser (deliberately no JSON library, per the original
  design) locates each sub-object's `{ … }` span first, then runs the existing
  key-scanners (`json_find_bool/number/string/pair/landmarks`) *scoped to that
  span*. Same approach, just bounded to a substring per hand.

### 4. C++ consumer — `gesture_client.h` / `gesture_parse.cpp` / `main.cpp`

- `GestureEvent` splits into a reusable `HandState` (present, pinching,
  pinch_x/y, pose, landmarks[21][2], has_landmarks) held twice: `left` and
  `right`. `parse_event` fills both.
- The render-loop gesture block becomes the state machine in the next section.

### 5. Overlay (HUD) — `main.cpp`

- Draw both hands' 21 landmark dots. Per-hand alpha follows *that hand's* armed
  flag; a hand's thumb/index tips go green only when that hand is armed and
  pinching (today's rule, applied per hand).
- Raise the overlay draw cap (currently `ndraw < 32`, and the `draws[]` array
  size) to at least 48 to fit 2×21 dots plus the pinch-status/cursor/screen
  quads.

## Gesture state machine (per render frame)

Per-hand arming, for `h` in {left, right}:

- `!present[h]` → `armed[h] = false`
- `pose[h] == "open_palm"` → `armed[h] = true`
- completing a gesture disarms the hand(s) it used.

Priority arbitration each frame:

1. **Two-hand grab** — if *both* hands are `present && armed && pinching`:
   begin the grab (snapshot state) if not already grabbing, else update it
   (resize + reposition, below). Single-hand logic is suppressed this frame and
   its pinch/fist run-state is reset so it re-seeds cleanly on exit.
2. **Otherwise** — if a grab just ended this frame, disarm **both** hands. Then
   run today's single-hand logic on the first qualifying **armed** hand
   (fist-hold recenter takes priority over pinch-drag distance), identical to
   current behavior.

Safety property: if one hand drops mid-grab, the grab ends and *both* hands
disarm — so the surviving hand cannot lurch into a distance-drag. No jarring
mode switches.

## Grab math (resize + reposition) — pure, unit-tested

Per repo convention (pure math like `classify.py` and `sensor-viz/src/math.js`
is isolated and unit-tested), the grab math lives in a pure function, not inline
in the render loop.

On grab **start**, snapshot:

- `spread0 = |pinchL − pinchR|` (normalized image distance)
- `mid0 = (pinchL + pinchR) / 2`
- `size0` = current screen diagonal (inches)
- `anchor0` = current screen anchor position
- head basis `right0`, `up0` from the current head pose

Each frame during the grab:

- **Resize:** `diag = clamp(size0 · spread/spread0, DIAG_MIN, DIAG_MAX)`.
  Ratio-from-start (not incremental) so it is drift-free. Suggested clamp
  ~20–200 in, tunable.
- **Reposition:** `dm = mid − mid0` (image space; +x right, +y down).
  `anchor = anchor0 + right0·(dm.x·GAIN·distance) − up0·(dm.y·GAIN·distance)`
  (image-y down negated to world-up). `GAIN` maps a normalized-image fraction to
  a world fraction and is scaled by `distance` so the screen tracks the hands at
  its depth. Orientation (`anchor_q`) is held fixed.

Screen distance is untouched during a grab (the one-handed drag owns depth).

## Error handling / risks

- **2× inference cost** (biggest risk). Two MediaPipe passes at 15 Hz may
  overrun the frame budget on hardware. Fallback: one frame with `num_hands=2`
  (both hands from the left camera). Same event schema; a one-line sidecar
  change. Decision is made on the hardware pass by measuring inference latency.
- **Handedness mirror.** Wrong routing swaps the hands. Isolated to
  `MIRROR_HANDEDNESS`; pinned on hardware.
- **Cross-camera parallax on spread.** The two pinch points come from cameras
  ~6 cm apart, so absolute spread is not metric. Using the *ratio* from grab
  start cancels most of it. If resize still feels wrong, source both grab pinch
  points from the left frame during a grab (both hands are center-ish then) —
  noted as a tuning fallback, not built in v1.
- Sidecar-unavailable and socket-backpressure behavior is unchanged: gestures
  remain an optional feature; `GestureClient` reports `enabled() == false` and
  the app runs normally without them.

## Testing

- **Python (pytest):** two-hand event round-trip through `encode_event` /
  `read_frame` in `protocol.py`; the multi-plane frame header; handedness /
  mirror selection logic.
- **C++ (`gesture_parse_test`):** parse events with both hands present, one
  present, and neither — assert per-hand fields and `has_landmarks`.
- **Pure grab math:** unit-test the resize+reposition function directly with
  synthetic pinch points (spread doubling → diag doubling; midpoint shift →
  expected anchor delta).
- **Hardware pass:** confirm the `MIRROR_HANDEDNESS` value, measure two-pass
  inference latency (decides the fallback), and tune grab feel (`GAIN`, clamps)
  and the parallax behavior.

## Future ideas (documented, not built)

- **Camera fusion for depth** (the proper next project; deferred 2026-07-06).
  Fuse the two grayscale cameras to recover each hand's true 3D position from
  stereo disparity. Real payoff: **depth** — push/pull-to-move-in-Z and other
  depth-aware gestures that a single 2D camera can't do robustly (see the
  one-handed distance gesture, which today rides on vertical hand motion because
  single-camera depth is too noisy). It would also inherently deduplicate hands
  (one 3D entity, not one-per-camera).
  **Why it's a real project, not a quick fix:** the VITURE SDK exposes *no*
  stereo calibration — the camera callback hands over raw left/right image
  buffers + width/height and nothing else (no intrinsics, no baseline, no
  relative pose; confirmed in `sdk/include/viture_device_carina.h`). So fusion
  requires **self-calibration** (checkerboard or online stereo calibration),
  then rectification, cross-camera correspondence, and triangulation — a
  multi-day computer-vision effort. It also still runs 2× inference, so it does
  **not** reduce latency. It deserves its own design + calibration spike.
  *Not* the way to fix hand left/right separation — that is solved structurally
  by the **single-frame `num_hands=2`** approach adopted on hardware
  (2026-07-06): detect both hands in ONE camera image so the left/right split
  comes from one consistent x-axis (no cross-camera parallax). See the branch
  doc `docs/branches/feat-two-hand-gestures.md` for that pivot and its evidence.
- **Head-motion compensation in hand reading** (requested 2026-07-06, hardware
  pass). The tracking camera is head-mounted, so when the user moves their head
  a *stationary* hand sweeps across the image — apparent hand motion that is
  really head motion. This adds jitter to gestures and, on the single-frame
  spatial split, can flip a near-center hand between the left/right sides just
  from a head turn. We already stream the 6DoF head pose (same SDK callback,
  `pose ok` in the HUD), so we can subtract the head's angular delta between
  frames from the landmark positions before classifying — stabilising hand
  coordinates in a head-independent frame. Cheap (no extra camera work, reuses
  existing pose), and it directly improves grab steadiness and the
  center-crossing behavior below. Needs the camera's mounting offset/FOV to map
  head rotation → image pixels (approximate constants, tunable on hardware).
- **Single-frame center-crossing / split hysteresis** — observed on hardware
  (2026-07-06): with the single-frame `num_hands=2` spatial split at image
  x=0.5, a *lone* hand moved across the center jumps from the left side/dot to
  the right (it is now physically on the other half). Harmless for the two-hand
  grab (hands stay on their own sides) and for single-hand gestures (symmetric —
  either hand fires), but visually surprising. Optional refinement: add a small
  dead-zone / hysteresis band around x=0.5 so an already-tracked hand keeps its
  side until it clearly crosses, rather than flipping at the exact midpoint.
  Pairs naturally with head-motion compensation above. Left out of v1 by choice.
- **Two-hand rotation** — angle between the two pinch points → screen yaw/roll,
  a natural add on top of the existing grab snapshot.
- **Downscaled grayscale inference** — *investigated on hardware 2026-07-06,
  does NOT help.* MediaPipe HandLandmarker resizes every input to fixed internal
  model resolutions, so feeding it lower-res frames leaves inference time flat
  (measured ~24–27 ms from 640×480 down to 160×120; per-frame preprocessing is
  already ~0.3 ms). Source downscaling only saves socket/copy overhead, which is
  not the bottleneck. **What actually worked:** run the two independent
  landmarker inferences *concurrently* (one thread each) — MediaPipe releases
  the GIL during its C++ graph, measured **1.74×** on hardware (~52 ms → ~30 ms
  per two-plane cycle). **That threading was then REMOVED in the single-frame
  pivot (2026-07-06):** single-frame `num_hands=2` runs ONE inference, so there
  is nothing to parallelize — the sidecar is single-threaded again (~24-27
  ms/cycle). The rest of this bullet is the *superseded* dual-camera narrative,
  kept for the aliasing lesson. It paired the threading with raising
  the C++ sender's rate cap (`GESTURE_INFER_HZ` 15→30): the tracking camera runs
  at ~25 Hz (40 ms grid), and the old 15 Hz cap (66.7 ms) aliased against that
  grid to pass only every *other* frame (~12.5 Hz); a cap above the camera rate
  passes them all. Together, threading + the higher cap took the live gesture
  rate from **12.5 Hz → a stable 25 Hz** on hardware at the time. The single-frame pivot later superseded this whole
  dual-camera path; the shipped sidecar uses ONE inference at the same 15 Hz
  cap (`GESTURE_INFER_HZ`), and raising the cap toward the ~25 Hz camera rate
  is a documented follow-up pending a fresh on-hardware measurement.

## Files touched

- `spatial-screens/gestures/hand_tracker.py` — dual landmarker, two-plane read,
  handedness routing, combined event.
- `spatial-screens/gestures/protocol.py` — multi-plane frame header; two-hand
  event schema.
- `spatial-screens/gestures/classify.py` — reused unchanged; add a pure
  handedness-selection helper if it aids testing.
- `spatial-screens/gestures/tests/` — new two-hand protocol + selection tests.
- `spatial-screens/src/gesture_client.h` — `HandState` × 2 in `GestureEvent`.
- `spatial-screens/src/gesture_parse.cpp` — per-hand sub-object parsing.
- `spatial-screens/src/gesture_parse_test.cpp` — two-hand cases.
- `spatial-screens/src/gesture_client.cpp` — `maybe_send_frame` sends 2 planes.
- `spatial-screens/src/main.cpp` — `on_camera_carina` forwards both planes;
  per-hand arming + mode arbitration + grab math; two-hand overlay + cap.
- `docs/specs/2026-07-06-two-hand-gestures-design.md` — this document.
