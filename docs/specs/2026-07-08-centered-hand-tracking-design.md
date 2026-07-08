# Centered hand tracking — brightening + both-camera detection union

Status: **design approved 2026-07-08** · Branch: `feat/camera-fusion` · 2026-07-08
Depends on: the camera-fusion sidecar (`spatial-screens/gestures/`, dual-inference
already present) and `docs/specs/2026-07-06-camera-fusion-depth-design.md`.

## Problem

Gesture tracking forces the user to hold their hands **offset to the left** and
low; they cannot keep hands **centered with respect to the face**. Measured on
hardware 2026-07-08 (see `docs/branches/feat-camera-fusion.md`, session
2026-07-08):

- Gesture landmarks are taken from `planes[0]` (the **left** tracking camera) only;
  the right camera feeds depth. Coverage is therefore biased to the left camera's
  field → the leftward offset.
- The tracking cameras are wide-fisheye, aimed **down** (~35° pitch); their usable
  zone is low (chest/desk), not at face height.
- The frames are **severely underexposed** (left0 mean **12/255**, 96 % of pixels
  below 30). MediaPipe only detects hands in the small lamp-lit region.
- Measured: a hand **centered at nose = 0 %** detected (136 frames) vs. the user's
  **left-offset = 87 %** detected. Lowering into the lit zone → both cameras track.
- The SDK's second image pair `image_left1`/`image_right1` is **NULL** on this
  device (probed 2026-07-08), so there is no brighter hardware exposure to use.
- The darkness is **recoverable in software**: gamma-0.45 lifts mean 12→59,
  gamma+CLAHE→66, and the whole room resolves from near-black.

## Goal / non-goals

**Goal:** let the user hold hands **centered left-right** at a comfortable
(flexible) height, with a forgiving detection zone. Decided with the user
2026-07-08: height is not a hard requirement; don't fight the downward camera angle.

**Non-goals:** changing the set of gestures; metric depth; face-height tracking;
hardware/lighting changes; the `feat/camera-fusion` merge gate (the both-hands fps
go/no-go is separate and still pending).

## Design overview

Two independent improvements, specced together, validated together on hardware:

1. **Brightening pre-pass** — enhance each grayscale plane before MediaPipe so hands
   are detected across the (currently dark) frame, not just the lit sliver. Simple,
   low-risk, validated offline on real frames.
2. **Both-camera detection union** — track a hand seen by **either** camera (fusion
   already runs a landmarker on both planes every frame; today the right one feeds
   only depth). This centers horizontal coverage. The stereo match dedups a hand
   seen in both cameras into one entity — the fusion-native cure for the
   cross-camera "which side is it" confusion that killed the original two-hand
   dual-camera design.

Both live in the Python sidecar (`hand_tracker.py` + helpers); no wire-protocol
change is required (handedness + per-hand depth already flow). Config keys are
forwarded from `~/.config/spatial-screens.conf` through the C++ `GestureClient`
the same way `--fusion` is.

---

## Part 1 — Brightening pre-pass

### Where
`hand_tracker.py`, in `infer()`, on the `gray` ndarray immediately before
`cv2.cvtColor(gray, GRAY2RGB)`. Factor the transform into a pure, unit-testable
helper (new module `enhance.py`, mirroring `depth_fusion.py`'s "pure math, no I/O"
style):

```python
def enhance(gray: np.ndarray, mode: str, gamma: float, clahe_clip: float) -> np.ndarray
```

`mode ∈ {none, gamma, clahe, gamma_clahe}`. `none` returns the input unchanged.

### What
- **gamma** (default 0.45): LUT `((i/255)**gamma)*255`. Lifts global shadows.
- **clahe** (default clipLimit 3.0, tile 8×8): local contrast; handles the
  lamp-blows-out-while-corners-are-black unevenness.
- **gamma_clahe** (default): gamma then CLAHE — best shadow recovery in the probe.

Applied to **both** planes (left and right) so detection *and* depth benefit.

### Cost / risk
- gamma is a 256-entry LUT; CLAHE on 640×480 GRAY8 is sub-millisecond. Negligible
  vs. MediaPipe inference, even ×2 for fusion. Does **not** materially change the
  both-hands real-time budget (that go/no-go remains a separate, pending test).
- Brightening amplifies sensor noise in near-black regions → possible false
  detections. Mitigation: modest CLAHE clip (≤3), keep it config-gated so it can be
  turned down or off in good light. Tune on hardware.

### Config
Sidecar CLI: `--enhance <mode>` `--enhance-gamma <f>` `--enhance-clahe-clip <f>`,
defaults `gamma_clahe` / 0.45 / 3.0 (the sidecar owns the defaults). The C++ app
forwards these with compile-time defaults, exactly like it forwards `--fusion`
(argv, run.sh-tunable). Persisting them as `spatial-screens.conf` keys
(`gesture-enhance` etc.) is a small deferred follow-up — `--fusion` itself isn't
config-driven either.

---

## Part 2 — Both-camera detection union

### Model: fused hand entities
Per frame, with both landmarkers run (as fusion already does):

- `left_hands`  = detections in the LEFT image  `[(label, landmarks), ...]`
- `right_hands` = detections in the RIGHT image `[(label, landmarks), ...]`

Build a set of **fused hands**, each = `{landmarks (canonical frame), depth|None,
canon_x}`:

1. **Match across cameras** (dedup): for each left detection, find a right detection
   that satisfies the epipolar/stereo constraint — small mean row (y) delta and
   **positive** disparity `mean(xl − xr) > 0` — reusing/generalizing the existing
   `match_right_hand` (in `hand_tracker.py`; consider moving it + the new union
   logic into a pure, testable module alongside `depth_fusion.py`). A matched pair →
   **one** fused hand seen by both (has depth via triangulation).
2. **Left-only** detections (unmatched) → fused hand, no depth, landmarks = left.
3. **Right-only** detections (unmatched) → fused hand, no depth, landmarks = right
   **mapped into the canonical (left) frame** (see below).

This is the confusion fix: a single physical hand visible in both images is matched
in step 1 and counted **once**, never doubled.

### Canonical coordinate frame
Gesture math (pinch position, two-hand spread/midpoint, reposition) runs on
image-normalized coords and today assumes the **left** image. Keep that as the
canonical frame:

- Both-camera and left-only hands: use left landmarks directly (unchanged behavior).
- Right-only hands: shift right landmarks by a **nominal horizontal disparity**
  `d0 = f·B / Z0` (assumed focal `f` from `depth_fusion.focal_norm()`, baseline
  `B = 0.06 m`, nominal gesture depth `Z0 ≈ 0.4 m` → `d0 ≈ 0.11` in normalized x),
  i.e. `x_canon = x_right + d0`. Approximate (true disparity depends on real depth,
  which a right-only hand lacks), but the offset is small and only needs to be good
  enough for handedness and continuity. **Flagged for hardware validation.**

### Handedness
Assign user-left/right by the fused hand's `canon_x` via the existing spatial split
(`classify.select_hand` generalized to operate on the fused set instead of just the
left image), keeping `MIRROR_HANDEDNESS=False`. Handedness is thus decided on the
**union**, so a centered hand the left camera misses but the right camera sees still
gets a stable side.

### Gesture classification
`pinch_norm` and `pose` are relative (finger distances / curls) → camera-invariant,
computed from whichever landmarks the fused hand carries. `pinch_pos` is absolute →
uses the canonical-frame landmarks (left, or shifted right). Depth (when the hand is
seen by both) flows exactly as today.

### Known v1 limitation (documented, to validate)
Two hands each visible in only *different* cameras get positions in the canonical
frame via the nominal-disparity shift, so a two-hand **spread/midpoint** spanning
the two cameras' exclusive zones is approximate. Single-hand gestures and
both-hands-in-overlap are unaffected. If hardware shows this matters, the upgrade is
to carry the full 3-D fused position (one entity in the left-camera metric frame)
and do gesture math in 3-D — deferred (larger refactor).

### Config
Sidecar CLI `--no-both-cam` disables it; default **on** when fusion is on. Forwarded
from the C++ app like `--fusion` (compile-time default on). Requires fusion (needs
the right landmarker); with `--no-fusion` it is inert. Config-key persistence is the
same deferred follow-up as above.

---

## Testing

**Unit (pure, no hardware) — `pytest`:**
- `enhance.py`: `none` is identity; `gamma`/`clahe`/`gamma_clahe` raise mean
  brightness on a synthetic dark image; output dtype/shape preserved; clip bounds.
- Union logic (factor the matching/dedup/canonicalization into pure helpers): a
  hand in both images → one fused hand with depth; left-only and right-only → one
  fused hand each with correct canonical x; a lone hand near center does **not**
  double; handedness assignment across the union.

**Hardware validation (needs a hand; next glasses session):**
1. Brightening: with a hand held in a previously-dark spot, confirm detection where
   it failed before; compare `enhance` modes; watch for noise false-positives.
2. **Finally capture the horizontal coverage map** (the sweep that failed twice —
   use a self-paced protocol, not a timed window): per-camera detection vs.
   horizontal hand position at a fixed comfortable height. Confirms the right camera
   covers center/right and that the union fills the gap.
3. Centering: verify the user can hold hands centered L-R and gesture reliably.
4. Two-hand grab spanning both cameras: check spread/midpoint feel (the v1
   limitation above); decide if the 3-D upgrade is needed.
5. Re-run the still-pending **both-hands fps go/no-go** for the fusion branch with
   brightening + union active (confirm no real-time regression).

## Rollout
Behind config flags, defaults on with fusion. Land brightening and the union in the
same branch but as separate commits so brightening can be validated/kept even if the
union needs iteration. Update `docs/branches/feat-camera-fusion.md` after the
hardware pass. Merge of `feat/camera-fusion` still gated on its own both-hands fps
test.

## Decisions (confirmed with user 2026-07-08)
1. Default `enhance` mode = **`gamma_clahe`** (max recovery; dial down to `clahe` on
   hardware if noise causes false detections).
2. Both-camera union = **on when fusion is on**, with a `--no-both-cam` escape hatch.
3. Right-only-hand coordinates = **v1 nominal-disparity approximation**; upgrade to
   the 3-D-fused-position model only if the two-hands-in-different-cameras case is
   shown to feel wrong on hardware.
