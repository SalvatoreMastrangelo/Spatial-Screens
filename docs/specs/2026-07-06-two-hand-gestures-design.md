# Two-Hand Gesture Support — Design

Date: 2026-07-06
Status: approved, pre-implementation
Branch: `feat/two-hand-gestures` (off `master`)

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

- **Stereo fusion** of the two grayscale cameras — true depth from disparity and
  a wider fused FOV. Would enable depth-aware gestures and z-reposition in the
  two-hand grab. Needs calibration and roughly doubles or restructures the
  inference path.
- **Two-hand rotation** — angle between the two pinch points → screen yaw/roll,
  a natural add on top of the existing grab snapshot.
- **Downscaled grayscale inference** — run MediaPipe on lower-resolution
  (downsampled) grayscale frames to cut per-inference cost. Directly targets the
  2× inference-cost risk of dual-camera tracking: hand landmarks are robust to
  moderate downscaling, so halving each dimension (~4× fewer pixels) could
  roughly restore single-camera compute while keeping both hands. Levers to
  explore: downscale in the C++ sender (less socket payload too) vs. in the
  sidecar; find the resolution floor where pinch/pose classification still
  holds; possibly downscale only for detection and keep full-res for the
  landmark refinement. Pairs with, or is an alternative to, the single-frame
  `num_hands=2` fallback in "Error handling / risks".

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
