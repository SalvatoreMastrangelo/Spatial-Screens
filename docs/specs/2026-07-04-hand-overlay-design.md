# Hand-gesture visual feedback: pinch-status dot + hand-landmark overlay

Date: 2026-07-04
Module: `spatial-screens/`

## Goal

Give the wearer live, in-display feedback about what the hand-gesture
pipeline sees, so pinch detection is legible while wearing the glasses:

1. A **pinch-status dot** next to the existing visual-odometry (VO)
   tracking-status dot, showing at a glance whether a hand is seen and
   whether it is pinching.
2. A **hand-landmark overlay** — the 21 MediaPipe hand points drawn as
   dots on the display whenever a hand is visible — so the wearer can
   watch the thumb and index tips converge and understand *why* a pinch
   does or does not trigger.

## Key enabler: the data already exists

The gesture sidecar (`gestures/hand_tracker.py`) already emits every
frame's full 21-point landmark array in its JSON event
(`protocol.py:encode_event`, the `landmarks` field — a list of `[x, y]`
pairs normalized to `[0,1]` image space, x-right, y-down). The C++
consumer (`gesture_client.cpp:parse_event`) simply doesn't parse it
today; it only extracts `present`, `pinch_norm`, `pinch_pos`, `pose`.

Consequences:
- **No Python changes.** No new socket traffic; the bytes are already on
  the wire.
- **No SDK changes.**
- The work is: parse the landmarks C++-side, and draw two new sets of
  quads in the existing head-locked overlay.

The pinch itself is the normalized distance between **landmark 4
(thumb tip)** and **landmark 8 (index tip)**
(`classify.py:pinch_norm`), so those are exactly the two points the
overlay highlights.

## Feature 1 — pinch-status dot

A circle-clipped quad placed just to the left of the VO tracking-status
dot, in the same head-locked eye-space plane
(`main.cpp:857-869`). Persistent (always drawn while the gesture
pipeline is live), with three colors reflecting the latest gesture
event:

| State                        | Color      |
|------------------------------|------------|
| No hand detected             | grey / dim |
| Hand detected, not pinching  | blue       |
| Pinching                     | green      |

Gating: drawn only when `g_gestures.enabled()` is true. If the sidecar
never connected (or has died), the dot is **absent** rather than stuck
grey — a grey dot would falsely imply "pipeline running, no hand seen."

The pinch state uses the raw `gev.pinching` from the event
(`present && pinch_norm < PINCH_THRESHOLD`), independent of the
fist-priority drag logic in the gesture handling block — this is a
"what does the detector see" indicator, not "what action fired."

## Feature 2 — hand-landmark overlay

A small head-locked panel in the lower-left of the view (mirroring the
status dot's lower-right corner), drawn only when
`g_gestures.enabled() && gev.present && gev.has_landmarks`. Contents:

- **21 dots**, one per landmark, circle-clipped quads.
- 19 of them: neutral soft-cyan (visible against the white virtual
  screen).
- **Landmark 4 (thumb tip) and landmark 8 (index tip):** yellow when a
  hand is present, **green while pinching** — the same green as the
  pinch-status dot, so the two feedback channels agree.

### Placement and mapping

- Reuse the status dot's technique: build an eye-space matrix (identity
  rotation, translation placing the panel `DOT_Z` meters in front of the
  eye), then `mvp = proj * eye`.
- **One shared panel `mvp` for all 21 dots.** The quad vertex shader
  (`shaders/quad.vert`) already positions each quad from `rect.xy`
  (center) ± `rect.zw` (half-extent), so each landmark is just a
  different `rect.xy` center and `color` — no per-dot matrix. 21 cheap
  draws, no rotation math.
- Map each normalized landmark `(nx, ny) ∈ [0,1]²` into the panel's local
  extent: `x = panel_cx + (nx - 0.5) * 2 * PW`,
  `y = panel_cy - (ny - 0.5) * 2 * PH` (y negated: image y is down,
  eye-space y is up).
- **Aspect:** `PW = PH * (cam_w / cam_h)` so the hand isn't stretched.
  The tracking-camera frame dimensions are captured into `g_` atomics in
  the camera callback (`on_camera_carina`, where width/height are
  already in hand) and read in the render loop.

### Defaults (easy to tune on hardware)

- **Mirroring:** landmark-x is mapped directly first. If the hand reads
  left-right flipped when worn, it's a one-line x-flip; a comment marks
  the spot.
- **Panel size:** ~10° tall, lower-left. Nudgeable constants.
- **Dot size:** ~0.3–0.5° apparent, matching the status dot's scale.

## Data-flow / code changes

### `src/gesture_client.h`
Extend `GestureEvent`:
```cpp
float landmarks[21][2] = {};  // normalized [0,1] image coords (x-right, y-down)
bool  has_landmarks = false;  // true iff a full 21-point array parsed
```

### `src/gesture_client.cpp`
Add `json_find_landmarks(const std::string&, float out[21][2])`: locate
`"landmarks":[`, then read 21 `[x, y]` pairs with `strtof`, advancing
past brackets/commas. Returns true iff exactly 21 pairs parse. Same
hand-rolled, dependency-free style as the existing `json_find_*`
scanners (a deliberate choice for this small, fixed, both-ends-owned
schema — see the comment at `gesture_client.cpp:28`). Wire it into
`parse_event`, setting `has_landmarks`.

Note: when no hand is seen the sidecar still sends 21 `[0,0]` pairs with
`present=false`, so the overlay is gated on `present`, not merely on
`has_landmarks`.

### `src/main.cpp`
- Capture tracking-camera `width`/`height` into `g_` atomics in
  `on_camera_carina`.
- Grow the fixed overlay buffer `QuadDraw draws[5]` → `draws[32]`
  (worst case: 1 screen + 1 VO dot + 1 pinch dot + 21 landmark dots =
  24). `vkr_draw` already accepts an arbitrary count (`for i in 0..n`),
  so no renderer change is needed.
- After the VO status dot: if `g_gestures.enabled()`, append the
  pinch-status dot (grey/blue/green per state).
- If `g_gestures.enabled() && gev.present && gev.has_landmarks`, append
  the 21 landmark dots via the shared-panel scheme above.

No changes to `vk_renderer.*`, shaders, the sidecar, or the SDK.

## Edge cases

- **Sidecar unavailable / disabled / died:** `enabled()` false → neither
  the pinch-status dot nor the overlay is drawn. Existing behavior
  (gestures are an optional feature) is preserved.
- **No hand:** `present=false` → overlay hidden; pinch-status dot grey.
- **Malformed / short landmarks array:** `has_landmarks=false` → overlay
  skipped; pinch-status dot still works off `present`/`pinching`.
- **Buffer bound:** `draws[32]` covers the worst case with headroom.

## Testing

The project has no C++ test harness for `spatial-screens` (verification
is manual on hardware, per CLAUDE.md). Plan:

- Before building, sanity-check `json_find_landmarks` with a throwaway
  program against a real sample event line captured from the sidecar,
  confirming the 21 pairs parse to the expected values.
- Python unit tests (`gestures/tests/`) remain untouched and green.
- Final verification on the glasses: hand in view shows the dot cloud;
  thumb/index tips are yellow and turn green on pinch; the pinch-status
  dot tracks grey → blue → green; no hand hides the overlay.

## Out of scope (YAGNI)

- Connecting skeleton lines between landmarks (dots only, per the
  request). Would need rotated thin quads (rotation baked into per-bone
  mvp) — deferred unless the dot cloud proves hard to read.
- World-anchoring the hand overlay to the actual hand position. This is
  a head-locked debug/status readout, not AR hand rendering.
- Any numeric pinch-distance / threshold readout.
