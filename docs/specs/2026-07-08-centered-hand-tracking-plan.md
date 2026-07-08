# Centered Hand Tracking Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the user hold hands centered left-right (at a flexible height) by brightening the dark camera frames before MediaPipe and tracking a hand seen by *either* tracking camera.

**Architecture:** All logic lives in the Python sidecar (`spatial-screens/gestures/`). Two new pure modules — `enhance.py` (image brightening) and `fuse_hands.py` (cross-camera match/dedup/canonicalize/handedness) — are unit-tested in isolation, then wired into `hand_tracker.py`. The C++ app forwards two knobs (`--enhance`, `--no-both-cam`) to the sidecar exactly as it forwards `--fusion`. No wire-protocol change.

**Tech Stack:** Python 3, OpenCV (`cv2`), NumPy, MediaPipe (sidecar); C++17 (`spatial-screens`). Tests: `pytest`.

**Design of record:** `docs/specs/2026-07-08-centered-hand-tracking-design.md`.

## Global Constraints

- Pure modules (`enhance.py`, `fuse_hands.py`) must have **no MediaPipe/socket/I-O** imports — `cv2`/`numpy`/stdlib only — so `pytest` runs them headless.
- Both-camera union **requires fusion** (needs the right landmarker). With `--no-fusion` it is inert; the single-camera path is unchanged.
- Canonical coordinate frame = the **left** image. Right-only detections are shifted `+d0` in normalized x, where `d0 = focal_norm()·BASELINE_M / NOMINAL_DEPTH_M`.
- Keep `MIRROR_HANDEDNESS = False` (pinned on hardware).
- Wire protocol (`protocol.py encode_event`) is unchanged; per-hand `landmarks`/`depth`/`handedness` already flow.
- Sidecar owns the defaults: `enhance = gamma_clahe`, `gamma = 0.45`, `clahe_clip = 3.0`, `both_cam = on` (when fusion on). C++ forwards these as argv with matching compile-time defaults.
- Run the sidecar tests with: `cd spatial-screens/gestures && python3 -m pytest tests/ -v`.

---

### Task 1: `enhance.py` — brightening pre-pass (pure)

**Files:**
- Create: `spatial-screens/gestures/enhance.py`
- Test: `spatial-screens/gestures/tests/test_enhance.py`

**Interfaces:**
- Produces: `make_enhancer(mode="gamma_clahe", gamma=0.45, clahe_clip=3.0) -> Callable[[np.ndarray], np.ndarray]`; module constant `MODES = ("none","gamma","clahe","gamma_clahe")`. The returned callable maps a `uint8` HxW grayscale array to an enhanced `uint8` HxW array.

- [ ] **Step 1: Write the failing test**

```python
# spatial-screens/gestures/tests/test_enhance.py
import numpy as np
import pytest
from enhance import make_enhancer, MODES


def _dark():  # 64x64 image resembling the hardware frames (mean ~12/255)
    rng = np.random.default_rng(0)
    return (rng.random((64, 64)) * 24).astype(np.uint8)


def test_none_is_identity():
    img = _dark()
    out = make_enhancer("none")(img)
    assert np.array_equal(out, img)


def test_gamma_brightens():
    img = _dark()
    out = make_enhancer("gamma", gamma=0.45)(img)
    assert out.dtype == np.uint8 and out.shape == img.shape
    assert out.mean() > img.mean() + 10


def test_clahe_adds_contrast():
    img = _dark()
    out = make_enhancer("clahe", clahe_clip=3.0)(img)
    assert out.dtype == np.uint8 and out.shape == img.shape
    assert out.std() > img.std()  # local contrast raised


def test_gamma_clahe_brightens_most():
    img = _dark()
    g = make_enhancer("gamma")(img)
    gc = make_enhancer("gamma_clahe")(img)
    assert gc.mean() > img.mean() and gc.std() >= g.std() - 1


def test_unknown_mode_raises():
    with pytest.raises(ValueError):
        make_enhancer("sharpen")


def test_modes_constant():
    assert set(MODES) == {"none", "gamma", "clahe", "gamma_clahe"}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spatial-screens/gestures && python3 -m pytest tests/test_enhance.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'enhance'`.

- [ ] **Step 3: Write minimal implementation**

```python
# spatial-screens/gestures/enhance.py
"""Image brightening pre-pass for the gesture sidecar. Pure (cv2/numpy only),
unit-testable. The tracking cameras deliver very dark GRAY8 frames (mean ~12/255
measured on hardware 2026-07-08); lifting shadows makes hands detectable across
the frame, not just the lamp-lit zone.
See docs/specs/2026-07-08-centered-hand-tracking-design.md."""
import cv2
import numpy as np

MODES = ("none", "gamma", "clahe", "gamma_clahe")


def _gamma_lut(gamma):
    return np.array([((i / 255.0) ** gamma) * 255 for i in range(256)],
                    dtype=np.uint8)


def make_enhancer(mode="gamma_clahe", gamma=0.45, clahe_clip=3.0):
    """Return a callable(gray_u8)->gray_u8 applying the configured enhancement.
    The CLAHE object is built once and reused across frames."""
    if mode not in MODES:
        raise ValueError(f"unknown enhance mode {mode!r}; want one of {MODES}")
    if mode == "none":
        return lambda gray: gray
    lut = _gamma_lut(gamma) if "gamma" in mode else None
    clahe = (cv2.createCLAHE(clipLimit=clahe_clip, tileGridSize=(8, 8))
             if "clahe" in mode else None)

    def enhance(gray):
        out = gray
        if lut is not None:
            out = cv2.LUT(out, lut)
        if clahe is not None:
            out = clahe.apply(out)
        return out

    return enhance
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd spatial-screens/gestures && python3 -m pytest tests/test_enhance.py -v`
Expected: PASS (6 passed).

- [ ] **Step 5: Commit**

```bash
git add spatial-screens/gestures/enhance.py spatial-screens/gestures/tests/test_enhance.py
git commit -m "feat(gestures): image brightening pre-pass (enhance.py)"
```

---

### Task 2: `fuse_hands.py` — cross-camera union (pure)

**Files:**
- Create: `spatial-screens/gestures/fuse_hands.py`
- Test: `spatial-screens/gestures/tests/test_fuse_hands.py`

(`hand_tracker.py` is left untouched in this task — its local `match_right_hand` stays as harmless duplication until Task 3 removes it and switches the import.)

**Interfaces:**
- Consumes: `depth_fusion.focal_norm`, `depth_fusion.robust_depth`, `depth_fusion.ASSUMED_HFOV_DEG`, `depth_fusion.BASELINE_M`; `classify.WRIST`.
- Produces:
  - `NOMINAL_DEPTH_M = 0.4`
  - `nominal_disparity(hfov_deg=ASSUMED_HFOV_DEG, baseline_m=BASELINE_M, z0=NOMINAL_DEPTH_M) -> float`
  - `match_right_hand(left_lm, right_hands, max_row_delta=0.15) -> landmarks|None` (moved verbatim from `hand_tracker.py`)
  - `build_fused_hands(left_hands, right_hands, d0) -> list[dict]`, each `{"landmarks": [(x,y),...] canonical, "depth": float|None}`
  - `assign_hands(fused, mirror) -> tuple[dict|None, dict|None]` (user-left, user-right)
  - `fuse_and_assign(left_hands, right_hands, d0, mirror) -> tuple[dict|None, dict|None]` (convenience: build then assign)

- [ ] **Step 1: Write the failing test**

```python
# spatial-screens/gestures/tests/test_fuse_hands.py
from fuse_hands import (nominal_disparity, build_fused_hands, assign_hands,
                        fuse_and_assign)

# 21-point hand at a given wrist x; y rows ~identical across the pair (rectified).
def hand(x, y=0.5):
    return [(x + 0.001 * i, y + 0.0005 * i) for i in range(21)]

D0 = nominal_disparity()


def test_nominal_disparity_positive_small():
    assert 0.0 < D0 < 0.3


def test_hand_in_both_is_one_fused_with_depth():
    # left sees it at x=0.55, right at x=0.44 (positive disparity => real point)
    left = [("L", hand(0.55))]
    right = [("R", hand(0.44))]
    fused = build_fused_hands(left, right, D0)
    assert len(fused) == 1
    assert fused[0]["depth"] is not None  # triangulated


def test_left_only_hand_no_depth_unshifted():
    left = [("L", hand(0.30))]
    fused = build_fused_hands(left, [], D0)
    assert len(fused) == 1 and fused[0]["depth"] is None
    assert abs(fused[0]["landmarks"][0][0] - 0.30) < 1e-9  # not shifted


def test_right_only_hand_shifted_into_left_frame():
    right = [("R", hand(0.30))]
    fused = build_fused_hands([], right, D0)
    assert len(fused) == 1 and fused[0]["depth"] is None
    assert abs(fused[0]["landmarks"][0][0] - (0.30 + D0)) < 1e-9  # +d0 shift


def test_lone_hand_in_both_does_not_double():
    # same physical hand, positive disparity => must dedup to ONE
    left = [("L", hand(0.52))]
    right = [("R", hand(0.42))]
    assert len(build_fused_hands(left, right, D0)) == 1


def test_assign_hands_splits_by_x_mirror_false():
    lo = {"landmarks": hand(0.2), "depth": None}
    hi = {"landmarks": hand(0.8), "depth": None}
    left, right = assign_hands([lo, hi], mirror=False)
    assert left is lo and right is hi  # user-left = image-left when not mirrored


def test_fuse_and_assign_end_to_end():
    left_hands = [("L", hand(0.25))]           # left-image hand, user-left side
    right_hands = [("R", hand(0.75))]          # right-image-only hand, user-right side
    left, right = fuse_and_assign(left_hands, right_hands, D0, mirror=False)
    assert left is not None and right is not None
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spatial-screens/gestures && python3 -m pytest tests/test_fuse_hands.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'fuse_hands'`.

- [ ] **Step 3: Write minimal implementation**

```python
# spatial-screens/gestures/fuse_hands.py
"""Pure cross-camera hand fusion for the gesture sidecar: match a hand across the
stereo pair, dedup a hand seen by both cameras into one entity, canonicalize
right-only detections into the LEFT image frame, and split the fused set into the
user's left/right hands. No MediaPipe/I-O — unit-testable with synthetic
landmarks. See docs/specs/2026-07-08-centered-hand-tracking-design.md."""
import statistics

from classify import WRIST
from depth_fusion import (ASSUMED_HFOV_DEG, BASELINE_M, focal_norm, robust_depth)

NOMINAL_DEPTH_M = 0.4  # assumed hand distance for the right-only disparity shift


def nominal_disparity(hfov_deg=ASSUMED_HFOV_DEG, baseline_m=BASELINE_M,
                      z0=NOMINAL_DEPTH_M):
    """Normalized-x disparity for a hand at the nominal depth. A right-only
    detection is shifted into the left frame by x_left ~= x_right + d0."""
    return focal_norm(hfov_deg) * baseline_m / z0


def match_right_hand(left_lm, right_hands, max_row_delta=0.15):
    """Right-image landmarks of the SAME physical hand as left_lm, or None.
    Smallest mean |dy| with positive mean disparity (xl - xr), within
    max_row_delta. (Moved verbatim from hand_tracker.py.)"""
    best, best_dy = None, max_row_delta
    for _label, rlm in right_hands:
        dy = statistics.mean(abs(l[1] - r[1]) for l, r in zip(left_lm, rlm))
        disp = statistics.mean(l[0] - r[0] for l, r in zip(left_lm, rlm))
        if disp <= 0:
            continue
        if dy < best_dy:
            best, best_dy = rlm, dy
    return best


def _shift_x(landmarks, dx):
    return [(x + dx, y) for (x, y) in landmarks]


def build_fused_hands(left_hands, right_hands, d0):
    """left_hands/right_hands: [(label, landmarks), ...] from the two images.
    Returns fused hands [{"landmarks": canonical [(x,y)...], "depth": m|None}].
    A hand in both images -> one fused hand (deduped) with depth; left-only and
    right-only -> separate, depth None; right-only shifted +d0 into the left
    frame."""
    fused = []
    used = set()  # indices of right detections already matched
    for _label, llm in left_hands:
        candidates = [(i, rh) for i, rh in enumerate(right_hands) if i not in used]
        rlm = match_right_hand(llm, [rh for _i, rh in candidates])
        depth = None
        if rlm is not None:
            for i, (_lbl, r) in candidates:
                if r is rlm:
                    used.add(i)
                    break
            depth = robust_depth(llm, rlm)
        fused.append({"landmarks": llm, "depth": depth})
    for i, (_label, rlm) in enumerate(right_hands):
        if i in used:
            continue
        fused.append({"landmarks": _shift_x(rlm, d0), "depth": None})
    return fused


def assign_hands(fused, mirror):
    """Split fused hands into the user's (left, right) by canonical wrist-x, with
    the same spatial rule as classify.select_hand. Returns (left, right), each a
    fused-hand dict or None."""
    if not fused:
        return None, None
    ordered = sorted(fused, key=lambda f: f["landmarks"][WRIST][0])

    def pick(target):
        want_image_left = (target == "right") if mirror else (target == "left")
        if len(ordered) == 1:
            is_image_left = ordered[0]["landmarks"][WRIST][0] < 0.5
            return ordered[0] if is_image_left == want_image_left else None
        return ordered[0] if want_image_left else ordered[-1]

    return pick("left"), pick("right")


def fuse_and_assign(left_hands, right_hands, d0, mirror):
    return assign_hands(build_fused_hands(left_hands, right_hands, d0), mirror)
```

`fuse_hands.py` carries its own copy of `match_right_hand`; `hand_tracker.py` keeps its local copy + `fuse_depths` for now (harmless one-task duplication). Task 3 deletes the local `match_right_hand` and switches `hand_tracker.py` to import it from `fuse_hands`.

- [ ] **Step 4: Run test to verify it passes**

Run: `cd spatial-screens/gestures && python3 -m pytest tests/test_fuse_hands.py -v`
Expected: PASS (7 passed).

- [ ] **Step 5: Commit**

```bash
git add spatial-screens/gestures/fuse_hands.py spatial-screens/gestures/tests/test_fuse_hands.py
git commit -m "feat(gestures): cross-camera hand fusion (fuse_hands.py)"
```

---

### Task 3: Wire brightening + union into `hand_tracker.py` + CLI flags

**Files:**
- Modify: `spatial-screens/gestures/hand_tracker.py` (imports; `infer` applies the enhancer; `run_inference` uses `fuse_and_assign` when both-cam+stereo; `main` adds CLI flags; remove local `match_right_hand`, import from `fuse_hands`)
- Test: `spatial-screens/gestures/tests/test_hand_tracker.py` (extend: flag parsing)

**Interfaces:**
- Consumes: `enhance.make_enhancer`, `fuse_hands.{nominal_disparity, fuse_and_assign, match_right_hand}`.
- Produces: `run_inference(sock, read_exact, landmarker, landmarker_r=None, fusion=False, both_cam=True, enhancer=None)`; `main()` argparse gains `--enhance`, `--enhance-gamma`, `--enhance-clahe-clip`, `--no-both-cam`.

- [ ] **Step 1: Write the failing test (flag parsing)**

Add to `spatial-screens/gestures/tests/test_hand_tracker.py`:

```python
def test_build_argparser_defaults_and_flags():
    from hand_tracker import build_argparser  # new: factor argparse out of main()
    p = build_argparser()
    a = p.parse_args(["--socket", "/tmp/s.sock"])
    assert a.enhance == "gamma_clahe" and a.enhance_gamma == 0.45
    assert a.enhance_clahe_clip == 3.0 and a.both_cam is True
    b = p.parse_args(["--socket", "/tmp/s.sock", "--no-both-cam",
                      "--enhance", "clahe", "--enhance-gamma", "0.6"])
    assert b.both_cam is False and b.enhance == "clahe" and b.enhance_gamma == 0.6
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spatial-screens/gestures && python3 -m pytest tests/test_hand_tracker.py::test_build_argparser_defaults_and_flags -v`
Expected: FAIL — `ImportError: cannot import name 'build_argparser'`.

- [ ] **Step 3: Implement the wiring**

In `hand_tracker.py`:

(a) Imports — replace the local `match_right_hand` def with an import, and add the new modules. At the top-level import block add:

```python
from enhance import make_enhancer
from fuse_hands import nominal_disparity, fuse_and_assign, match_right_hand
```

Delete the local `def match_right_hand(...)` (now in `fuse_hands`). `fuse_depths` stays and uses the imported `match_right_hand`.

(b) `infer` applies the enhancer. Change its body's first lines to:

```python
    def infer(landmarker_obj, plane, width, height, ts):
        gray = np.frombuffer(plane, dtype=np.uint8).reshape(height, width)
        gray = enhancer(gray)                      # brightening pre-pass
        rgb = cv2.cvtColor(gray, cv2.COLOR_GRAY2RGB)
        mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
        result = landmarker_obj.detect_for_video(mp_image, ts)
        return [(result.handedness[i][0].category_name, _landmarks_to_pairs(lm))
                for i, lm in enumerate(result.hand_landmarks)]
```

(c) `run_inference` signature + emission. Change the signature to
`def run_inference(sock, read_exact, landmarker, landmarker_r=None, fusion=False, both_cam=True, enhancer=None):`
and near the top add `enhancer = enhancer or (lambda g: g)` and `d0 = nominal_disparity()`. Replace the current `left_lm/right_lm/fuse_depths/hand_dict/encode_event` tail with:

```python
        if stereo and both_cam:
            left_f, right_f = fuse_and_assign(hands, right_hands, d0, MIRROR_HANDEDNESS)
            left = (hand_dict(left_f["landmarks"], "left", left_f["depth"])
                    if left_f is not None else None)
            right = (hand_dict(right_f["landmarks"], "right", right_f["depth"])
                     if right_f is not None else None)
        else:
            left_lm = select_hand(hands, "left")
            right_lm = select_hand(hands, "right")
            depths = (fuse_depths({"left": left_lm, "right": right_lm}, right_hands)
                      if stereo else {"left": None, "right": None})
            left = (hand_dict(left_lm, "left", depths["left"])
                    if left_lm is not None else None)
            right = (hand_dict(right_lm, "right", depths["right"])
                     if right_lm is not None else None)
        sock.sendall(encode_event(timestamp, left, right))
```

(Import `MIRROR_HANDEDNESS`, `select_hand` from `classify` — `select_hand` is already imported; add `MIRROR_HANDEDNESS`.)

(d) `main()` — factor argparse into `build_argparser()` and use the new args:

```python
def build_argparser():
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", required=True)
    parser.add_argument("--echo", action="store_true",
                        help="skip MediaPipe; just acknowledge frames (IPC smoke test)")
    parser.add_argument("--fusion", action="store_true",
                        help="enable stereo depth fusion (2nd inference on the right plane)")
    parser.add_argument("--no-both-cam", dest="both_cam", action="store_false",
                        help="disable both-camera tracking union (fusion only feeds depth)")
    parser.add_argument("--enhance", default="gamma_clahe",
                        choices=["none", "gamma", "clahe", "gamma_clahe"])
    parser.add_argument("--enhance-gamma", type=float, default=0.45)
    parser.add_argument("--enhance-clahe-clip", type=float, default=3.0)
    parser.set_defaults(both_cam=True)
    return parser


def main():
    args = build_argparser().parse_args()
    landmarker = None if args.echo else build_landmarker()
    landmarker_r = build_landmarker() if (args.fusion and not args.echo) else None
    enhancer = make_enhancer(args.enhance, args.enhance_gamma, args.enhance_clahe_clip)
    sock = connect(args.socket)
    read_exact = make_reader(sock)
    if args.echo:
        run_echo(sock, read_exact)
    else:
        run_inference(sock, read_exact, landmarker, landmarker_r,
                      args.fusion, args.both_cam, enhancer)
```

- [ ] **Step 4: Run tests to verify they pass (and no regression)**

Run: `cd spatial-screens/gestures && python3 -m pytest tests/ -v`
Expected: PASS — the new flag test passes and all pre-existing sidecar tests (`test_protocol`, `test_classify`, `test_depth_fusion`, `test_hand_tracker`) still pass.

- [ ] **Step 5: Commit**

```bash
git add spatial-screens/gestures/hand_tracker.py spatial-screens/gestures/tests/test_hand_tracker.py
git commit -m "feat(gestures): wire brightening + both-camera union into the tracker"
```

---

### Task 4: C++ — forward `--enhance` / `--no-both-cam` to the sidecar

**Files:**
- Modify: `spatial-screens/src/gesture_client.h:38-39` (start() signature)
- Modify: `spatial-screens/src/gesture_client.cpp:63-70` (argv building)
- Modify: `spatial-screens/src/main.cpp` (defaults + argv parse + start() call)

**Interfaces:**
- Consumes: nothing new.
- Produces: `GestureClient::start(socket, script, timeout=5.0, fusion=false, both_cam=true, enhance="gamma_clahe", enhance_gamma=0.45f, enhance_clahe_clip=3.0f)`.

- [ ] **Step 1: Extend the `start()` signature** — `gesture_client.h`:

```cpp
    bool start(const std::string& socket_path, const std::string& script_path,
               double connect_timeout_s = 5.0, bool fusion = false,
               bool both_cam = true, const std::string& enhance = "gamma_clahe",
               float enhance_gamma = 0.45f, float enhance_clahe_clip = 3.0f);
```

- [ ] **Step 2: Forward the flags** — `gesture_client.cpp`, update the definition line to match the header, then extend the argv block (the enhance strings are locals that live until `start()` returns, after `posix_spawnp`):

```cpp
    std::string s_gamma = std::to_string(enhance_gamma);
    std::string s_clip = std::to_string(enhance_clahe_clip);
    std::vector<char*> argv = {
        const_cast<char*>("python3"),
        const_cast<char*>(script_path.c_str()),
        const_cast<char*>("--socket"),
        const_cast<char*>(socket_path.c_str()),
        const_cast<char*>("--enhance"),
        const_cast<char*>(enhance.c_str()),
        const_cast<char*>("--enhance-gamma"),
        const_cast<char*>(s_gamma.c_str()),
        const_cast<char*>("--enhance-clahe-clip"),
        const_cast<char*>(s_clip.c_str()),
    };
    if (fusion) argv.push_back(const_cast<char*>("--fusion"));
    if (fusion && !both_cam) argv.push_back(const_cast<char*>("--no-both-cam"));
    argv.push_back(nullptr);
```

- [ ] **Step 3: Defaults + parse + call** — `main.cpp`. Near `bool fusion = true;` (line ~279) add:

```cpp
    bool both_cam = true;
    std::string gesture_enhance = "gamma_clahe";
    float gesture_enhance_gamma = 0.45f, gesture_enhance_clahe_clip = 3.0f;
```

In the argv loop, beside the `--fusion`/`--no-fusion` cases (line ~283) add:

```cpp
        if (!strcmp(a, "--no-both-cam")) { both_cam = false; continue; }
        if (!strcmp(a, "--enhance") && i + 1 < argc) { gesture_enhance = argv[++i]; continue; }
        if (!strcmp(a, "--enhance-gamma") && i + 1 < argc) { gesture_enhance_gamma = atof(argv[++i]); continue; }
        if (!strcmp(a, "--enhance-clahe-clip") && i + 1 < argc) { gesture_enhance_clahe_clip = atof(argv[++i]); continue; }
```

Update the usage string (line ~298-300) to mention `[--no-both-cam] [--enhance MODE]`. Update the `start()` call (line ~580):

```cpp
    g_gestures.start(gesture_socket, executable_dir() + "/gestures/hand_tracker.py",
                     5.0, fusion, both_cam, gesture_enhance,
                     gesture_enhance_gamma, gesture_enhance_clahe_clip);
```

- [ ] **Step 4: Build + verify the sidecar receives the flags**

Run: `cd spatial-screens && make`
Expected: links `spatial-screens` with no new errors (the pre-existing `ws_server.hpp` `-Wmissing-field-initializers` warning is unrelated).

Runtime check (needs the glasses, folds into Task 5): after launching, confirm the sidecar argv carries the flags:
`tr '\0' ' ' < /proc/$(pgrep -f hand_tracker.py | head -1)/cmdline`
Expected substring: `--enhance gamma_clahe --enhance-gamma 0.45 --enhance-clahe-clip 3.0 --fusion`.

- [ ] **Step 5: Commit**

```bash
git add spatial-screens/src/gesture_client.h spatial-screens/src/gesture_client.cpp spatial-screens/src/main.cpp
git commit -m "feat(spatial): forward --enhance/--no-both-cam to the gesture sidecar"
```

---

### Task 5: Hardware validation (on glasses; needs a hand)

**Files:** none (manual). Record results in `docs/branches/feat-camera-fusion.md`.

Not code — this is the on-glasses pass. Use a **self-paced protocol** (user says "now"), not timed windows (two timed sweeps failed 2026-07-08).

- [ ] **Step 1:** Launch `./run.sh` (fusion + both-cam + enhance all default-on). Confirm the sidecar argv (Task 4, Step 4).
- [ ] **Step 2: Brightening** — hold a hand where tracking previously failed (dark/upper area); confirm it now detects. Compare `./run.sh --enhance clahe` vs `--enhance none`; watch for noise false-positives. Decide whether to keep `gamma_clahe` or drop to `clahe`.
- [ ] **Step 3: Horizontal coverage map** — slowly sweep a hand left→center→right at a comfortable height; confirm the union tracks across center (no forced left offset). This is the coverage map the timed sweeps couldn't get.
- [ ] **Step 4: Centering** — verify hands can be held centered L-R and gestures (arm, pinch-drag, fist-recenter) work reliably. **Watch for a phantom SECOND hand** when a centered hand sits near the fisheye edge (final-review finding #2: the cameras are not truly rectified, so if the stereo match fails — mean row-delta ≥ 0.15 or disparity ≤ 0 — one physical hand can split into a left-only + a right-only fused entity. The dedup code is correct; this is a match-heuristic limit. If seen, tune `fuse_hands.match_right_hand`'s `max_row_delta`).
- [ ] **Step 5: Two-hand grab across cameras** — both hands armed+pinching, spread apart; check resize/reposition feel (the v1 nominal-disparity limitation). Decide whether the 3-D-fused upgrade is needed.
- [ ] **Step 6: Real-time** — re-run the pending both-hands fps go/no-go with brightening + union active; confirm no fps regression (drain still bounds backlog). This remains the merge gate for `feat/camera-fusion`.
- [ ] **Step 7:** Record verdict + the keep/adjust decisions in `docs/branches/feat-camera-fusion.md`; then the branch is ready for its merge review.

---

## Self-review notes
- **Spec coverage:** brightening (Tasks 1,3) ✓; both-camera union + dedup + canonicalize + handedness (Tasks 2,3) ✓; config/flags (Tasks 3,4) ✓; wire protocol unchanged ✓; hardware validation incl. coverage map + fps gate (Task 5) ✓; v1 nominal-disparity limitation surfaced (Tasks 2,5) ✓.
- **Types:** `make_enhancer`, `nominal_disparity`, `build_fused_hands`, `assign_hands`, `fuse_and_assign`, and the extended `run_inference`/`start` signatures are consistent across tasks.
- Both-hands fps go/no-go for the fusion branch itself is intentionally NOT closed by this plan — it is a pre-existing gate, re-run in Task 5 Step 6.
