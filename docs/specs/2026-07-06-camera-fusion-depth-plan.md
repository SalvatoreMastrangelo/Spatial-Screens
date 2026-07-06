# Camera Fusion for Depth — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Publish rough-scaled per-hand depth (meters) on the gesture wire and dashboard telemetry by triangulating hand landmarks across the glasses' two tracking cameras — a data channel, no new gesture.

**Architecture:** Keep the single-frame LEFT-camera MediaPipe path (identity/pose/gesture) untouched. Add a second inference on the RIGHT plane; match each authoritative left-image hand to its right-image counterpart; triangulate matched landmarks under an assumed-rectified stereo model; publish the robust (median-Z) per-hand depth. Threaded dual inference (strategy B) guarded by a latest-frame-wins sidecar drain so the C++ sender never trips its render-loop-mutex-held 200 ms send deadline.

**Tech Stack:** Python 3 (MediaPipe Tasks, NumPy, OpenCV — all already deps; the depth math itself is pure stdlib `math`/`statistics`), C++17 (hand-rolled JSON scanners, Vulkan quad HUD), WebSocket telemetry, vanilla-JS sensor-viz dashboard.

## Global Constraints

- **No new dependencies.** `depth_fusion.py` uses only stdlib (`math`, `statistics`) — the assumed-rectified closed form needs no OpenCV. (OpenCV enters only with the future checkerboard upgrade.)
- **Compact JSON separators — no space after `:` or `,`.** The C++ hand-rolled scanners require it; `encode_event` uses `json.dumps(..., separators=(",",":"))`. Pinned by `test_encode_event_uses_compact_separators_the_cpp_parser_requires`.
- **v1 wire carries per-hand `depth` (float) only, omitted when `None`.** `landmarks_z` (per-landmark z) is deferred to a future increment — the internal triangulation still computes per-landmark 3D to derive the median, but only the aggregate crosses the wire (keeps the C++ parser to one `json_find_number`).
- **Additive + degrade-to-off.** Depth absent ⇒ `has_depth == false` ⇒ zero behavior change. Fusion is **opt-in via `--fusion`** in v1 (default runtime path unchanged until the hardware pass validates it). Sidecar absent / plane 1 missing / no right-hand match / degenerate disparity ⇒ `depth = None`.
- **Both camera planes must keep flowing.** The two-hand review-backlog "drop `planes[1]`" optimization must NOT be applied — fusion consumes `planes[1]`.
- **Image coordinates are MediaPipe normalized `[0,1]`** (x right, y down) everywhere in the Python depth code.
- Python tests: `cd spatial-screens && python3 -m pytest gestures/tests/ -v`. C++ tests: `cd spatial-screens && make test` (runs `gesture-parse-test`, `gesture-manip-test`, `stereo-math-test`).

---

### Task 1: `depth_fusion.py` — assumed-rectified triangulation + robust depth

Pure math, no MediaPipe/I/O — fully unit-testable with synthetic coordinates. This is the CV core; everything else is plumbing.

**Files:**
- Create: `spatial-screens/gestures/depth_fusion.py`
- Test: `spatial-screens/gestures/tests/test_depth_fusion.py`

**Interfaces:**
- Consumes: nothing (stdlib only).
- Produces:
  - `focal_norm(hfov_deg=ASSUMED_HFOV_DEG) -> float`
  - `triangulate(xl, yl, xr, yr, hfov_deg=..., baseline_m=...) -> tuple[float,float,float] | None` — `(X,Y,Z)` meters in the left-camera frame, `None` if disparity ≤ `MIN_DISPARITY`.
  - `robust_depth(pts_left, pts_right, hfov_deg=..., baseline_m=...) -> float | None` — median Z over index-aligned valid landmark pairs, `None` if fewer than `MIN_VALID_LANDMARKS` valid.
  - Constants `ASSUMED_HFOV_DEG=70.0`, `BASELINE_M=0.06`, `MIN_DISPARITY=1e-3`, `MIN_VALID_LANDMARKS=6`.

- [ ] **Step 1: Write the failing test**

Create `spatial-screens/gestures/tests/test_depth_fusion.py`:

```python
import math

from depth_fusion import (
    BASELINE_M, MIN_VALID_LANDMARKS, focal_norm, robust_depth, triangulate,
)


def _project(X, Y, Z, hfov_deg=70.0, baseline_m=BASELINE_M):
    """Forward model: where a 3D point (left-camera frame) lands in each
    normalized image. Left cam at origin, right cam at +baseline along X."""
    f = focal_norm(hfov_deg)
    xl = X * f / Z + 0.5
    yl = Y * f / Z + 0.5
    xr = (X - baseline_m) * f / Z + 0.5
    return xl, yl, xr, yl  # rectified: right shares the row (yr == yl)


def test_triangulate_recovers_known_point():
    xl, yl, xr, yr = _project(0.10, -0.05, 0.60)
    p = triangulate(xl, yl, xr, yr)
    assert p is not None
    X, Y, Z = p
    assert abs(Z - 0.60) < 1e-3
    assert abs(X - 0.10) < 1e-3
    assert abs(Y - (-0.05)) < 1e-3


def test_nearer_point_has_larger_disparity_and_smaller_Z():
    near = _project(0.0, 0.0, 0.30)
    far = _project(0.0, 0.0, 0.90)
    disp_near = near[0] - near[2]
    disp_far = far[0] - far[2]
    assert disp_near > disp_far          # nearer => bigger disparity
    assert triangulate(*near)[2] < triangulate(*far)[2]


def test_triangulate_nonpositive_disparity_returns_none():
    # xr >= xl => disparity <= 0 (diverged / behind).
    assert triangulate(0.5, 0.5, 0.5, 0.5) is None
    assert triangulate(0.4, 0.5, 0.6, 0.5) is None


def test_robust_depth_medians_valid_landmarks():
    pts_l, pts_r = [], []
    for i in range(21):
        xl, yl, xr, yr = _project(0.01 * i, 0.0, 0.50)
        pts_l.append((xl, yl))
        pts_r.append((xr, yr))
    d = robust_depth(pts_l, pts_r)
    assert d is not None
    assert abs(d - 0.50) < 1e-2


def test_robust_depth_too_few_valid_returns_none():
    # All pairs have zero disparity => none valid.
    pts = [(0.5, 0.5)] * 21
    assert robust_depth(pts, pts) is None
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spatial-screens && python3 -m pytest gestures/tests/test_depth_fusion.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'depth_fusion'`.

- [ ] **Step 3: Write minimal implementation**

Create `spatial-screens/gestures/depth_fusion.py`:

```python
"""Stereo depth fusion for the gesture sidecar — pure math, no MediaPipe/I/O,
fully unit-testable with synthetic coordinates.

Recovers a hand's depth (meters, rough-scaled) by triangulating the same
landmark seen in the two tracking cameras. The VITURE SDK exposes NO stereo
calibration, so v1 ASSUMES a nominally-rectified horizontal stereo pair (same
intrinsics, parallel optical axes): focal length from an assumed FOV, a fixed
baseline. Depth is reliable near/far but not trustworthy metric cm — see
docs/specs/2026-07-06-camera-fusion-depth-design.md. The checkerboard upgrade
replaces this assumed model with no change to callers.

Image coordinates are MediaPipe normalized [0,1] (x right, y down).
"""
import math
import statistics

ASSUMED_HFOV_DEG = 70.0    # tracking-camera horizontal FOV (approx; tune on hw)
BASELINE_M = 0.06          # ~6 cm inter-camera baseline (two-hand hardware pass)
MIN_DISPARITY = 1e-3       # normalized-x units; at/below this => invalid
MIN_VALID_LANDMARKS = 6    # need this many valid points for a robust median


def focal_norm(hfov_deg=ASSUMED_HFOV_DEG):
    """Focal length in normalized-x units for an assumed horizontal FOV
    (pinhole: half-width 0.5 subtends half the FOV)."""
    return 0.5 / math.tan(math.radians(hfov_deg) / 2.0)


def triangulate(xl, yl, xr, yr, hfov_deg=ASSUMED_HFOV_DEG, baseline_m=BASELINE_M):
    """Triangulate one landmark from its normalized positions in the left/right
    rectified images. Returns (X, Y, Z) meters in the left-camera frame, or
    None if disparity <= MIN_DISPARITY (diverged / behind).

    Left cam at origin, right cam at +baseline along X: disparity
    d = xl - xr = f*B/Z, so Z = f*B/d."""
    d = xl - xr
    if d <= MIN_DISPARITY:
        return None
    f = focal_norm(hfov_deg)
    Z = f * baseline_m / d
    X = (xl - 0.5) * Z / f
    Y = (yl - 0.5) * Z / f
    return (X, Y, Z)


def robust_depth(pts_left, pts_right, hfov_deg=ASSUMED_HFOV_DEG, baseline_m=BASELINE_M):
    """Median Z over all index-aligned landmark pairs that triangulate validly.
    Returns meters, or None if fewer than MIN_VALID_LANDMARKS are valid.

    pts_left/pts_right are equal-length lists of (x, y) normalized pairs — the
    SAME landmark index in each image."""
    zs = []
    for (xl, yl), (xr, yr) in zip(pts_left, pts_right):
        p = triangulate(xl, yl, xr, yr, hfov_deg, baseline_m)
        if p is not None:
            zs.append(p[2])
    if len(zs) < MIN_VALID_LANDMARKS:
        return None
    return statistics.median(zs)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd spatial-screens && python3 -m pytest gestures/tests/test_depth_fusion.py -v`
Expected: PASS (5 passed).

- [ ] **Step 5: Commit**

```bash
git add spatial-screens/gestures/depth_fusion.py spatial-screens/gestures/tests/test_depth_fusion.py
git commit -m "feat(fusion): assumed-rectified stereo triangulation + robust depth"
```

---

### Task 2: `protocol.py` — add per-hand `depth` to the wire (additive)

**Files:**
- Modify: `spatial-screens/gestures/protocol.py` (`_hand_obj`, lines 47-58)
- Test: `spatial-screens/gestures/tests/test_protocol.py`

**Interfaces:**
- Consumes: hand dicts may now carry an optional `"depth": float` key.
- Produces: `encode_event` emits `"depth":<float>` inside a present hand's sub-object **only when the hand dict has a non-None `depth`**; omits it otherwise.

- [ ] **Step 1: Write the failing test**

Append to `spatial-screens/gestures/tests/test_protocol.py`:

```python
def test_encode_event_includes_depth_when_present():
    lm = [(0.1, 0.2)] * 21
    left = {"present": True, "handedness": "left", "pinch_norm": 0.3,
            "pinch_pos": (0.15, 0.2), "pose": "fist", "landmarks": lm,
            "depth": 0.523}
    obj = _decode(encode_event(1.0, left, None))
    assert obj["left"]["depth"] == 0.523
    # compact, no space — the C++ scanner requires it
    assert b'"depth":0.523' in encode_event(1.0, left, None)


def test_encode_event_omits_depth_when_absent_or_none():
    lm = [(0.1, 0.2)] * 21
    base = {"present": True, "handedness": "left", "pinch_norm": 0.3,
            "pinch_pos": (0.15, 0.2), "pose": "fist", "landmarks": lm}
    assert "depth" not in _decode(encode_event(1.0, base, None))["left"]
    withnone = dict(base, depth=None)
    assert "depth" not in _decode(encode_event(1.0, withnone, None))["left"]
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spatial-screens && python3 -m pytest gestures/tests/test_protocol.py -v -k depth`
Expected: FAIL — `KeyError: 'depth'` (key not emitted yet).

- [ ] **Step 3: Write minimal implementation**

In `spatial-screens/gestures/protocol.py`, replace `_hand_obj` (lines 47-58) with:

```python
def _hand_obj(h):
    """One hand's sub-object. h is a dict (see encode_event) or None."""
    if h is None or not h.get("present", False):
        return {"present": False}
    obj = {
        "present": True,
        "handedness": h.get("handedness", ""),
        "pinch_norm": h["pinch_norm"],
        "pinch_pos": list(h["pinch_pos"]),
        "pose": h["pose"],
        "landmarks": [list(p) for p in h["landmarks"]],
    }
    # Fused stereo depth (meters), when available. Omitted (not null) when the
    # sidecar couldn't triangulate, so the C++ scanner's key-presence check
    # (has_depth) stays simple. Emitted AFTER landmarks — still no nested
    # braces, so hand_object()'s first-'}' termination holds.
    depth = h.get("depth")
    if depth is not None:
        obj["depth"] = depth
    return obj
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd spatial-screens && python3 -m pytest gestures/tests/test_protocol.py -v`
Expected: PASS (all existing + 2 new).

- [ ] **Step 5: Commit**

```bash
git add spatial-screens/gestures/protocol.py spatial-screens/gestures/tests/test_protocol.py
git commit -m "feat(fusion): carry optional per-hand depth on the gesture wire"
```

---

### Task 3: C++ `gesture_parse` — parse `depth` into `HandState`

**Files:**
- Modify: `spatial-screens/src/gesture_client.h` (`HandState`, lines 16-23)
- Modify: `spatial-screens/src/gesture_parse.cpp` (`parse_hand`, lines 100-110)
- Test: `spatial-screens/src/gesture_parse_test.cpp`

**Interfaces:**
- Consumes: event JSON that may contain `"depth":<float>` in a hand sub-object.
- Produces: `HandState.has_depth` (bool) and `HandState.depth` (float, meters).

- [ ] **Step 1: Write the failing test**

In `spatial-screens/src/gesture_parse_test.cpp`, add depth to the `both` event's left hand and assert. Change the left-hand literal (line 29) to include depth after landmarks, and add checks after line 45:

```cpp
    // left carries fused depth; right does not.
    std::string both =
        "{\"type\":\"hand\",\"t\":1.5,"
        "\"left\":{\"present\":true,\"handedness\":\"left\",\"pinch_norm\":0.12,"
        "\"pinch_pos\":[0.4,0.5],\"pose\":\"point\",\"landmarks\":" + lm21(0.10f) + ",\"depth\":0.523},"
        "\"right\":{\"present\":true,\"handedness\":\"right\",\"pinch_norm\":0.90,"
        "\"pinch_pos\":[0.6,0.7],\"pose\":\"open_palm\",\"landmarks\":" + lm21(0.30f) + "}}";
```

Then add, alongside the other left/right checks (after line 45):

```cpp
    CHECK(ev.left.has_depth == true);
    CHECK(std::fabs(ev.left.depth - 0.523f) < 1e-4f);
    CHECK(ev.right.has_depth == false);   // right sent no depth
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spatial-screens && make gesture-parse-test && ./gesture-parse-test`
Expected: FAIL — `has_depth`/`depth` are not members yet → compile error.

- [ ] **Step 3: Write minimal implementation**

In `spatial-screens/src/gesture_client.h`, add two fields to `HandState` (after line 22, `has_landmarks`):

```cpp
    bool has_landmarks = false;  // true iff a full 21-point array parsed
    bool has_depth = false;      // true iff the sidecar sent a fused stereo depth
    float depth = 0.f;           // meters, rough-scaled (see camera-fusion design)
};
```

In `spatial-screens/src/gesture_parse.cpp`, add one line to `parse_hand` (after line 108, `h.has_landmarks = ...`):

```cpp
    h.has_landmarks = json_find_landmarks(obj, h.landmarks);
    h.has_depth = json_find_number(obj, "depth", h.depth);  // absent => false, depth stays 0
    return h;
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd spatial-screens && make gesture-parse-test && ./gesture-parse-test`
Expected: `gesture_parse_test: all checks passed`.

- [ ] **Step 5: Commit**

```bash
git add spatial-screens/src/gesture_client.h spatial-screens/src/gesture_parse.cpp spatial-screens/src/gesture_parse_test.cpp
git commit -m "feat(fusion): parse per-hand depth into HandState"
```

---

### Task 4: `hand_tracker.py` pure helpers — match, drain, fuse (unit-tested)

The testable glue that keeps Task 5's `run_inference` thin. All three functions are module-level and import-safe (they don't touch MediaPipe/cv2, which `hand_tracker` imports lazily inside functions).

**Files:**
- Modify: `spatial-screens/gestures/hand_tracker.py` (add module-level helpers + top imports)
- Test: `spatial-screens/gestures/tests/test_hand_tracker.py` (new)

**Interfaces:**
- Consumes: `depth_fusion.robust_depth` (Task 1).
- Produces:
  - `match_right_hand(left_lm, right_hands, max_row_delta=0.15) -> list | None` — the right-image landmarks matching the authoritative left-image hand.
  - `drain_to_latest(read_frame_fn, readable_fn) -> frame | None` — newest buffered frame, discarding backlog.
  - `fuse_depths(user_hands, right_hands) -> dict` — `{"left": depth|None, "right": depth|None}`.

- [ ] **Step 1: Write the failing test**

Create `spatial-screens/gestures/tests/test_hand_tracker.py`:

```python
from hand_tracker import drain_to_latest, fuse_depths, match_right_hand


def _hand(x0, y0):
    # 21 landmarks in a small diagonal cluster.
    return [(x0 + i * 0.004, y0 + i * 0.003) for i in range(21)]


def _shift(lm, dx, dy=0.0):
    return [(x + dx, y + dy) for x, y in lm]


def test_match_right_hand_picks_positive_disparity_same_row():
    left = _hand(0.60, 0.50)
    match = _shift(left, -0.05)              # xr = xl - 0.05  (disparity +0.05), same row
    decoy = _shift(left, -0.05, dy=0.40)     # right row => far off
    got = match_right_hand(left, [("a", decoy), ("b", match)])
    assert got == match


def test_match_right_hand_rejects_negative_disparity():
    left = _hand(0.60, 0.50)
    diverged = _shift(left, +0.05)           # xr > xl => disparity < 0
    assert match_right_hand(left, [("a", diverged)]) is None


def test_match_right_hand_empty_returns_none():
    assert match_right_hand(_hand(0.5, 0.5), []) is None


def test_fuse_depths_matched_left_only():
    left = _hand(0.60, 0.50)
    match = _shift(left, -0.05)
    out = fuse_depths({"left": left, "right": None}, [("x", match)])
    assert out["left"] is not None and out["left"] > 0
    assert out["right"] is None


def test_fuse_depths_no_right_hands():
    left = _hand(0.60, 0.50)
    out = fuse_depths({"left": left, "right": None}, [])
    assert out == {"left": None, "right": None}


def test_drain_to_latest_returns_newest():
    frames = iter([("f1",), ("f2",), ("f3",), None])
    readable = iter([True, True, False])
    assert drain_to_latest(lambda: next(frames), lambda: next(readable, False)) == ("f3",)


def test_drain_to_latest_single_when_not_readable():
    frames = iter([("f1",)])
    assert drain_to_latest(lambda: next(frames, None), lambda: False) == ("f1",)


def test_drain_to_latest_eof_returns_none():
    assert drain_to_latest(lambda: None, lambda: False) is None
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spatial-screens && python3 -m pytest gestures/tests/test_hand_tracker.py -v`
Expected: FAIL — `ImportError: cannot import name 'drain_to_latest'`.

- [ ] **Step 3: Write minimal implementation**

In `spatial-screens/gestures/hand_tracker.py`, extend the top imports (after line 18, `from protocol import ...`):

```python
import statistics

from protocol import encode_event, read_frame
from depth_fusion import robust_depth
```

Then add these module-level helpers (place them just above `def run_echo`, ~line 89):

```python
def match_right_hand(left_lm, right_hands, max_row_delta=0.15):
    """Given the authoritative LEFT-image hand landmarks and the RIGHT-image
    detections [(handedness, landmarks), ...], return the right-image landmarks
    of the SAME physical hand, or None.

    Under a nominally-rectified horizontal stereo pair, corresponding landmarks
    share ~the same image row (y) and the right-image x is < the left-image x
    (positive disparity). Pick the right hand with the smallest mean |dy| that
    also has positive mean disparity, within max_row_delta."""
    best, best_dy = None, max_row_delta
    for _label, rlm in right_hands:
        dy = statistics.mean(abs(l[1] - r[1]) for l, r in zip(left_lm, rlm))
        disp = statistics.mean(l[0] - r[0] for l, r in zip(left_lm, rlm))
        if disp <= 0:
            continue
        if dy < best_dy:
            best, best_dy = rlm, dy
    return best


def fuse_depths(user_hands, right_hands):
    """user_hands: {"left": lm|None, "right": lm|None} — authoritative, from the
    LEFT image. right_hands: [(handedness, lm), ...] from the RIGHT image.
    Returns {"left": depth|None, "right": depth|None} (meters)."""
    out = {}
    for side, llm in user_hands.items():
        if llm is None or not right_hands:
            out[side] = None
            continue
        rlm = match_right_hand(llm, right_hands)
        out[side] = robust_depth(llm, rlm) if rlm is not None else None
    return out


def drain_to_latest(read_frame_fn, readable_fn):
    """Read frames until the socket would block, returning the newest (or None
    on EOF). readable_fn() -> bool says whether another frame is buffered.

    Prevents backlog: a slow 2x inference can't accumulate a queue that fills
    the socket buffer and trips the C++ sender's 200 ms send deadline (which
    would permanently disable gesture control). Stale frames are silently
    dropped — benign for a depth channel."""
    frame = read_frame_fn()
    if frame is None:
        return None
    while readable_fn():
        nxt = read_frame_fn()
        if nxt is None:
            break
        frame = nxt
    return frame
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd spatial-screens && python3 -m pytest gestures/tests/test_hand_tracker.py -v`
Expected: PASS (8 passed).

- [ ] **Step 5: Commit**

```bash
git add spatial-screens/gestures/hand_tracker.py spatial-screens/gestures/tests/test_hand_tracker.py
git commit -m "feat(fusion): pure hand-match, frame-drain, and depth-fuse helpers"
```

---

### Task 5: `hand_tracker.py` — wire the second landmarker + threading + `--fusion`

Integration code: needs MediaPipe, so its real verification is the hardware pass. The testable logic already lives in Task 4; this task keeps `run_inference` a thin wrapper around it. Verified here by: all pytest green (unchanged behavior when fusion off) + import smoke.

**Files:**
- Modify: `spatial-screens/gestures/hand_tracker.py` (`run_inference` ~lines 142-195, `main` ~lines 198-216)

**Interfaces:**
- Consumes: `drain_to_latest`, `fuse_depths` (Task 4); `build_landmarker` (existing).
- Produces: a `--fusion` CLI flag; `run_inference(sock, read_exact, landmarker, landmarker_r=None, fusion=False)`.

- [ ] **Step 1: Replace `run_inference` (lines 142-195)**

```python
def run_inference(sock, read_exact, landmarker, landmarker_r=None, fusion=False):
    import select as _select
    from concurrent.futures import ThreadPoolExecutor

    import cv2
    import mediapipe as mp
    import numpy as np
    from classify import classify_pose, pinch_norm, pinch_pos, select_hand

    def hand_dict(lm, handedness, depth=None):
        return {
            "present": True,
            "handedness": handedness,
            "pinch_norm": pinch_norm(lm),
            "pinch_pos": pinch_pos(lm),
            "pose": classify_pose(lm),
            "landmarks": lm,
            "depth": depth,
        }

    def infer(landmarker_obj, plane, width, height, ts):
        gray = np.frombuffer(plane, dtype=np.uint8).reshape(height, width)
        rgb = cv2.cvtColor(gray, cv2.COLOR_GRAY2RGB)
        mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
        result = landmarker_obj.detect_for_video(mp_image, ts)
        return [(result.handedness[i][0].category_name, _landmarks_to_pairs(lm))
                for i, lm in enumerate(result.hand_landmarks)]

    def readable():
        return bool(_select.select([sock], [], [], 0)[0])

    do_fusion = fusion and landmarker_r is not None
    pool = ThreadPoolExecutor(max_workers=2) if do_fusion else None
    last_ts_l = last_ts_r = -1
    while True:
        frame = drain_to_latest(lambda: read_frame(read_exact), readable)
        if frame is None:
            break
        timestamp, width, height, fmt, planes = frame
        if fmt != FORMAT_GRAY8 or len(planes) < 1:
            print(f"hand_tracker: unexpected format {fmt}/{len(planes)} planes, "
                  f"skipping", file=sys.stderr)
            continue

        # detect_for_video needs a strictly-increasing timestamp PER landmarker.
        ts_l = max(int(timestamp * 1000), last_ts_l + 1)
        last_ts_l = ts_l
        stereo = do_fusion and len(planes) >= 2
        if stereo:
            ts_r = max(int(timestamp * 1000), last_ts_r + 1)
            last_ts_r = ts_r
            fut_r = pool.submit(infer, landmarker_r, planes[1], width, height, ts_r)
            hands = infer(landmarker, planes[0], width, height, ts_l)
            try:
                right_hands = fut_r.result()
            except Exception as e:  # right inference is best-effort; never kill gestures
                print(f"hand_tracker: right-eye inference failed ({e}); depth off "
                      f"this frame", file=sys.stderr)
                right_hands, stereo = None, False
        else:
            hands = infer(landmarker, planes[0], width, height, ts_l)
            right_hands = None

        left_lm = select_hand(hands, "left")
        right_lm = select_hand(hands, "right")
        depths = (fuse_depths({"left": left_lm, "right": right_lm}, right_hands)
                  if stereo else {"left": None, "right": None})
        left = hand_dict(left_lm, "left", depths["left"]) if left_lm is not None else None
        right = hand_dict(right_lm, "right", depths["right"]) if right_lm is not None else None
        sock.sendall(encode_event(timestamp, left, right))
```

- [ ] **Step 2: Update `main` (lines 198-216) to add `--fusion` and the right landmarker**

```python
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", required=True)
    parser.add_argument("--echo", action="store_true",
                        help="skip MediaPipe; just acknowledge frames (IPC smoke test)")
    parser.add_argument("--fusion", action="store_true",
                        help="enable stereo depth fusion (2nd inference on the right plane)")
    args = parser.parse_args()

    # Build the (slow-to-import/load) landmarker(s) before connecting — see
    # build_landmarker()'s docstring for why ordering matters.
    landmarker = None if args.echo else build_landmarker()
    landmarker_r = build_landmarker() if (args.fusion and not args.echo) else None

    sock = connect(args.socket)
    read_exact = make_reader(sock)

    if args.echo:
        run_echo(sock, read_exact)
    else:
        run_inference(sock, read_exact, landmarker, landmarker_r, args.fusion)
```

- [ ] **Step 3: Verify the whole suite is still green (fusion-off path unchanged)**

Run: `cd spatial-screens && python3 -m pytest gestures/ -v`
Expected: PASS — all `test_classify`, `test_protocol`, `test_depth_fusion`, `test_hand_tracker` green.

- [ ] **Step 4: Import smoke test**

Run: `cd spatial-screens/gestures && python3 -c "import hand_tracker; print('ok')"`
Expected: `ok` (module imports without MediaPipe present at import time).

- [ ] **Step 5: Commit**

```bash
git add spatial-screens/gestures/hand_tracker.py
git commit -m "feat(fusion): threaded dual-inference + latest-frame drain behind --fusion"
```

---

### Task 6: C++ — plumb `--fusion` through to the sidecar spawn

**Files:**
- Modify: `spatial-screens/src/gesture_client.h` (`start` signature, line 36)
- Modify: `spatial-screens/src/gesture_client.cpp` (argv build, lines 62-69)
- Modify: `spatial-screens/src/main.cpp` (CLI parse ~line 279, `start` call line 575)

**Interfaces:**
- Consumes: a `--fusion` process CLI flag.
- Produces: `GestureClient::start(..., bool fusion=false)` that appends `--fusion` to the spawned sidecar's argv.

- [ ] **Step 1: Widen `start()`'s signature**

In `spatial-screens/src/gesture_client.h`, line 36-37:

```cpp
    bool start(const std::string& socket_path, const std::string& script_path,
               double connect_timeout_s = 5.0, bool fusion = false);
```

- [ ] **Step 2: Append `--fusion` to argv**

In `spatial-screens/src/gesture_client.cpp`, update the signature (line 41-42) to match and replace the fixed `argv[]` (lines 62-69) with a vector:

```cpp
bool GestureClient::start(const std::string& socket_path, const std::string& script_path,
                           double connect_timeout_s, bool fusion) {
```

```cpp
    std::vector<char*> argv = {
        const_cast<char*>("python3"),
        const_cast<char*>(script_path.c_str()),
        const_cast<char*>("--socket"),
        const_cast<char*>(socket_path.c_str()),
    };
    if (fusion) argv.push_back(const_cast<char*>("--fusion"));
    argv.push_back(nullptr);
    int rc = posix_spawnp(&child_pid_, "python3", nullptr, nullptr, argv.data(), environ);
```

Add `#include <vector>` near the top of `gesture_client.cpp` if not already present.

- [ ] **Step 3: Parse `--fusion` in `main.cpp` and pass it to `start`**

In `spatial-screens/src/main.cpp`, in the flag loop starting line 279 (add a case alongside the other `--` flags), introduce a `bool fusion = false;` before the loop and:

```cpp
        if (!strcmp(a, "--fusion")) { fusion = true; continue; }
```

Then update the `start` call at line 575:

```cpp
    g_gestures.start(gesture_socket, executable_dir() + "/gestures/hand_tracker.py", 5.0, fusion);
```

- [ ] **Step 4: Verify it builds and existing tests pass**

Run: `cd spatial-screens && make test`
Expected: all three unit binaries print their pass lines (`gesture_parse_test: all checks passed`, etc.). A full `make` should also link `spatial-screens` without error.

- [ ] **Step 5: Commit**

```bash
git add spatial-screens/src/gesture_client.h spatial-screens/src/gesture_client.cpp spatial-screens/src/main.cpp
git commit -m "feat(fusion): --fusion CLI flag plumbed to the sidecar spawn"
```

---

### Task 7: Surface depth — telemetry `send_hands` + depth-scaled HUD dot

The in-glasses HUD is quad-only (no glyph renderer), so the numeric value goes to the dashboard and the glasses get a cheap **depth-scaled per-hand dot** (nearer hand ⇒ bigger dot). Verified on hardware + dashboard.

**Files:**
- Modify: `spatial-screens/src/telemetry.h` (declare `send_hands`, add `last_hands_ms_`)
- Modify: `spatial-screens/src/telemetry.cpp` (implement `send_hands`)
- Modify: `spatial-screens/src/main.cpp` (call `send_hands` near line 1236; scale the dot at line 1073)

**Interfaces:**
- Consumes: `GestureEvent`'s `has_depth`/`depth` (Task 3).
- Produces: WS message `{"type":"hands","left_present":bool,"left_depth":float,"right_present":bool,"right_depth":float}` where depth `< 0` means "no depth this frame".

- [ ] **Step 1: Declare `send_hands` in `telemetry.h`**

After the `send_app` declaration (line 32) add:

```cpp
    // Per-hand presence + fused stereo depth (meters); depth < 0 = none.
    void send_hands(bool left_present, bool left_has_depth, float left_depth,
                    bool right_present, bool right_has_depth, float right_depth); // <= 10 Hz
```

And add to the private timestamp members (line 45):

```cpp
    long last_hello_ms_ = 0, last_pose_ms_ = 0, last_app_ms_ = 0, last_hands_ms_ = 0;
```

- [ ] **Step 2: Implement `send_hands` in `telemetry.cpp`**

Append after `send_app` (after line 72):

```cpp
void Telemetry::send_hands(bool lp, bool l_has, float ld, bool rp, bool r_has, float rd) {
    if (!enabled_) return;
    long t = now_ms();
    if (t - last_hands_ms_ < 100) return;   // <= 10 Hz
    last_hands_ms_ = t;
    char buf[256];
    snprintf(buf, sizeof(buf),
             R"({"type":"hands","left_present":%s,"left_depth":%.3f,"right_present":%s,"right_depth":%.3f})",
             lp ? "true" : "false", l_has ? ld : -1.0f,
             rp ? "true" : "false", r_has ? rd : -1.0f);
    ws_.broadcast(buf);
}
```

- [ ] **Step 3: Call it once per frame in `main.cpp`**

After the `tele.send_app(...)` call (ends ~line 1237), add:

```cpp
        tele.send_hands(gev.left.present, gev.left.has_depth, gev.left.depth,
                        gev.right.present, gev.right.has_depth, gev.right.depth);
```

- [ ] **Step 4: Scale the per-hand HUD dot by depth in `main.cpp`**

Replace the dot-rect line (line 1073) inside the per-hand dot loop with a depth-scaled radius:

```cpp
                        float pr = dot_r;
                        if (h.has_depth && h.depth > 0.05f) {
                            // Nearer hand -> bigger dot (cheap in-glasses depth
                            // cue; the HUD has no glyphs). 0.5 m reads nominal.
                            float s = 0.5f / h.depth;
                            if (s < 0.6f) s = 0.6f; else if (s > 1.8f) s = 1.8f;
                            pr = dot_r * s;
                        }
                        pd.rect[0] = 0; pd.rect[1] = 0; pd.rect[2] = pr; pd.rect[3] = pr;
```

- [ ] **Step 5: Build**

Run: `cd spatial-screens && make`
Expected: links `spatial-screens` with no errors/warnings. (`make test` still green — no unit surface changed.)

- [ ] **Step 6: Commit**

```bash
git add spatial-screens/src/telemetry.h spatial-screens/src/telemetry.cpp spatial-screens/src/main.cpp
git commit -m "feat(fusion): stream per-hand depth telemetry + depth-scaled HUD dot"
```

---

### Task 8: sensor-viz dashboard — per-hand depth readout

**Files:**
- Modify: `sensor-viz/src/drivers/bridge-ws.js` (`_handle` switch, ~line 138)
- Modify: the component/store that already consumes the `'app'` event (mirror its `'app'` subscription for `'hands'`)

**Interfaces:**
- Consumes: WS `{"type":"hands", left_present, left_depth, right_present, right_depth}`.
- Produces: an emitted `'hands'` app-event and a small on-screen readout (`left_depth`/`right_depth`, `< 0` shown as `—`).

- [ ] **Step 1: Emit the `hands` message from the driver**

In `sensor-viz/src/drivers/bridge-ws.js`, add a case before `default:` (line 142):

```javascript
    case 'hands':
      // spatial-screens fused per-hand depth (meters); depth < 0 = none.
      this._emit('hands', {
        t,
        left: { present: msg.left_present, depth: msg.left_depth },
        right: { present: msg.right_present, depth: msg.right_depth },
      });
      break;
```

- [ ] **Step 2: Verify the driver forwards it (no unknown-msg log)**

Run: `cd sensor-viz && npm run lint`
Expected: clean (eslint passes on the new case).

- [ ] **Step 3: Add the readout**

Find where the `'app'` event is consumed:

Run: `cd sensor-viz && grep -rn "'app'\|\"app\"" src/ | grep -v drivers`

Mirror that subscription for `'hands'`: add a small readout (two rows, "L" / "R") that shows `depth.toFixed(2) + ' m'` when `present && depth >= 0`, else `—`. Follow the exact store/component pattern the `'app'` consumer uses (same subscribe call, same render idiom) — do not invent a new state mechanism.

- [ ] **Step 4: Verify in the browser**

Run: `cd sensor-viz && npm run dev`
Expected: dashboard loads; with no device the readout shows `—`/`—`. (Live values are confirmed in the hardware pass below.)

- [ ] **Step 5: Commit**

```bash
git add sensor-viz/src/drivers/bridge-ws.js sensor-viz/src/
git commit -m "feat(fusion): dashboard per-hand depth readout"
```

---

## Hardware verification pass (post-implementation, on-glasses)

Not a code task — the go/no-go for the feature. Launch with fusion on:

```bash
cd spatial-screens && ./run.sh --fusion     # (ensure run.sh forwards extra args; else ./spatial-screens --fusion ...)
```

Confirm, per the design's testing section:
1. **Depth monotonicity:** hold a hand at ~30 / 50 / 70 cm → dashboard `left_depth`/`right_depth` increases with distance and is roughly right; the HUD per-hand dot shrinks as the hand moves away.
2. **No regression / no stall — the critical check:** with **both hands present**, fps holds (target ~90–115 as on prior passes) and gestures feel unchanged. This is the exact case that stalled the first dual-camera build; if fps collapses, the latest-frame drain (safeguard #1) is insufficient → apply the documented fallback (send off the render mutex) before shipping.
3. **Degrade-to-off:** relaunch WITHOUT `--fusion` → identical to today's behavior, no depth in telemetry, dots un-scaled.

Record the result in `docs/branches/feat-camera-fusion.md` (Current state / next step), then use `superpowers:finishing-a-development-branch` to merge to `main`.

---

## Self-Review

**Spec coverage:**
- Assumed-param triangulation → Task 1. ✓
- `stereo.py` renamed `depth_fusion.py` (avoids clash with C++ `src/stereo.h`) — noted in Global Constraints. ✓
- Wire `depth` field → Tasks 2 (Python) + 3 (C++). `landmarks_z` explicitly deferred (Global Constraints) — a scoped reduction of the design's "optional" field. ✓
- `match_hands` + latest-frame-wins drain + threaded dual inference → Tasks 4 (pure) + 5 (integration). ✓
- `--fusion` gate → Task 5 (Python) + 6 (C++ plumbing). ✓
- HUD + telemetry surfacing → Task 7; dashboard → Task 8. HUD realized as a depth-scaled dot (quad-only HUD, no glyphs) — reconciliation noted. ✓
- Error handling / degrade-to-off → Task 5 (try/except right inference), Tasks 2/3 (omit/absent depth), Task 6 (opt-in flag). ✓
- Keep-both-planes dependency → Global Constraints. ✓
- Testing (stereo, match, drain, protocol round-trip, C++ parse, hardware both-hands fps) → Tasks 1-4 unit tests + hardware pass. ✓

**Placeholder scan:** none — every code step has literal code; Task 8 Step 3 gives a bounded "mirror the `'app'` consumer" instruction with a grep to locate it (not a vague "add UI").

**Type consistency:** `match_right_hand`/`fuse_depths`/`drain_to_latest`/`robust_depth`/`triangulate`/`focal_norm` signatures identical across Tasks 1, 4, 5. `HandState.has_depth`/`depth` identical across Tasks 3, 7. `send_hands` param order identical in Tasks 7 declaration/impl/call. Wire key `"depth"` consistent Tasks 2↔3; `"type":"hands"` fields consistent Tasks 7↔8.
