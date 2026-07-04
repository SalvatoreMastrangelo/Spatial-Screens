# Handoff: resting/lowering hand false-triggers fist-recenter (future focus)

Date: 2026-07-05
Area: `spatial-screens/gestures/` (pose classification) + `spatial-screens/src/main.cpp` (gesture state machine)
Status: **deferred** — reverted the tuning attempts; documenting for a future focused pass.

## The problem

With the open-palm gesture arming in place (open palm = arm; fist-hold 0.5s =
recenter; pinch-drag = distance), **lowering or resting the hand sometimes
classifies as a fist and fires an unintended recenter.** As you put your hand
down, it passes through — or settles into — a shape the classifier reads as
"all fingers curled" = fist, and if you were still armed, the 0.5s hold elapses
and the screen recenters.

Reported on hardware: "sometimes when I put down the hand it still thinks it's a
fist, so it repositions the screen."

## Why it's hard

Pose classification is done from 21 **2-D** normalized landmarks
(`classify.py`). Distinguishing a *deliberate fist* from a *hand being lowered
or withdrawn* is genuinely ambiguous in 2-D:

- A tilting/lowering hand **foreshortens** the fingers, so fingertips project
  close to the wrist and look "curled" even when the hand is open/relaxed.
- **Fist and pinch share the closed-hand shape.** The fist test is "all four
  non-thumb fingers curled"; pinch is "thumb-tip↔index-tip distance small."
  Making the fist test stricter pushes a real clench *out* of "fist" and it then
  falls through to being read as a **pinch** (thumb and index are close in a
  fist) — which is exactly what happened when we tightened it.

So there is a direct tension: looser fist detection → occasional false fists on
rest; stricter fist detection → missed fists and fist↔pinch confusion.

## What was tried (and reverted)

1. **Curl margin** (`classify.py:_finger_curled`): require the tip to be inside
   the PIP by a factor (`_CURL_MARGIN`), 0.7 then 0.65, so only a clear curl
   counts. At 0.7 it reduced the false trigger "a bit" but did not eliminate it.
   At 0.65 (plus #2) it started **misreading a deliberate fist as a pinch**.
2. **Longer fist-hold** (`FIST_HOLD_SECONDS` 0.5 → 0.8s in `main.cpp`): to reject
   the transient fist-shape during the put-down motion. Contributed to the
   fist↔pinch confusion and made the intentional recenter feel harder.

Both were reverted. Current state: original simple curl test, 0.5s hold. The
fist works reliably again; the occasional resting-hand false-recenter remains a
**known issue**. Everything else stayed: open-palm arming, 4-state status dot,
overlay transparency, and the thin (~0.35°) dots.

## Suggested approaches for the focused pass

Roughly in order of expected value:

1. **Capture real data first — don't keep guessing.** Add temporary logging in
   the sidecar (or `main.cpp`) that dumps the 21 landmarks + each finger's
   tip/pip curl ratio + `pinch_norm` at the instant a fist-recenter is about to
   fire. Do a hardware run, deliberately trigger a few false fists, and design
   the fix from the actual geometry. Every attempt above failed because it was
   tuned blind.
2. **Resolve fist vs pinch explicitly.** They overlap. Make them mutually
   exclusive: if `pinch_norm` is small (thumb+index together), prefer *pinch*;
   only treat a closed hand as *fist* when it is clearly **not** a pinch (thumb
   tucked / all tips clustered near the palm, not thumb-to-index).
3. **Thumb-fold discriminator.** A deliberate fist tucks the thumb; a resting
   hand often splays it. Add a thumb-position requirement to the fist test —
   calibrated from the data in #1.
4. **Leaving-frame gating.** A hand being put down drifts toward the frame
   bottom/edge and/or its bounding box shrinks. Have the sidecar expose the hand
   bbox / detection confidence and suppress actionable poses when the hand is
   near the image boundary or shrinking.
5. **Temporal stability.** Require the fist pose to be stable for N consecutive
   frames before the hold timer starts, so a transient shape during the put-down
   never accumulates.
6. **Arm-timeout.** Disarm if armed without a deliberate gesture within a short
   window — helps the "armed, then rested" case (though not an immediate
   transient, since the resting hand reads as an active pose right away).
7. **Consider MediaPipe's gesture recognizer** (trained `Closed_Fist` /
   `Open_Palm` / `Pointing_Up` classes) instead of the hand-rolled 2-D curl
   heuristic — likely more robust than geometric thresholds, at some latency/dep
   cost.

## Pointers

- Classifier: `spatial-screens/gestures/classify.py` (`_finger_curled`,
  `classify_pose`, `pinch_norm`); unit tests `gestures/tests/test_classify.py`
  (`python3 -m pytest gestures/tests/ -v`).
- Consumer/state machine: `spatial-screens/src/main.cpp`, the `// ---- gestures`
  block (arming, fist-hold, pinch-drag) and `FIST_HOLD_SECONDS`.
- Design context: `docs/specs/2026-07-04-hand-overlay-design.md` and the
  `feat/hand-overlay` branch history.
