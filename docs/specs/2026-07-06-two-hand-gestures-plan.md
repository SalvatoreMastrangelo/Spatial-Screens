# Two-Hand Gesture Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend spatial-screens gestures from one tracked hand to two — either hand does the existing single-hand gestures, and a two-hand grab resizes + repositions the virtual screen.

**Architecture:** The Python sidecar runs two MediaPipe HandLandmarkers (one per camera), routes each hand by handedness, and emits one event carrying both hands. The C++ consumer parses per-hand state, runs a per-hand arming state machine, and — when both hands are armed and pinching — drives a pure resize+reposition grab function.

**Tech Stack:** Python 3 / MediaPipe Tasks API (sidecar), C++17 (spatial-screens), Unix-domain-socket length-prefixed binary frames down + newline-delimited JSON events up. No JSON library on the C++ side (hand-rolled key scanners). No test framework on the C++ side (assert-and-return `CHECK` macro); pytest on the Python side.

## Global Constraints

- Linux x86_64 only. Design spec: `docs/specs/2026-07-06-two-hand-gestures-design.md`. Prior art: `docs/specs/2026-07-03-hand-gesture-control-design.md`.
- Wire protocol is owned on both ends and versioned together — no backward compatibility required, but both ends must change together before runtime works. Unit tests gate each commit; full integration is verified on hardware at the end.
- Conventions: Python 2-space? No — Python is PEP8 4-space (match existing `gestures/*.py`). C++: snake_case functions, `g_` prefix for atomic globals, 4-space indent (match existing `src/*.cpp`).
- Pure math lives in isolated, unit-tested units (`classify.py`, new `gesture_manip.*`) — no MediaPipe/socket/SDK dependency in those files.
- Sidecar ordering invariant: build the (slow) landmarker(s) **before** `connect()` — see `build_landmarker`'s docstring. Preserve this.
- Frame format is GRAY8 (`format == 0`), planes are `width*height` bytes each.
- MediaPipe handedness is mirror-flipped on the forward-facing tracking cameras; the flip lives in one `MIRROR_HANDEDNESS` constant, pinned on hardware.

---

### Task 1: Multi-plane frame protocol (Python)

Extend the length-prefixed frame header with a `u8 n_planes` and carry N equal-size planes so the sidecar receives both camera images in one message.

**Files:**
- Modify: `spatial-screens/gestures/protocol.py` (`read_frame`, `encode_frame`, `_FRAME_HEADER`)
- Test: `spatial-screens/gestures/tests/test_protocol.py`

**Interfaces:**
- Produces: `read_frame(read_exact) -> (timestamp:float, width:int, height:int, fmt:int, planes:list[bytes]) | None`; `encode_frame(timestamp, width, height, fmt, planes:list[bytes]) -> bytes`.

- [ ] **Step 1: Write the failing test** — replace the single-plane frame test with a two-plane round-trip. In `tests/test_protocol.py`, add:

```python
from protocol import encode_frame, read_frame

def _reader_from_bytes(buf):
    state = {"pos": 0}
    def read_exact(n):
        if state["pos"] + n > len(buf):
            return None
        chunk = buf[state["pos"]:state["pos"] + n]
        state["pos"] += n
        return chunk
    return read_exact

def test_frame_two_planes_roundtrip():
    left = bytes(range(10)) * 4      # 40 bytes
    right = bytes(range(40, 50)) * 4 # 40 bytes, distinct
    buf = encode_frame(1.25, 8, 5, 0, [left, right])
    ts, w, h, fmt, planes = read_frame(_reader_from_bytes(buf))
    assert (ts, w, h, fmt) == (1.25, 8, 5, 0)
    assert len(planes) == 2
    assert planes[0] == left
    assert planes[1] == right

def test_frame_single_plane_roundtrip():
    data = bytes(range(20))
    buf = encode_frame(2.0, 4, 5, 0, [data])
    ts, w, h, fmt, planes = read_frame(_reader_from_bytes(buf))
    assert planes == [data]
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spatial-screens/gestures && python3 -m pytest tests/test_protocol.py -v`
Expected: FAIL (`encode_frame` still takes a single `data` bytes, not a list; `TypeError` or assertion on `planes`).

- [ ] **Step 3: Write minimal implementation** — in `protocol.py`, change the header struct and both functions:

```python
_LENGTH_PREFIX = struct.Struct("<I")
_FRAME_HEADER = struct.Struct("<dii B B")  # timestamp, width, height, format, n_planes


def read_frame(read_exact):
    """Read one frame message via read_exact(n) -> bytes|None.

    Returns (timestamp, width, height, format, planes), where planes is a list
    of n_planes equal-size byte buffers (GRAY8: one byte per pixel), or None on
    clean EOF / short read.
    """
    length_bytes = read_exact(_LENGTH_PREFIX.size)
    if length_bytes is None:
        return None
    (length,) = _LENGTH_PREFIX.unpack(length_bytes)
    payload = read_exact(length)
    if payload is None or len(payload) < length:
        return None
    timestamp, width, height, fmt, n_planes = _FRAME_HEADER.unpack(
        payload[: _FRAME_HEADER.size])
    body = payload[_FRAME_HEADER.size :]
    plane_size = len(body) // n_planes if n_planes else 0
    planes = [body[i * plane_size : (i + 1) * plane_size] for i in range(n_planes)]
    return timestamp, width, height, fmt, planes


def encode_frame(timestamp, width, height, fmt, planes):
    """Inverse of read_frame. planes is a list of equal-size byte buffers."""
    body = b"".join(planes)
    payload = _FRAME_HEADER.pack(timestamp, width, height, fmt, len(planes)) + body
    return _LENGTH_PREFIX.pack(len(payload)) + payload
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd spatial-screens/gestures && python3 -m pytest tests/test_protocol.py -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add spatial-screens/gestures/protocol.py spatial-screens/gestures/tests/test_protocol.py
git commit -m "gestures: multi-plane frame protocol (carry both camera planes)"
```

---

### Task 2: Two-hand event schema (Python)

Change `encode_event` to serialize two hands as named `left`/`right` sub-objects.

**Files:**
- Modify: `spatial-screens/gestures/protocol.py` (`encode_event`, add `_hand_obj`)
- Test: `spatial-screens/gestures/tests/test_protocol.py`

**Interfaces:**
- Produces: `encode_event(t:float, left:dict|None, right:dict|None) -> bytes`. Each hand dict has keys `present:bool, handedness:str, pinch_norm:float, pinch_pos:(x,y), pose:str, landmarks:list[(x,y)]` (21 pairs). `None` (or `present=False`) emits `{"present":false}`.

- [ ] **Step 1: Write the failing test** — add to `tests/test_protocol.py`:

```python
import json
from protocol import encode_event

def _decode(evt_bytes):
    assert evt_bytes.endswith(b"\n")
    return json.loads(evt_bytes[:-1].decode("utf-8"))

def test_event_two_hands():
    lm = [(i / 100.0, i / 50.0) for i in range(21)]
    left = {"present": True, "handedness": "left", "pinch_norm": 0.3,
            "pinch_pos": (0.4, 0.5), "pose": "open_palm", "landmarks": lm}
    obj = _decode(encode_event(1.5, left, None))
    assert obj["type"] == "hand"
    assert obj["t"] == 1.5
    assert obj["left"]["present"] is True
    assert obj["left"]["pose"] == "open_palm"
    assert obj["left"]["pinch_pos"] == [0.4, 0.5]
    assert len(obj["left"]["landmarks"]) == 21
    assert obj["left"]["landmarks"][8] == [0.08, 0.16]
    assert obj["right"] == {"present": False}

def test_event_no_hands():
    obj = _decode(encode_event(2.0, None, None))
    assert obj["left"] == {"present": False}
    assert obj["right"] == {"present": False}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spatial-screens/gestures && python3 -m pytest tests/test_protocol.py::test_event_two_hands -v`
Expected: FAIL (`encode_event` still has the old `(t, present, handedness, landmarks, pinch_norm, pinch_pos, pose)` signature).

- [ ] **Step 3: Write minimal implementation** — replace `encode_event` in `protocol.py`:

```python
def _hand_obj(h):
    """One hand's sub-object. h is a dict (see encode_event) or None."""
    if h is None or not h.get("present", False):
        return {"present": False}
    return {
        "present": True,
        "handedness": h.get("handedness", ""),
        "pinch_norm": h["pinch_norm"],
        "pinch_pos": list(h["pinch_pos"]),
        "pose": h["pose"],
        "landmarks": [list(p) for p in h["landmarks"]],
    }


def encode_event(t, left, right):
    """One gesture event as newline-delimited JSON carrying up to two hands.

    left/right are per-hand dicts (keys: present, handedness, pinch_norm,
    pinch_pos, pose, landmarks) or None. pinch_pos is the normalized [0,1]
    midpoint of the thumb-tip/index-tip landmarks — a convenience field so the
    C++ consumer can track pinch-drag deltas without parsing the full landmarks
    array.
    """
    obj = {
        "type": "hand",
        "t": t,
        "left": _hand_obj(left),
        "right": _hand_obj(right),
    }
    return (json.dumps(obj, separators=(",", ":")) + "\n").encode("utf-8")
```

Note: this changes `encode_event`'s signature, so `hand_tracker.py`'s callers are temporarily broken at runtime (not at import) until Task 6. pytest does not import `hand_tracker.py`, so protocol tests stay green.

- [ ] **Step 4: Run test to verify it passes**

Run: `cd spatial-screens/gestures && python3 -m pytest tests/test_protocol.py -v`
Expected: PASS (all protocol tests)

- [ ] **Step 5: Commit**

```bash
git add spatial-screens/gestures/protocol.py spatial-screens/gestures/tests/test_protocol.py
git commit -m "gestures: two-hand event schema (left/right sub-objects)"
```

---

### Task 3: Handedness selection helper (Python)

Add a pure function that picks the user's left/right hand from a frame's detections, applying the mirror flip.

**Files:**
- Modify: `spatial-screens/gestures/classify.py` (add `MIRROR_HANDEDNESS`, `select_hand`)
- Test: `spatial-screens/gestures/tests/test_classify.py`

**Interfaces:**
- Produces: `MIRROR_HANDEDNESS: bool` (module constant); `select_hand(hands, target, mirror=MIRROR_HANDEDNESS) -> landmarks | None`, where `hands` is a list of `(handedness_label:str, landmarks)` tuples as reported per frame and `target` is `"left"`/`"right"` (the user's actual hand).

- [ ] **Step 1: Write the failing test** — add to `tests/test_classify.py`:

```python
from classify import select_hand

_LMA = [(0.1, 0.1)] * 21
_LMB = [(0.9, 0.9)] * 21

def test_select_hand_no_mirror():
    hands = [("Left", _LMA), ("Right", _LMB)]
    assert select_hand(hands, "left", mirror=False) is _LMA
    assert select_hand(hands, "right", mirror=False) is _LMB

def test_select_hand_mirror_flips_label():
    # Forward-facing camera: MediaPipe's "Left" is really the user's right hand.
    hands = [("Left", _LMA), ("Right", _LMB)]
    assert select_hand(hands, "right", mirror=True) is _LMA
    assert select_hand(hands, "left", mirror=True) is _LMB

def test_select_hand_absent_returns_none():
    hands = [("Left", _LMA)]
    assert select_hand(hands, "left", mirror=False) is None  # only Left present
    assert select_hand([], "left", mirror=False) is None
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spatial-screens/gestures && python3 -m pytest tests/test_classify.py -v`
Expected: FAIL (`cannot import name 'select_hand'`).

- [ ] **Step 3: Write minimal implementation** — add to `classify.py`:

```python
# MediaPipe reports handedness assuming a mirrored (selfie) image; the Luma's
# tracking cameras face forward, so the Left/Right labels are inverted relative
# to the user's actual hands. Flip them here. Pinned during the hardware pass.
MIRROR_HANDEDNESS = True


def select_hand(hands, target, mirror=MIRROR_HANDEDNESS):
    """Pick the user's `target` ('left'/'right') hand from one frame's
    detections. `hands` is a list of (handedness_label, landmarks) tuples.
    Returns the matching landmarks, or None if that hand isn't in this frame."""
    for label, landmarks in hands:
        user_label = label.lower()
        if mirror:
            user_label = "right" if user_label == "left" else "left"
        if user_label == target:
            return landmarks
    return None
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd spatial-screens/gestures && python3 -m pytest tests/test_classify.py -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add spatial-screens/gestures/classify.py spatial-screens/gestures/tests/test_classify.py
git commit -m "gestures: handedness selection helper with mirror flip"
```

---

### Task 4: HandState + two-hand C++ parser

Split `GestureEvent` into per-hand `HandState` (kept twice: `left`, `right`) and rewrite `parse_event` to parse the `left`/`right` sub-objects. Keep a legacy primary-hand view so `main.cpp` compiles unchanged until Task 8.

**Files:**
- Modify: `spatial-screens/src/gesture_client.h` (add `HandState`, restructure `GestureEvent`)
- Modify: `spatial-screens/src/gesture_parse.cpp` (`parse_event`, add `hand_object`/`parse_hand`)
- Test: `spatial-screens/src/gesture_parse_test.cpp` (rewrite for two-hand JSON)

**Interfaces:**
- Produces: `struct HandState { bool present; bool pinching; float pinch_x, pinch_y; std::string pose; float landmarks[21][2]; bool has_landmarks; };` and `struct GestureEvent { HandState left, right; /* + legacy primary fields */ };`. `parse_event(const std::string&) -> GestureEvent`.

- [ ] **Step 1: Write the failing test** — replace `src/gesture_parse_test.cpp` body with two-hand cases:

```cpp
// Standalone unit test for the two-hand gesture event parser. No framework:
// asserts via CHECK, returns non-zero on any failure. Build+run with
//   make gesture-parse-test && ./gesture-parse-test
#include "gesture_parse.h"
#include <cmath>
#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static std::string lm21(float base) {
    std::string s = "[";
    for (int i = 0; i < 21; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s[%.2f,%.2f]", i ? "," : "", base + i * 0.01f,
                 base + i * 0.01f + 0.5f);
        s += buf;
    }
    return s + "]";
}

int main() {
    // Both hands present: left pinching (0.12 < 0.5), right open (0.9 >= 0.5).
    std::string both =
        "{\"type\":\"hand\",\"t\":1.5,"
        "\"left\":{\"present\":true,\"handedness\":\"left\",\"pinch_norm\":0.12,"
        "\"pinch_pos\":[0.4,0.5],\"pose\":\"point\",\"landmarks\":" + lm21(0.10f) + "},"
        "\"right\":{\"present\":true,\"handedness\":\"right\",\"pinch_norm\":0.90,"
        "\"pinch_pos\":[0.6,0.7],\"pose\":\"open_palm\",\"landmarks\":" + lm21(0.30f) + "}}";
    GestureEvent ev = parse_event(both);
    CHECK(ev.left.present == true);
    CHECK(ev.left.pinching == true);
    CHECK(ev.left.pose == "point");
    CHECK(std::fabs(ev.left.pinch_x - 0.4f) < 1e-4f);
    CHECK(std::fabs(ev.left.pinch_y - 0.5f) < 1e-4f);
    CHECK(ev.left.has_landmarks == true);
    CHECK(std::fabs(ev.left.landmarks[0][0] - 0.10f) < 1e-4f);
    CHECK(ev.right.present == true);
    CHECK(ev.right.pinching == false);            // 0.90 >= 0.5
    CHECK(ev.right.pose == "open_palm");
    CHECK(std::fabs(ev.right.pinch_x - 0.6f) < 1e-4f);
    CHECK(ev.right.has_landmarks == true);
    CHECK(std::fabs(ev.right.landmarks[20][0] - (0.30f + 20 * 0.01f)) < 1e-4f);

    // One hand present.
    std::string one =
        "{\"type\":\"hand\",\"t\":2.0,"
        "\"left\":{\"present\":false},"
        "\"right\":{\"present\":true,\"handedness\":\"right\",\"pinch_norm\":0.20,"
        "\"pinch_pos\":[0.5,0.5],\"pose\":\"fist\",\"landmarks\":" + lm21(0.20f) + "}}";
    GestureEvent ev2 = parse_event(one);
    CHECK(ev2.left.present == false);
    CHECK(ev2.left.pinching == false);
    CHECK(ev2.left.has_landmarks == false);
    CHECK(ev2.right.present == true);
    CHECK(ev2.right.pinching == true);
    CHECK(ev2.right.pose == "fist");

    // Neither hand present.
    std::string none = "{\"type\":\"hand\",\"t\":3.0,"
                       "\"left\":{\"present\":false},\"right\":{\"present\":false}}";
    GestureEvent ev3 = parse_event(none);
    CHECK(ev3.left.present == false);
    CHECK(ev3.right.present == false);
    CHECK(ev3.left.has_landmarks == false);

    if (failures == 0) { printf("gesture_parse_test: all checks passed\n"); return 0; }
    printf("gesture_parse_test: %d failure(s)\n", failures);
    return 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spatial-screens && make gesture-parse-test`
Expected: FAIL to compile (`ev.left` has no member — `GestureEvent` is still flat).

- [ ] **Step 3a: Restructure `GestureEvent`** — replace the struct in `src/gesture_client.h` (lines 15-22) with:

```cpp
// One hand's classified state from the Python/MediaPipe sidecar.
struct HandState {
    bool present = false;
    bool pinching = false;   // pinch_norm < PINCH_THRESHOLD
    float pinch_x = 0.f, pinch_y = 0.f; // normalized [0,1] pinch midpoint
    std::string pose;        // "open_palm" | "fist" | "point" | "none" | ""
    float landmarks[21][2] = {}; // MediaPipe hand, normalized [0,1] image coords (x-right, y-down); thumb tip = [4], index tip = [8]
    bool has_landmarks = false;  // true iff a full 21-point array parsed
};

// Both hands from one event. See gestures/protocol.py encode_event.
struct GestureEvent {
    HandState left, right;
    // Legacy single-hand view (primary = left if present, else right), a copy
    // of the primary hand's fields so the existing single-hand render-loop /
    // overlay code in main.cpp compiles and runs unchanged during the two-hand
    // migration. Removed in Task 9 once main.cpp reads left/right directly.
    bool present = false;
    bool pinching = false;
    float pinch_x = 0.f, pinch_y = 0.f;
    std::string pose;
    float landmarks[21][2] = {};
    bool has_landmarks = false;
};
```

- [ ] **Step 3b: Rewrite `parse_event`** — in `src/gesture_parse.cpp`, keep the `json_find_*` helpers as-is, and replace `parse_event` (lines 90-100) with:

```cpp
namespace {

// Extract the "{...}" body of a hand sub-object ("left"/"right") — its contents
// contain no nested '{' (values are bools/numbers/strings/arrays), so the first
// '}' after the opening brace closes it. Returns "" if the key is absent.
std::string hand_object(const std::string& s, const char* key) {
    std::string pat = std::string("\"") + key + "\":{";
    auto pos = s.find(pat);
    if (pos == std::string::npos) return std::string();
    pos += pat.size();
    auto end = s.find('}', pos);
    if (end == std::string::npos) return std::string();
    return s.substr(pos, end - pos);
}

HandState parse_hand(const std::string& obj) {
    HandState h;
    json_find_bool(obj, "present", h.present);
    float pinch_norm = 999.f;
    json_find_number(obj, "pinch_norm", pinch_norm);
    h.pinching = h.present && pinch_norm < PINCH_THRESHOLD;
    json_find_pair(obj, "pinch_pos", h.pinch_x, h.pinch_y);
    json_find_string(obj, "pose", h.pose);
    h.has_landmarks = json_find_landmarks(obj, h.landmarks);
    return h;
}

} // namespace

GestureEvent parse_event(const std::string& line) {
    GestureEvent ev;
    ev.left = parse_hand(hand_object(line, "left"));
    ev.right = parse_hand(hand_object(line, "right"));
    // Populate the legacy primary-hand view (left if present, else right).
    const HandState& primary = ev.left.present ? ev.left : ev.right;
    ev.present = primary.present;
    ev.pinching = primary.pinching;
    ev.pinch_x = primary.pinch_x;
    ev.pinch_y = primary.pinch_y;
    ev.pose = primary.pose;
    ev.has_landmarks = primary.has_landmarks;
    memcpy(ev.landmarks, primary.landmarks, sizeof(ev.landmarks));
    return ev;
}
```

Move the existing `json_find_*` helpers and the `PINCH_THRESHOLD` constant so they are declared **above** the new `namespace { ... }` block that uses them (they are already in an anonymous namespace at the top of the file — extend it to include `hand_object`/`parse_hand`, or place the new helpers after the existing ones but before `parse_event`).

- [ ] **Step 4: Run test to verify it passes**

Run: `cd spatial-screens && make gesture-parse-test && ./gesture-parse-test`
Expected: `gesture_parse_test: all checks passed`

Also confirm the app still compiles with the legacy view intact:
Run: `cd spatial-screens && make spatial-screens 2>&1 | tail -5`
Expected: builds (no errors referencing `gev.` fields).

- [ ] **Step 5: Commit**

```bash
git add spatial-screens/src/gesture_client.h spatial-screens/src/gesture_parse.cpp spatial-screens/src/gesture_parse_test.cpp
git commit -m "spatial-screens: parse two-hand events into HandState left/right"
```

---

### Task 5: Pure grab math (C++)

Add a pure, unit-tested resize+reposition function used by the two-hand grab. No SDK/Vulkan/socket dependencies.

**Files:**
- Create: `spatial-screens/src/gesture_manip.h`
- Create: `spatial-screens/src/gesture_manip.cpp`
- Create: `spatial-screens/src/gesture_manip_test.cpp`
- Modify: `spatial-screens/Makefile` (add object rule + `gesture-manip-test` target; add to `test`)

**Interfaces:**
- Produces: `struct GVec3 { float x, y, z; };`, `struct GrabState { bool active; float spread0, mid0x, mid0y, size0; GVec3 anchor0, right0, up0; };`, `struct GrabResult { float diag; GVec3 anchor; };`, `GrabState grab_begin(float pLx,float pLy,float pRx,float pRy,float size0,GVec3 anchor0,GVec3 right0,GVec3 up0);`, `GrabResult grab_update(const GrabState&,float pLx,float pLy,float pRx,float pRy,float distance,float gain,float diag_min,float diag_max);`.

- [ ] **Step 1: Write the failing test** — create `src/gesture_manip_test.cpp`:

```cpp
// Standalone unit test for the pure two-hand grab math. Build+run with
//   make gesture-manip-test && ./gesture-manip-test
#include "gesture_manip.h"
#include <cmath>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main() {
    GVec3 anchor0{1.f, 2.f, 3.f};
    GVec3 right0{1.f, 0.f, 0.f};
    GVec3 up0{0.f, 1.f, 0.f};
    // Pinches 0.2 apart horizontally, midpoint (0.5,0.5), size0 = 60 in.
    GrabState g = grab_begin(0.4f, 0.5f, 0.6f, 0.5f, 60.f, anchor0, right0, up0);
    CHECK(g.active);
    CHECK(std::fabs(g.spread0 - 0.2f) < 1e-4f);

    // Spread doubled (0.4), midpoint unchanged -> diag doubles, anchor unchanged.
    GrabResult r1 = grab_update(g, 0.3f, 0.5f, 0.7f, 0.5f,
                                /*distance*/2.f, /*gain*/1.f, /*min*/20.f, /*max*/200.f);
    CHECK(std::fabs(r1.diag - 120.f) < 1e-3f);
    CHECK(std::fabs(r1.anchor.x - 1.f) < 1e-4f);
    CHECK(std::fabs(r1.anchor.y - 2.f) < 1e-4f);

    // Midpoint shifts +0.1 in x, -0.1 in y (image space); spread unchanged.
    // world dx = dm.x*gain*distance = 0.1*1*2 = 0.2 along right0 (+x)
    // world dy = -dm.y*gain*distance = -(-0.1)*1*2 = 0.2 along up0 (+y)
    GrabResult r2 = grab_update(g, 0.5f, 0.4f, 0.7f, 0.4f,
                                2.f, 1.f, 20.f, 200.f);
    CHECK(std::fabs(r2.anchor.x - 1.2f) < 1e-4f);
    CHECK(std::fabs(r2.anchor.y - 2.2f) < 1e-4f);
    CHECK(std::fabs(r2.diag - 60.f) < 1e-3f);   // spread unchanged -> size0

    // Clamp: huge spread -> diag capped at max.
    GrabResult r3 = grab_update(g, 0.0f, 0.5f, 1.0f, 0.5f, 2.f, 1.f, 20.f, 200.f);
    CHECK(std::fabs(r3.diag - 200.f) < 1e-3f);

    if (failures == 0) { printf("gesture_manip_test: all checks passed\n"); return 0; }
    printf("gesture_manip_test: %d failure(s)\n", failures);
    return 1;
}
```

- [ ] **Step 2: Create the header** — `src/gesture_manip.h`:

```cpp
#pragma once

// Pure two-hand "grab" math: spread between the two pinch points resizes the
// screen (diagonal inches), their midpoint repositions it in the screen's
// right/up plane. No SDK/Vulkan/socket deps — unit-tested in gesture_manip_test.
// Its own minimal vector type keeps this unit free of main.cpp's Vec3; callers
// convert at the boundary.
struct GVec3 { float x = 0.f, y = 0.f, z = 0.f; };

struct GrabState {
    bool active = false;
    float spread0 = 0.f;          // pinch-point distance at grab start
    float mid0x = 0.f, mid0y = 0.f; // pinch midpoint at grab start (image space)
    float size0 = 0.f;            // screen diagonal (inches) at grab start
    GVec3 anchor0;                // screen anchor position at grab start
    GVec3 right0, up0;            // screen basis snapshot (for reposition)
};

struct GrabResult {
    float diag = 0.f;             // new screen diagonal (inches)
    GVec3 anchor;                 // new screen anchor position
};

// Snapshot grab-start state from the two normalized-image pinch points.
GrabState grab_begin(float pLx, float pLy, float pRx, float pRy,
                     float size0, GVec3 anchor0, GVec3 right0, GVec3 up0);

// Compute new diag + anchor for the current pinch points. `distance` is the
// current screen distance (m); `gain` maps a normalized-image fraction to a
// world fraction; diag is clamped to [diag_min, diag_max].
GrabResult grab_update(const GrabState& g, float pLx, float pLy, float pRx, float pRy,
                       float distance, float gain, float diag_min, float diag_max);
```

- [ ] **Step 3: Create the implementation** — `src/gesture_manip.cpp`:

```cpp
#include "gesture_manip.h"
#include <cmath>

GrabState grab_begin(float pLx, float pLy, float pRx, float pRy,
                     float size0, GVec3 anchor0, GVec3 right0, GVec3 up0) {
    GrabState g;
    g.active = true;
    g.spread0 = std::hypot(pLx - pRx, pLy - pRy);
    if (g.spread0 < 1e-4f) g.spread0 = 1e-4f;   // avoid div-by-zero on update
    g.mid0x = (pLx + pRx) * 0.5f;
    g.mid0y = (pLy + pRy) * 0.5f;
    g.size0 = size0;
    g.anchor0 = anchor0;
    g.right0 = right0;
    g.up0 = up0;
    return g;
}

GrabResult grab_update(const GrabState& g, float pLx, float pLy, float pRx, float pRy,
                       float distance, float gain, float diag_min, float diag_max) {
    float spread = std::hypot(pLx - pRx, pLy - pRy);
    float diag = g.size0 * (spread / g.spread0);   // ratio-from-start: drift-free
    if (diag < diag_min) diag = diag_min;
    if (diag > diag_max) diag = diag_max;

    float midx = (pLx + pRx) * 0.5f, midy = (pLy + pRy) * 0.5f;
    float dmx = midx - g.mid0x, dmy = midy - g.mid0y;
    float wx = dmx * gain * distance;   // rightward
    float wy = -dmy * gain * distance;  // image y is down -> world up

    GrabResult r;
    r.diag = diag;
    r.anchor.x = g.anchor0.x + g.right0.x * wx + g.up0.x * wy;
    r.anchor.y = g.anchor0.y + g.right0.y * wx + g.up0.y * wy;
    r.anchor.z = g.anchor0.z + g.right0.z * wx + g.up0.z * wy;
    return r;
}
```

- [ ] **Step 4: Wire the Makefile** — in `spatial-screens/Makefile`, after the `gesture-parse-test` rules (around line 36), add:

```make
src/gesture_manip.o: src/gesture_manip.cpp src/gesture_manip.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/gesture_manip_test.o: src/gesture_manip_test.cpp src/gesture_manip.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Pure grab-math test — links only the manip unit, no SDK/Vulkan deps.
gesture-manip-test: src/gesture_manip_test.o src/gesture_manip.o
	$(CXX) $^ -o $@
```

Change the `test` target (line 39) to run both, and add the new object to `spatial-screens`'s `OBJS` (line 18-20) and the `clean` list (line 77):

```make
test: gesture-parse-test gesture-manip-test
	./gesture-parse-test
	./gesture-manip-test
```

`OBJS := src/main.o src/vk_renderer.o src/vk_surface.o src/gesture_client.o \
        src/gesture_parse.o src/gesture_manip.o \
        src/capture.o src/capture_xshm.o src/config.o src/capture_portal.o src/telemetry.o`

`clean`: add `gesture-manip-test` to the `rm -f` list.

- [ ] **Step 5: Run test to verify it passes**

Run: `cd spatial-screens && make gesture-manip-test && ./gesture-manip-test`
Expected: `gesture_manip_test: all checks passed`

- [ ] **Step 6: Commit**

```bash
git add spatial-screens/src/gesture_manip.h spatial-screens/src/gesture_manip.cpp spatial-screens/src/gesture_manip_test.cpp spatial-screens/Makefile
git commit -m "spatial-screens: pure two-hand grab (resize+reposition) math + test"
```

---

### Task 6: Sidecar dual-inference wiring

Make `hand_tracker.py` build two landmarkers, read both planes, route each hand by handedness, and emit the two-hand event.

**Files:**
- Modify: `spatial-screens/gestures/hand_tracker.py` (`build_landmarker` → two instances, `run_inference`, `_no_hand_event`, `run_echo`, `_landmarks_to_pairs` usage)

**Interfaces:**
- Consumes: `protocol.encode_event(t, left, right)`, `protocol.read_frame` (planes list), `classify.select_hand`, `classify.pinch_norm/pinch_pos/classify_pose`.

- [ ] **Step 1: Update `build_landmarker`** to build one landmarker (num_hands=2) and have `main()` create two. Replace the `num_hands=1` in `build_landmarker` (line 133) with `num_hands=2`, and update its docstring's "one hand" wording. Then change `main()` (lines 188-196):

```python
    # Build the (slow-to-import/load) landmarkers before connecting — see
    # build_landmarker()'s docstring for why ordering matters. Two independent
    # VIDEO streams (left + right camera) each need their own stateful instance.
    landmarker_left = None if args.echo else build_landmarker()
    landmarker_right = None if args.echo else build_landmarker()

    sock = connect(args.socket)
    read_exact = make_reader(sock)

    if args.echo:
        run_echo(sock, read_exact)
    else:
        run_inference(sock, read_exact, landmarker_left, landmarker_right)
```

- [ ] **Step 2: Update `_no_hand_event` and `run_echo`** for the new `encode_event` signature (lines 86-99):

```python
def _no_hand_event(timestamp):
    return encode_event(timestamp, None, None)


def run_echo(sock, read_exact):
    frame_count = 0
    while True:
        frame = read_frame(read_exact)
        if frame is None:
            break
        timestamp, width, height, fmt, planes = frame
        frame_count += 1
        print(f"echo: frame {frame_count} {width}x{height} fmt={fmt} "
              f"planes={len(planes)}", file=sys.stderr)
        sock.sendall(_no_hand_event(timestamp))
```

- [ ] **Step 3: Rewrite `run_inference`** for dual-frame, per-hand routing (lines 141-176):

```python
def run_inference(sock, read_exact, landmarker_left, landmarker_right):
    import cv2
    import mediapipe as mp
    import numpy as np
    from classify import classify_pose, pinch_norm, pinch_pos, select_hand

    def detect(landmarker, plane, width, height, ts_ms):
        gray = np.frombuffer(plane, dtype=np.uint8).reshape(height, width)
        rgb = cv2.cvtColor(gray, cv2.COLOR_GRAY2RGB)
        mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
        result = landmarker.detect_for_video(mp_image, ts_ms)
        return [
            (result.handedness[i][0].category_name, _landmarks_to_pairs(lm))
            for i, lm in enumerate(result.hand_landmarks)
        ]

    def hand_dict(lm, handedness):
        return {
            "present": True,
            "handedness": handedness,
            "pinch_norm": pinch_norm(lm),
            "pinch_pos": pinch_pos(lm),
            "pose": classify_pose(lm),
            "landmarks": lm,
        }

    last_ts_l, last_ts_r = -1, -1
    while True:
        frame = read_frame(read_exact)
        if frame is None:
            break
        timestamp, width, height, fmt, planes = frame

        if fmt != FORMAT_GRAY8 or len(planes) < 1:
            print(f"hand_tracker: unexpected format {fmt}/{len(planes)} planes, "
                  f"skipping", file=sys.stderr)
            continue

        # detect_for_video needs strictly-increasing per-stream timestamps.
        ts_l = max(int(timestamp * 1000), last_ts_l + 1)
        last_ts_l = ts_l
        left_hands = detect(landmarker_left, planes[0], width, height, ts_l)

        # Right camera plane if present, else fall back to the left plane so a
        # single-plane sender still yields both hands (num_hands=2 sees both).
        right_plane = planes[1] if len(planes) > 1 else planes[0]
        ts_r = max(int(timestamp * 1000), last_ts_r + 1)
        last_ts_r = ts_r
        right_hands = detect(landmarker_right, right_plane, width, height, ts_r)

        left_lm = select_hand(left_hands, "left")
        right_lm = select_hand(right_hands, "right")
        left = hand_dict(left_lm, "left") if left_lm is not None else None
        right = hand_dict(right_lm, "right") if right_lm is not None else None
        sock.sendall(encode_event(timestamp, left, right))
```

- [ ] **Step 4: Verify it imports and byte-compiles** (no hardware needed):

Run: `cd spatial-screens/gestures && python3 -c "import ast; ast.parse(open('hand_tracker.py').read()); print('parse ok')" && python3 -m pytest tests/ -v`
Expected: `parse ok` and all pytest pass (protocol + classify).

- [ ] **Step 5: Commit**

```bash
git add spatial-screens/gestures/hand_tracker.py
git commit -m "gestures: dual-landmarker sidecar — route each hand by handedness"
```

---

### Task 7: C++ dual-plane frame send

Send both camera planes in one frame message and forward `image_right0` from the SDK callback.

**Files:**
- Modify: `spatial-screens/src/gesture_client.h` (`maybe_send_frame` signature)
- Modify: `spatial-screens/src/gesture_client.cpp` (`maybe_send_frame` packs 2 planes)
- Modify: `spatial-screens/src/main.cpp` (`on_camera_carina` passes both planes)

**Interfaces:**
- Produces: `void GestureClient::maybe_send_frame(const uint8_t* left, const uint8_t* right, int width, int height, double timestamp);`

- [ ] **Step 1: Update the header** — in `src/gesture_client.h`, change the `maybe_send_frame` declaration (line 35):

```cpp
    // Rate-limited to GESTURE_INFER_HZ; safe to call every render frame.
    // `left` and `right` must each be width*height bytes (GRAY8), one byte per
    // pixel — the two tracking-camera planes, sent together as one frame.
    void maybe_send_frame(const uint8_t* left, const uint8_t* right,
                          int width, int height, double timestamp);
```

- [ ] **Step 2: Update the packing** — in `src/gesture_client.cpp`, replace the body of `maybe_send_frame` (lines 89-108, up to the send loop) so it packs two planes and the `n_planes` byte:

```cpp
void GestureClient::maybe_send_frame(const uint8_t* left, const uint8_t* right,
                                     int width, int height, double timestamp) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) return;
    double t = now_s();
    if (t - last_send_s_ < 1.0 / GESTURE_INFER_HZ) return;
    last_send_s_ = t;

    const size_t plane_len = size_t(width) * size_t(height);
    // header: f64 ts + i32 w + i32 h + u8 fmt + u8 n_planes, then 2 planes
    const uint32_t payload_len = uint32_t(8 + 4 + 4 + 1 + 1 + 2 * plane_len);

    std::string msg;
    msg.resize(4 + payload_len);
    uint8_t* p = reinterpret_cast<uint8_t*>(&msg[0]);
    memcpy(p, &payload_len, 4);             p += 4;
    memcpy(p, &timestamp, 8);               p += 8;
    int32_t w = width, h = height;
    memcpy(p, &w, 4);                       p += 4;
    memcpy(p, &h, 4);                       p += 4;
    *p = 0; /* format: GRAY8 */             p += 1;
    *p = 2; /* n_planes: left,right */      p += 1;
    memcpy(p, left, plane_len);             p += plane_len;
    memcpy(p, right, plane_len);
```

Leave the existing send-loop comment and `while (sent_total < msg.size())` drain logic **unchanged** below this point (it already handles the larger ~2×307 KB payload via the 200 ms aggregate deadline + `poll(POLLOUT)`).

- [ ] **Step 3: Forward both planes from the SDK callback** — in `src/main.cpp`, update `on_camera_carina` (lines 211-229). Un-comment `image_right0` and pass it through:

```cpp
static void on_camera_carina(char* image_left0, char* image_right0,
                              char* /*image_left1*/, char* /*image_right1*/,
                              double timestamp, int width, int height) {
    if (g_probe_camera && g_probe_frames_remaining > 0) {
        static int frame_idx = 0;
        char path[256];
        snprintf(path, sizeof(path), "/tmp/spatial-screens-probe-%03d.pgm", frame_idx++);
        FILE* f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P5\n%d %d\n255\n", width, height);
            fwrite(image_left0, 1, size_t(width) * size_t(height), f);
            fclose(f);
            printf("gestures: probe frame -> %s (%dx%d, t=%.3f)\n", path, width, height, timestamp);
        }
        g_probe_frames_remaining--;
    }
    g_cam_w.store(width, std::memory_order_relaxed);
    g_cam_h.store(height, std::memory_order_relaxed);
    g_gestures.maybe_send_frame(reinterpret_cast<uint8_t*>(image_left0),
                                reinterpret_cast<uint8_t*>(image_right0),
                                width, height, timestamp);
}
```

- [ ] **Step 4: Verify it builds**

Run: `cd spatial-screens && make spatial-screens 2>&1 | tail -5`
Expected: builds with no errors.

- [ ] **Step 5: Commit**

```bash
git add spatial-screens/src/gesture_client.h spatial-screens/src/gesture_client.cpp spatial-screens/src/main.cpp
git commit -m "spatial-screens: forward both camera planes to the gesture sidecar"
```

---

### Task 8: Per-hand arming + two-hand grab state machine

Replace the single-hand render-loop gesture block with per-hand arming, single-hand-either-hand gestures, and the two-hand grab.

**Files:**
- Modify: `spatial-screens/src/main.cpp` (add include + grab tunables; gesture state vars ~612-616; gesture block ~675-732)

**Interfaces:**
- Consumes: `gesture_manip.h` (`GrabState`, `GrabResult`, `GVec3`, `grab_begin`, `grab_update`); `GestureEvent{left,right}`; `qrot`, `Quat`, `Vec3`, `place_screen`, `anchor_q`, `anchor_p`, `distance`, `diag_in`, `ori_offset`, `head_q`.

- [ ] **Step 1: Add the include and tunables** — near the top includes of `main.cpp` (after `#include "gesture_client.h"`, line 51) add:

```cpp
#include "gesture_manip.h"
```

And with the other gesture constants (after line 206, `FIST_HOLD_SECONDS`), add:

```cpp
static constexpr float GRAB_REPOSITION_GAIN = 1.5f; // image-fraction -> world-fraction; tune on hardware
static constexpr float GRAB_DIAG_MIN = 20.f;        // inches
static constexpr float GRAB_DIAG_MAX = 200.f;       // inches
```

- [ ] **Step 2: Replace the per-hand gesture state variables** — in `main.cpp` replace lines 612-616 (`was_pinching` … `gestures_armed`) with per-hand versions plus grab state:

```cpp
        // Per-hand arming + single-hand drag state (index 0 = left, 1 = right).
        bool armed[2] = {false, false};
        bool was_pinching[2] = {false, false};
        float pinch_prev_y[2] = {0.f, 0.f};
        double fist_start_s[2] = {-1.0, -1.0};
        bool fist_triggered[2] = {false, false};
        // Two-hand grab (resize + reposition).
        GrabState grab;
```

- [ ] **Step 3: Replace the gesture block** — replace lines 675-732 (the entire `// ---- gestures` block, from `GestureEvent gev = g_gestures.poll();` through the closing `}` of the `else` at line 732) with:

```cpp
        // ---- gestures (two hands)
        // Per-hand arming: each open palm arms its own hand. A single-hand
        // gesture needs that hand armed; the two-hand grab needs both. Each
        // completed gesture disarms the hand(s) it used. See
        // docs/specs/2026-07-06-two-hand-gestures-design.md.
        GestureEvent gev = g_gestures.poll();
        HandState* hands[2] = { &gev.left, &gev.right };
        for (int i = 0; i < 2; i++) {
            if (!hands[i]->present) armed[i] = false;      // hand gone -> re-arm
            if (hands[i]->pose == "open_palm") armed[i] = true;
        }

        bool grab_now = gev.left.present && gev.right.present &&
                        armed[0] && armed[1] &&
                        gev.left.pinching && gev.right.pinching;

        if (grab_now) {
            if (!grab.active) {
                Vec3 r = qrot(anchor_q, { 1, 0, 0 });
                Vec3 u = qrot(anchor_q, { 0, 1, 0 });
                grab = grab_begin(gev.left.pinch_x, gev.left.pinch_y,
                                  gev.right.pinch_x, gev.right.pinch_y,
                                  diag_in, { anchor_p.x, anchor_p.y, anchor_p.z },
                                  { r.x, r.y, r.z }, { u.x, u.y, u.z });
            } else {
                GrabResult gr = grab_update(grab, gev.left.pinch_x, gev.left.pinch_y,
                                            gev.right.pinch_x, gev.right.pinch_y,
                                            distance, GRAB_REPOSITION_GAIN,
                                            GRAB_DIAG_MIN, GRAB_DIAG_MAX);
                diag_in = gr.diag;
                anchor_p = { gr.anchor.x, gr.anchor.y, gr.anchor.z };
            }
            // Suppress single-hand logic and reset its per-hand run-state so it
            // re-seeds cleanly if a hand later acts alone.
            for (int i = 0; i < 2; i++) {
                was_pinching[i] = false;
                fist_start_s[i] = -1;
                fist_triggered[i] = false;
            }
        } else {
            if (grab.active) {
                grab.active = false;   // grab ended -> both hands must re-arm
                armed[0] = armed[1] = false;
            }
            // Single-hand gestures on the first qualifying armed hand
            // (fist-hold recenter takes priority over pinch-drag distance).
            for (int i = 0; i < 2; i++) {
                HandState& h = *hands[i];
                if (armed[i] && h.pose == "fist") {
                    if (fist_start_s[i] < 0) { fist_start_s[i] = now_s(); fist_triggered[i] = false; }
                    else if (!fist_triggered[i] && now_s() - fist_start_s[i] > FIST_HOLD_SECONDS) {
                        ori_offset = yaw_twist(head_q);
                        place_screen();
                        printf("gesture recenter (fist-hold)\n");
                        tele.log("info", "recentered");
                        fist_triggered[i] = true;
                        armed[i] = false;      // one gesture per open-hand arm
                    }
                    was_pinching[i] = false;
                    break;                     // this hand owns the gesture this frame
                } else if (armed[i] && h.pinching) {
                    fist_start_s[i] = -1;
                    fist_triggered[i] = false;
                    if (was_pinching[i]) {
                        float dy = h.pinch_y - pinch_prev_y[i]; // image space: +y down
                        float old_distance = distance;
                        distance = std::clamp(distance - dy * PINCH_DISTANCE_SENSITIVITY, 0.5f, 10.f);
                        Vec3 fwd = qrot(anchor_q, { 0, 0, -1 });
                        float ddist = distance - old_distance;
                        anchor_p.x += fwd.x * ddist;
                        anchor_p.y += fwd.y * ddist;
                        anchor_p.z += fwd.z * ddist;
                    }
                    pinch_prev_y[i] = h.pinch_y;
                    was_pinching[i] = true;
                    break;
                } else {
                    fist_start_s[i] = -1;
                    fist_triggered[i] = false;
                    if (was_pinching[i]) armed[i] = false; // completed pinch-drag -> disarm
                    was_pinching[i] = false;
                }
            }
        }
```

Note: `diag_in` is the runtime alias for screen size (declared `float distance = o.distance, diag_in = o.size, …` at line 391) — assigning to it here resizes the screen the same way the `-`/`=` keys do.

- [ ] **Step 4: Verify it builds**

Run: `cd spatial-screens && make spatial-screens 2>&1 | tail -8`
Expected: builds with no errors.

- [ ] **Step 5: Commit**

```bash
git add spatial-screens/src/main.cpp
git commit -m "spatial-screens: per-hand arming + two-hand resize/reposition grab"
```

---

### Task 9: Two-hand overlay + remove legacy fields

Draw both hands in the HUD (per-hand alpha/green), raise the draw cap, and drop the now-unused legacy primary-hand fields from `GestureEvent`.

**Files:**
- Modify: `spatial-screens/src/main.cpp` (overlay block ~939-976; confirm `draws[]` capacity)
- Modify: `spatial-screens/src/gesture_client.h` (remove legacy fields)
- Modify: `spatial-screens/src/gesture_parse.cpp` (stop populating legacy fields)

**Interfaces:**
- Consumes: `gev.left`, `gev.right` (`HandState`), `armed[2]`.

- [ ] **Step 1: Confirm/raise the overlay draw capacity** — find the `draws[]`/`QuadDraw` array declaration and `ndraw` cap. Search:

Run: `grep -n "QuadDraw draws\|draws\[\|ndraw\b\|< 32" spatial-screens/src/main.cpp`

If the `draws[]` array is sized < 48 (two hands = 42 landmark dots + screen/cursor/status quads), raise its size to at least 56, and change the landmark loop cap `ndraw < 32` (line 956) to `ndraw < 54`. (Exact array name/size confirmed at implementation; the array is declared once before the render loop.)

- [ ] **Step 2: Replace the overlay block** — replace the single-hand overlay (lines 939-976) with a per-hand loop. This uses the per-hand `armed[2]` from Task 8:

```cpp
            // Hand-landmark overlay: both hands, each as 21 dots in a
            // head-locked lower-left panel, shown only while that hand is seen.
            // Per-hand alpha follows that hand's armed flag; thumb/index tips
            // go green when that hand is armed and pinching.
            if (g_gestures.enabled()) {
                int cw = g_cam_w.load(std::memory_order_relaxed);
                int ch = g_cam_h.load(std::memory_order_relaxed);
                float aspect = (cw > 0 && ch > 0) ? float(cw) / float(ch) : 4.f / 3.f;
                const float PANEL_H = 0.09f * DOT_Z;        // half-height (~10° tall)
                const float PANEL_W = PANEL_H * aspect;     // aspect-preserved half-width
                float leye[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,
                                   -0.55f * tan_r * DOT_Z, -0.45f * tan_t * DOT_Z, -DOT_Z, 1 };
                float panel_mvp[16];
                mat_mul(proj, leye, panel_mvp);
                const float lm_col[4] = { 0.40f, 0.90f, 1.00f, 1.f }; // soft cyan
                const float tip[4]    = { 1.00f, 0.85f, 0.10f, 1.f }; // yellow (thumb/index tip)
                const float lm_r = 0.0033f * DOT_Z;  // ~0.35 degrees apparent size
                HandState* ovh[2] = { &gev.left, &gev.right };
                for (int hnd = 0; hnd < 2; hnd++) {
                    const HandState& h = *ovh[hnd];
                    if (!h.present || !h.has_landmarks) continue;
                    const float ov_alpha = armed[hnd] ? 1.f : 0.45f;
                    for (int i = 0; i < 21 && ndraw < 54; i++) {
                        float nx = h.landmarks[i][0];
                        float ny = h.landmarks[i][1];
                        // Normalized image coords -> panel-local. Image y is
                        // down, eye-space y up, so negate y. If the hand reads
                        // mirrored on hardware, change (nx - 0.5f) to (0.5f - nx).
                        float lx =  (nx - 0.5f) * 2.f * PANEL_W;
                        float ly = -(ny - 0.5f) * 2.f * PANEL_H;
                        const float* base = (i == 4 || i == 8)
                                              ? ((armed[hnd] && h.pinching) ? status_green : tip)
                                              : lm_col;
                        float col[4] = { base[0], base[1], base[2], ov_alpha };
                        QuadDraw& ld = draws[ndraw++];
                        memcpy(ld.mvp, panel_mvp, sizeof(panel_mvp));
                        memcpy(ld.color, col, 4 * sizeof(float));
                        ld.rect[0] = lx; ld.rect[1] = ly; ld.rect[2] = lm_r; ld.rect[3] = lm_r;
                        ld.textured = false;
                        ld.circle = true;
                    }
                }
            }
```

If the pinch-status dot block just above (lines ~907-931, the one that sets `pd.circle = true`) references the legacy `gev.pinching`/`gev.present`, update it to reflect either hand: use `bool any_present = gev.left.present || gev.right.present;` and `bool any_pinch_armed = (armed[0] && gev.left.pinching) || (armed[1] && gev.right.pinching);` in place of the single-hand `gev.present` / `gestures_armed && gev.pinching`. (Confirm and adapt that block at implementation — it is the grey/amber/blue/green status dot.)

- [ ] **Step 3: Remove the legacy fields** — in `src/gesture_client.h`, delete the "legacy single-hand view" fields from `GestureEvent`, leaving:

```cpp
// Both hands from one event. See gestures/protocol.py encode_event.
struct GestureEvent {
    HandState left, right;
};
```

- [ ] **Step 4: Stop populating them** — in `src/gesture_parse.cpp` `parse_event`, delete the primary-hand copy block, leaving:

```cpp
GestureEvent parse_event(const std::string& line) {
    GestureEvent ev;
    ev.left = parse_hand(hand_object(line, "left"));
    ev.right = parse_hand(hand_object(line, "right"));
    return ev;
}
```

Remove the now-unused `#include <cstring>` only if nothing else uses it (the landmark scanner still may — leave it if in doubt).

- [ ] **Step 5: Verify build + parser test still pass**

Run: `cd spatial-screens && make gesture-parse-test && ./gesture-parse-test && make spatial-screens 2>&1 | tail -5`
Expected: `gesture_parse_test: all checks passed` and a clean build (no references to removed `gev.present`/`gev.pinching`/etc. — if the compiler flags any, they are single-hand leftovers to migrate to `gev.left`/`gev.right`).

- [ ] **Step 6: Commit**

```bash
git add spatial-screens/src/main.cpp spatial-screens/src/gesture_client.h spatial-screens/src/gesture_parse.cpp
git commit -m "spatial-screens: draw both hands in the overlay; drop legacy single-hand view"
```

---

### Task 10: Integration build, full test run, docs

Build everything, run all unit tests together, update the branch resume doc, and record the hardware-verification checklist.

**Files:**
- Modify: `docs/branches/feat-two-hand-gestures.md` (progress + hardware checklist)

- [ ] **Step 1: Full clean build + all unit tests**

Run:
```bash
cd spatial-screens && make clean && make spatial-screens && make test
cd gestures && python3 -m pytest tests/ -v
```
Expected: spatial-screens links; `gesture_parse_test: all checks passed`; `gesture_manip_test: all checks passed`; all pytest pass.

- [ ] **Step 2: Update the branch resume doc** — in `docs/branches/feat-two-hand-gestures.md`, tick the implementation box and fill in the hardware checklist under "Current state / next step":

```markdown
- [x] Design doc written + approved (2026-07-06)
- [x] Implementation plan (`docs/specs/2026-07-06-two-hand-gestures-plan.md`)
- [x] Implementation — all unit tests green (parser, grab math, protocol, classify)
- [ ] Hardware verification pass:
      - [ ] Confirm `MIRROR_HANDEDNESS` value (does the left hand light up the
            left overlay?). Flip the constant in `gestures/classify.py` if wrong.
      - [ ] Measure two-pass inference latency at 15 Hz; if it overruns, switch
            the sidecar to one plane + `num_hands=2` (both hands from left frame).
      - [ ] Tune `GRAB_REPOSITION_GAIN`, `GRAB_DIAG_MIN/MAX` for grab feel.
      - [ ] Check resize doesn't drift from cross-camera parallax; if it does,
            source both grab pinch points from the left frame during a grab.
- [ ] Merge to master
```

- [ ] **Step 3: Commit**

```bash
git add docs/branches/feat-two-hand-gestures.md
git commit -m "docs: two-hand gestures implemented — unit tests green, hardware pass pending"
```

- [ ] **Step 4: Hand off for the hardware pass** — the remaining verification requires the glasses (single-SDK-client: no viture-bridge / other spatial-screens session running). Per the hardware-test division of labor, run the glasses session end-to-end on the user's "go":

```bash
cd spatial-screens && ./run.sh
```
Watch for: sidecar connects, both hands appear in the overlay with correct handedness, single-hand gestures work with either hand, and a two-hand pinch resizes + repositions the screen. Record results in the branch doc and the testing log.

---

## Self-Review

**Spec coverage:**
- Both hands tracked → Tasks 6 (sidecar num_hands=2 ×2), 4 (parse both), 8 (state machine). ✓
- Either hand does single-hand gestures → Task 8 (per-hand loop, first qualifying armed hand). ✓
- Two-hand grab resize (spread) + reposition (midpoint) → Task 5 (pure math) + Task 8 (wiring). ✓
- Per-hand arming (each open palm arms its hand) → Task 8. ✓
- Left hand ← left camera, right hand ← right camera → Task 6 (`select_hand` per plane) + Task 7 (send both planes). ✓
- Multi-plane frame protocol → Task 1. ✓
- Two-hand event schema → Task 2. ✓
- Handedness mirror flag → Task 3 (`MIRROR_HANDEDNESS`). ✓
- Overlay both hands + draw cap → Task 9. ✓
- 2×-inference fallback documented → Task 10 checklist. ✓
- Testing (Python protocol/selection, C++ parser, pure grab math, hardware) → Tasks 1-6 tests + Task 10. ✓
- Non-goals honored: no stereo fusion, no rotation, lateral-only reposition (grab_update moves only in right0/up0 plane, orientation untouched). ✓

**Placeholder scan:** No TBD/TODO in code steps; the one deferred detail (exact `draws[]` array name/size and the status-dot block adaptation in Task 9) is bounded by a `grep` command and explicit criteria, not an open-ended "handle it". Every code step shows complete code.

**Type consistency:** `HandState`/`GestureEvent{left,right}` defined in Task 4, consumed identically in Tasks 8-9. `grab_begin`/`grab_update`/`GrabState`/`GrabResult`/`GVec3` defined in Task 5, consumed in Task 8. `encode_event(t,left,right)` defined in Task 2, called in Task 6. `read_frame`→planes list defined in Task 1, consumed in Task 6. `maybe_send_frame(left,right,w,h,ts)` defined in Task 7, called from `on_camera_carina` same task. `select_hand` defined in Task 3, called in Task 6. Consistent throughout.
