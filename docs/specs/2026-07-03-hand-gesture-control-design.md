# Hand gesture control for spatial-screens

Design for detecting hand gestures via the Luma Ultra's tracking camera and using
them to drive `spatial-screens` actions (distance, size, recenter). Written
2026-07-03. Elevates the "speculative V3+" hand-tracking bullet in
[`phase2-spatial-screens.md`](../plan/phase2-spatial-screens.md) into a concrete,
scoped design.

## Goal

Build toward real gesture control of the spatial-screens virtual screen — not just
a detection spike. Gestures should replace/augment the existing keyboard hotkeys
(`R` recenter, `[`/`]` distance, `-`/`=` size) with pinch-drag and a discrete pose.

## Context and constraints

- The vendored SDK exposes stereo camera frames via `XRCameraCallback`
  (`sdk/include/viture_device_carina.h`), registered through
  `register_callbacks_carina()`. Nobody has pulled a frame from it yet — pixel
  format, resolution, FOV, and frame rate are all unverified.
- **The SDK allows only one process to hold the device open at a time**
  (`spatial-screens/README.md`: "stop viture-bridge first"). `spatial-screens`
  already owns that connection directly (it links the SDK itself, not through
  `bridge`), so camera capture must live in `spatial-screens`.
- `spatial-screens` currently supports exactly one virtual screen (M2/M3 spike);
  multi-screen switching does not exist yet. Gesture vocabulary is scoped to the
  three actions the app can actually perform today: distance, size, recenter.
- The phase-2 roadmap explicitly treats hand tracking as speculative: "may never
  be exposed on desktop — treat all gesture UX as optional garnish, never a
  dependency." This design preserves that: gesture support must degrade to "off"
  without affecting the renderer if the sidecar is unavailable or crashes.

## Architecture

```
spatial-screens (C++, owns SDK handle)
  ├─ existing: pose callback → screen transform → GL render
  └─ new: camera_callback → forward raw frame over Unix domain socket
                                        │
                                        ▼
                          gestures/ (new Python sidecar, MediaPipe Hands)
                                        │
                          landmarks → gesture/manipulation events
                                        │
                                        ▼
  ← reads latest event each render frame ──── back over the same socket
  └─ applies to screen state (distance/size/recenter) alongside pose
```

New code lives in `spatial-screens/gestures/` (Python sidecar + this IPC
protocol). `spatial-screens/src/main.cpp` gains: camera callback registration, a
UDS client/server role (see below), and a per-frame "poll latest gesture event"
call feeding into the existing screen-transform update path — the same place
pose currently updates it.

This is the first Python in the repo (currently JS + C++17 only per
`CLAUDE.md`) — `CLAUDE.md` needs a note once this is built.

### Inference stack: Python sidecar (MediaPipe Hands), not in-process ONNX

Chosen over embedding ONNX Runtime C++ directly in `spatial-screens`:

- Frame format/FOV/usability is completely unverified. MediaPipe Python
  (`pip install mediapipe opencv-python`) gets from "unknown frame format" to
  "21 hand landmarks on screen" fastest — no model conversion needed.
  MediaPipe's hand landmark model is TFLite; converting to ONNX is unofficial
  and fragile, and would need doing *before* even validating the camera works
  for this at all.
- Hand tracking is explicitly speculative per the roadmap. A sidecar process
  that can be cleanly removed if it doesn't pan out beats a permanent C++ build
  dependency (ONNX Runtime) for a feature that might get abandoned.
- If it proves out and the extra process/IPC hop becomes a real latency
  problem, porting the model to ONNX Runtime C++ is a well-scoped v2 — not
  blocked by anything in this design.

## IPC protocol

**Transport:** `spatial-screens` creates a Unix domain socket (`SOCK_STREAM`) at
a known path and starts listening *before* spawning the sidecar
(`posix_spawn`'ing `python3 gestures/hand_tracker.py --socket <path>`), avoiding
a startup race. It waits briefly (a few seconds) for the sidecar to connect; on
failure (Python/MediaPipe missing, import error, timeout) it logs a warning and
continues running with gestures disabled.

**Frame forwarding (spatial-screens → sidecar), length-prefixed binary:**

```
[u32 len][f64 timestamp][i32 width][i32 height][u8 format][raw bytes]
```

Only `image_left0` (one of the four buffers `XRCameraCallback` provides) is
forwarded in v1 — MediaPipe needs a single 2D image, and pinch detection uses a
2D landmark-distance proxy (see below), so there's no need to ship the full
stereo quad. `format` is a placeholder byte until M1 determines what the SDK
actually hands back. Sends are throttled to a `GESTURE_INFER_HZ` constant
(mirrors `bridge`'s `POSE_SEND_HZ`/`IMU_SEND_HZ` pattern), starting at a guess
of 15 Hz and tuned once M1/M3 show what the sidecar can sustain.

**Gesture events (sidecar → spatial-screens), JSON, one object per processed
frame** (styled like `bridge`'s WS protocol for consistency):

```json
{"type":"hand","t":0.0,"present":true,"handedness":"left",
 "landmarks":[[x,y], ...21 pairs...],
 "pinch_norm":0.0,"pose":"open_palm"}
```

`pose` is one of `"open_palm" | "fist" | "point" | "none"`. `spatial-screens`
does a non-blocking read each render frame, keeps the latest event, and holds
last-known state between inference ticks rather than flickering when no new
event has arrived.

**Failure modes:**

- Sidecar not installed / fails to connect → log once, run with gestures off.
- Sidecar crashes mid-session (socket EOF) → log once, disable gestures for the
  rest of the session. No auto-respawn loop.
- No hand in frame → explicit `present:false`; the renderer stops applying
  gesture-driven transforms that frame rather than guessing from stale data.

## Gesture vocabulary and action mapping

Spatial-screens today has exactly three adjustable things: distance (`[`/`]`),
size (`-`/`=`), recenter (`R`). Depth-from-a-single-camera is unreliable
(foreshortening confounds "hand moved toward the camera" with "hand is just
bigger/smaller in frame"), so instead of detecting push/pull motion directly,
this design uses the visionOS pattern: **pinch is the engagement signal**, and
2D drag-while-pinched drives deltas. Thumb and index touching is rare during
idle hand motion, so it's a naturally low-false-positive gate — no separate
"arm gesture control" gesture is needed.

| Gesture | Action | Replaces |
|---|---|---|
| Pinch + drag vertically | Distance (up = farther, down = closer) | `[` / `]` |
| Pinch + drag horizontally | Size (right = bigger, left = smaller) | `-` / `=` |
| Fist held ~0.5s | Recenter | `R` |
| *(none)* | Quit | stays keyboard-only (`Q`/`Esc`) — not worth false-positive risk on a destructive action |

A single pinch-drag acts as a 2-axis joystick (vertical → distance, horizontal
→ size) while held. The fist dwell time (~0.5s) prevents a passing closed-hand
shape from triggering an accidental recenter. Hotkeys remain the ground-truth
fallback; gestures are additive, not a replacement input path.

**Explicitly out of scope for this design:** multi-screen switch/focus gestures
(blocked on the multi-screen `spatial-screens` work from the phase-2 roadmap,
which doesn't exist yet), two-hand gestures, user-configurable gesture
vocabulary, stereo/depth-based pinch, the ONNX Runtime port.

## Milestones

1. **M1 — Camera probe.** Register `camera_callback` in `spatial-screens` behind
   a `--probe-camera` flag; dump `image_left0` frames to disk (try
   grayscale/PGM first, fall back to raw dump for inspection otherwise). Exit
   criteria: known width/height/format/rate, hardcoded into the IPC `format`
   byte.
2. **M2 — IPC plumbing.** UDS server, spawn/lifecycle/crash-handling from the
   protocol section above; sidecar just echoes frame dimensions back as JSON.
   Exit criteria: frames flow end-to-end; the renderer survives sidecar
   absence or a mid-session crash without disruption.
3. **M3 — Landmarks + classification.** MediaPipe Hands runs in the sidecar
   with `max_num_hands=1` (single-hand tracking only — the first detected hand
   controls the screen; disambiguating multiple visible hands is out of
   scope, see non-goals); compute `pinch_norm` (thumb-tip/index-tip distance
   normalized by palm size) and `pose` (fist/open_palm/none via landmark curl
   heuristics); emit the event schema above. Exit criteria: live
   landmark/pose values logged correctly while moving a hand in front of the
   glasses.
4. **M4 — Wire to actions.** Pinch-drag → distance/size deltas, fist-hold →
   recenter. Exit criteria: resize/redistance/recenter the screen using only
   hand gestures, verified hands-on.

## Testing

No hardware CI, matching the rest of the repo — end-to-end verification is
manual, on hardware. What can be automated without the glasses attached:

- `pytest` smoke tests for the sidecar's pinch/fist classification against
  static sample frames captured during M1.
- A standalone round-trip test for the length-prefixed frame protocol
  (encode/decode, no socket needed).

## Risks

- Frame format/FOV/rate are entirely unknown until M1 — could kill the
  approach outright if the tracking camera doesn't cover hand-height in front
  of the face, or the frame rate is too low to feel responsive.
- MediaPipe's CPU cost stacks on top of VIO's own CPU load, already a flagged
  phase-2 risk (`phase2-spatial-screens.md` risk #3).
- First Python dependency in the repo — `CLAUDE.md` needs a note once this
  ships.
- Pinch/fist thresholds will likely need per-session lighting/hand-size
  tuning; no auto-calibration in v1.
