# Hand Gesture Control for spatial-screens Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `spatial-screens` pinch-drag (distance/size) and fist-hold (recenter) gesture control, sourced from the Luma Ultra's tracking camera via a new Python/MediaPipe sidecar process.

**Architecture:** `spatial-screens` (the only process allowed to hold the SDK's device handle) registers the SDK's `camera_callback` and forwards raw grayscale frames over a Unix domain socket it listens on; a spawned Python sidecar (`spatial-screens/gestures/hand_tracker.py`) runs MediaPipe Hands and sends back one JSON gesture event per processed frame; `spatial-screens` polls the latest event each render frame and maps it onto the existing distance/size/recenter state.

**Tech Stack:** C++17 (existing `spatial-screens`), Python 3 + MediaPipe + OpenCV + NumPy (new sidecar), Unix domain socket IPC, pytest for the Python-side pure logic.

**Spec:** [`2026-07-03-hand-gesture-control-design.md`](2026-07-03-hand-gesture-control-design.md) — read it first; this plan implements it milestone-by-milestone (M1-M4).

## Global Constraints

- Linux x86_64 only (repo-wide).
- C++17, following `spatial-screens/src/main.cpp`'s existing style: free functions and file-scope statics for SDK callbacks (they're plain C function pointers with no userdata param), state as locals inside `main()`, not new globals, unless a C callback needs file-scope access.
- No C++ test framework in this repo (`CLAUDE.md`: "No C++ tests, linter, or formatter for bridge/" — the same applies to `spatial-screens`). C++ tasks below use manual build-and-run verification, not TDD red/green steps.
- Python code gets real TDD (pytest) wherever it's pure logic (`protocol.py`, `classify.py`). The sidecar's I/O loop (`hand_tracker.py`) is glue code verified manually, same as the C++ side.
- The SDK allows exactly **one process** to hold the device open at a time. Stop `viture-bridge` before running `spatial-screens` for any hardware-verification step below.
- `max_num_hands=1` in MediaPipe — single-hand tracking only, per the spec's non-goals.
- Gesture vocabulary is exactly: pinch-drag → distance/size, fist-hold (~0.5s) → recenter. No gesture maps to quit.
- `GESTURE_INFER_HZ = 15` and `PINCH_THRESHOLD = 0.5f` are starting guesses (per spec); both are named constants specifically so they're easy to retune after hands-on testing in Tasks 7 and 9.
- Camera pixel format is assumed **GRAY8** (1 byte/pixel) based on the phase-2 doc's "2x grayscale tracking cameras" finding. Task 1 verifies this empirically on hardware; if it turns out wrong, Task 1's step 4 says exactly what to change before continuing to Task 2.

## File Structure

New files:
- `spatial-screens/src/gesture_client.h` / `.cpp` — `GestureClient`: owns the listening socket, spawns/monitors the sidecar, forwards frames, parses gesture events. The only new C++ surface `main.cpp` talks to.
- `spatial-screens/gestures/protocol.py` — wire format encode/decode (frame header, gesture event JSON). Pure functions, no I/O.
- `spatial-screens/gestures/classify.py` — landmark math (`pinch_norm`, `pinch_pos`, `classify_pose`). Pure functions, no MediaPipe/I/O dependency.
- `spatial-screens/gestures/hand_tracker.py` — sidecar entry point: connects to the socket, runs MediaPipe, calls into `classify.py`/`protocol.py`.
- `spatial-screens/gestures/requirements.txt` — `mediapipe`, `opencv-python-headless`, `numpy`, `pytest`.
- `spatial-screens/gestures/conftest.py` — adds `gestures/` to `sys.path` so tests can `import protocol`/`import classify` regardless of invocation directory.
- `spatial-screens/gestures/tests/test_protocol.py`, `test_classify.py`.

Modified files:
- `spatial-screens/src/main.cpp` — camera callback registration, `GestureClient` wiring, gesture→action mapping in the render loop.
- `spatial-screens/Makefile` — compile `gesture_client.cpp` alongside `main.cpp`.
- `spatial-screens/README.md` — document gesture controls and sidecar setup.
- `CLAUDE.md` — note the new Python dependency.

---

### Task 1: Camera format probe (M1)

**Files:**
- Modify: `spatial-screens/src/main.cpp`

**Interfaces:**
- Produces: `on_camera_carina(char*, char*, char*, char*, double, int, int)` — a free function matching `XRCameraCallback` (`sdk/include/viture_device_carina.h:50`), registered as the 5th argument to `register_callbacks_carina`. Task 5 will extend this same function; its signature doesn't change.

- [ ] **Step 1: Add the probe flag and camera callback**

In `spatial-screens/src/main.cpp`, add near the other SDK-glue statics (after line 96, `static bool g_want_sgi = false;`):

```cpp
static bool g_probe_camera = false;
static int g_probe_frames_remaining = 0;
```

Add the callback function after `on_pose_noop` (after line 99):

```cpp
static void on_camera_carina(char* image_left0, char* /*image_right0*/,
                              char* /*image_left1*/, char* /*image_right1*/,
                              double timestamp, int width, int height) {
    if (!g_probe_camera || g_probe_frames_remaining <= 0) return;
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
```

- [ ] **Step 2: Wire the callback and the CLI flag**

In `main()`'s argument-parsing loop (around line 210, right after the `--smooth-ori` branch), add:

```cpp
        else if (!strcmp(argv[i], "--probe-camera")) { g_probe_camera = true; g_probe_frames_remaining = 10; }
```

Change the `register_callbacks_carina` call at line 147 from:

```cpp
    if (register_callbacks_carina(g_provider, on_pose_noop, nullptr, on_imu_noop, nullptr) != 0 ||
```

to:

```cpp
    if (register_callbacks_carina(g_provider, on_pose_noop, nullptr, on_imu_noop, on_camera_carina) != 0 ||
```

- [ ] **Step 3: Build and run the probe on hardware**

```bash
cd /home/salvatore/Desktop/code/viture/spatial-screens
make
# stop viture-bridge first if it's running — the SDK allows one client
./run.sh --probe-camera --capture test
```

Watch the console for 10 lines like `gestures: probe frame -> /tmp/spatial-screens-probe-009.pgm (WxH, t=...)`. Once they appear, press `Ctrl+Alt+Q` (global hotkey) to quit.

- [ ] **Step 4: Inspect the dumped frames and record the format**

```bash
file /tmp/spatial-screens-probe-000.pgm
xdg-open /tmp/spatial-screens-probe-005.pgm   # or: eog, gimp, display
```

Expected: a valid grayscale image showing a plausible camera view (whatever the tracking camera saw — e.g. the room, or your hand if you held it up during the probe). Note the exact `W×H` printed on the console.

**Contingency:** if the file size printed by `file` is exactly `W×H` bytes, GRAY8 was correct — no changes needed, continue to Task 2 as written. If the file size is `2×W×H` bytes (image won't open as a clean grayscale image; looks like noise or a half-width smear when forced), the format is packed YUV 4:2:2, not GRAY8 — before continuing, change `FORMAT_GRAY8 = 0` to a new `FORMAT_YUYV = 1` throughout Task 2/4/7's code below, and add a `cv2.cvtColor(frame, cv2.COLOR_YUV2RGB_YUYV)` conversion in Task 7's `hand_tracker.py` instead of the GRAY2RGB conversion.

- [ ] **Step 5: Commit**

```bash
git add spatial-screens/src/main.cpp
git commit -m "$(cat <<'EOF'
spatial-screens: probe camera_callback frame format for gesture control

Dumps the first 10 image_left0 frames to /tmp as PGM behind a
--probe-camera flag, to empirically determine pixel format/resolution
before building the gesture pipeline (docs/specs/2026-07-03-hand-gesture-control-plan.md, M1).
EOF
)"
```

---

### Task 2: Frame/event wire protocol — `protocol.py` (M2)

**Files:**
- Create: `spatial-screens/gestures/protocol.py`
- Create: `spatial-screens/gestures/conftest.py`
- Test: `spatial-screens/gestures/tests/test_protocol.py`

**Interfaces:**
- Produces: `read_frame(read_exact) -> (timestamp: float, width: int, height: int, fmt: int, data: bytes) | None`, `encode_frame(timestamp, width, height, fmt, data) -> bytes`, `encode_event(t, present, handedness, landmarks, pinch_norm, pinch_pos, pose) -> bytes`. `read_exact` is a caller-supplied `callable(n: int) -> bytes | None` (returns `None` on clean EOF, raises on real I/O errors) — this indirection lets tests drive `read_frame` from an in-memory buffer instead of a real socket.
- Consumes: nothing (pure).

- [ ] **Step 1: Write the failing tests**

Create `spatial-screens/gestures/conftest.py`:

```python
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
```

Create `spatial-screens/gestures/tests/test_protocol.py`:

```python
import io
import json

from protocol import encode_event, encode_frame, read_frame


def _reader_from_bytes(data):
    buf = io.BytesIO(data)

    def read_exact(n):
        chunk = buf.read(n)
        return chunk if chunk else None

    return read_exact


def test_encode_then_read_frame_round_trips():
    raw = b"\x01\x02\x03\x04\x05\x06"
    msg = encode_frame(1.5, 3, 2, 0, raw)

    t, w, h, fmt, data = read_frame(_reader_from_bytes(msg))

    assert (t, w, h, fmt, data) == (1.5, 3, 2, 0, raw)


def test_read_frame_returns_none_on_clean_eof():
    assert read_frame(_reader_from_bytes(b"")) is None


def test_encode_event_produces_newline_delimited_json():
    msg = encode_event(
        t=1.0, present=True, handedness="left",
        landmarks=[(0.1, 0.2)] * 21,
        pinch_norm=0.3, pinch_pos=(0.15, 0.2), pose="fist",
    )

    assert msg.endswith(b"\n")
    obj = json.loads(msg.decode("utf-8").strip())
    assert obj == {
        "type": "hand", "t": 1.0, "present": True, "handedness": "left",
        "landmarks": [[0.1, 0.2]] * 21,
        "pinch_norm": 0.3, "pinch_pos": [0.15, 0.2], "pose": "fist",
    }
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cd /home/salvatore/Desktop/code/viture/spatial-screens/gestures
python3 -m pytest tests/test_protocol.py -v
```

Expected: `ModuleNotFoundError: No module named 'protocol'` (the module doesn't exist yet).

- [ ] **Step 3: Implement `protocol.py`**

```python
"""Wire protocol between spatial-screens (C++) and the gesture sidecar.

Frame forwarding (spatial-screens -> sidecar), length-prefixed binary:
    [u32 len][f64 timestamp][i32 width][i32 height][u8 format][raw bytes]

Gesture events (sidecar -> spatial-screens), newline-delimited JSON, one
object per processed frame. See
docs/specs/2026-07-03-hand-gesture-control-design.md for the design
rationale.
"""
import json
import struct

_LENGTH_PREFIX = struct.Struct("<I")
_FRAME_HEADER = struct.Struct("<dii B")  # timestamp, width, height, format


def read_frame(read_exact):
    """Read one frame message via read_exact(n) -> bytes|None.

    Returns (timestamp, width, height, format, data), or None on clean EOF
    (read_exact returned None while reading the length prefix).
    """
    length_bytes = read_exact(_LENGTH_PREFIX.size)
    if length_bytes is None:
        return None
    (length,) = _LENGTH_PREFIX.unpack(length_bytes)
    payload = read_exact(length)
    timestamp, width, height, fmt = _FRAME_HEADER.unpack(payload[: _FRAME_HEADER.size])
    data = payload[_FRAME_HEADER.size :]
    return timestamp, width, height, fmt, data


def encode_frame(timestamp, width, height, fmt, data):
    """Inverse of read_frame — used by spatial-screens (conceptually; the
    C++ side has its own byte-for-byte equivalent) and by tests here to
    build synthetic input."""
    payload = _FRAME_HEADER.pack(timestamp, width, height, fmt) + data
    return _LENGTH_PREFIX.pack(len(payload)) + payload


def encode_event(t, present, handedness, landmarks, pinch_norm, pinch_pos, pose):
    """One gesture event as newline-delimited JSON.

    pinch_pos is the normalized [0,1] midpoint of the thumb-tip/index-tip
    landmarks — a convenience field (not derivable cheaply on the C++ side
    without a full JSON array parser) so the C++ consumer can track
    pinch-drag deltas without parsing the full 21-point landmarks array.
    """
    obj = {
        "type": "hand",
        "t": t,
        "present": present,
        "handedness": handedness,
        "landmarks": [list(p) for p in landmarks],
        "pinch_norm": pinch_norm,
        "pinch_pos": list(pinch_pos),
        "pose": pose,
    }
    return (json.dumps(obj) + "\n").encode("utf-8")
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cd /home/salvatore/Desktop/code/viture/spatial-screens/gestures
python3 -m pytest tests/test_protocol.py -v
```

Expected: 3 passed.

- [ ] **Step 5: Commit**

```bash
cd /home/salvatore/Desktop/code/viture/spatial-screens
git add gestures/protocol.py gestures/conftest.py gestures/tests/test_protocol.py
git commit -m "$(cat <<'EOF'
spatial-screens/gestures: add frame/event wire protocol

Length-prefixed binary framing for camera frames, newline-delimited
JSON for gesture events, per docs/specs/2026-07-03-hand-gesture-control-design.md.
EOF
)"
```

---

### Task 3: Gesture sidecar skeleton — `hand_tracker.py --echo` (M2)

**Files:**
- Create: `spatial-screens/gestures/hand_tracker.py`

**Interfaces:**
- Consumes: `protocol.read_frame`, `protocol.encode_event` (Task 2).
- Produces: a runnable script `hand_tracker.py --socket PATH [--echo]`. `--echo` mode never imports MediaPipe/OpenCV — it exists specifically so the IPC pipeline (this task + Task 4/5) can be verified before the heavier MediaPipe dependency is wired in (Task 7).

No automated test here — this is I/O glue (socket connect/loop), verified manually once Task 5 wires it into `spatial-screens`. (`classify.py`/`protocol.py` carry the automated tests; this file is the thin script gluing them together, same split the design spec's Testing section describes.)

- [ ] **Step 1: Write `hand_tracker.py`**

```python
#!/usr/bin/env python3
"""Hand-gesture sidecar for spatial-screens.

Connects to the Unix domain socket spatial-screens listens on, receives raw
camera frames, and sends back one JSON gesture event per frame. See
docs/specs/2026-07-03-hand-gesture-control-design.md for the wire protocol.

Standalone testing: python3 hand_tracker.py --socket /tmp/test.sock --echo
"""
import argparse
import socket
import sys
import time

from protocol import encode_event, read_frame

FORMAT_GRAY8 = 0


def connect(socket_path, retries=20, delay=0.25):
    for _ in range(retries):
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.connect(socket_path)
            return sock
        except (FileNotFoundError, ConnectionRefusedError):
            time.sleep(delay)
    raise RuntimeError(f"could not connect to {socket_path} after {retries} retries")


def make_reader(sock):
    def read_exact(n):
        chunks = []
        remaining = n
        while remaining > 0:
            chunk = sock.recv(remaining)
            if not chunk:
                return None
            chunks.append(chunk)
            remaining -= len(chunk)
        return b"".join(chunks)
    return read_exact


def _no_hand_event(timestamp):
    return encode_event(timestamp, False, "", [(0.0, 0.0)] * 21, 999.0, (0.0, 0.0), "none")


def run_echo(sock, read_exact):
    frame_count = 0
    while True:
        frame = read_frame(read_exact)
        if frame is None:
            break
        timestamp, width, height, fmt, _data = frame
        frame_count += 1
        print(f"echo: frame {frame_count} {width}x{height} fmt={fmt}", file=sys.stderr)
        sock.sendall(_no_hand_event(timestamp))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", required=True)
    parser.add_argument("--echo", action="store_true",
                         help="skip MediaPipe; just acknowledge frames (IPC smoke test)")
    args = parser.parse_args()

    sock = connect(args.socket)
    read_exact = make_reader(sock)

    if args.echo:
        run_echo(sock, read_exact)
        return

    print("hand_tracker: real inference not wired up yet (see Task 7)", file=sys.stderr)
    run_echo(sock, read_exact)


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Manually smoke-test the skeleton against a throwaway listener**

```bash
cd /home/salvatore/Desktop/code/viture/spatial-screens/gestures
python3 - <<'EOF'
import socket, os, struct, subprocess, time

sock_path = "/tmp/hand-tracker-smoke.sock"
if os.path.exists(sock_path):
    os.remove(sock_path)
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(sock_path)
srv.listen(1)

proc = subprocess.Popen(["python3", "hand_tracker.py", "--socket", sock_path, "--echo"])
conn, _ = srv.accept()

header = struct.pack("<dii B", 1.0, 4, 2, 0) + bytes(8)  # 4x2 fake GRAY8 frame
conn.sendall(struct.pack("<I", len(header)) + header)

print("event from sidecar:", conn.recv(4096))
proc.terminate()
proc.wait()
EOF
```

Expected: prints `event from sidecar: b'{"type": "hand", ... "present": false ...}\n'`, and the sidecar's stderr (visible in the terminal) shows `echo: frame 1 4x2 fmt=0`.

- [ ] **Step 3: Commit**

```bash
cd /home/salvatore/Desktop/code/viture/spatial-screens
git add gestures/hand_tracker.py
git commit -m "$(cat <<'EOF'
spatial-screens/gestures: add sidecar skeleton with --echo smoke-test mode

Connects to the spatial-screens socket and round-trips frames as
no-hand-detected events, with no MediaPipe dependency yet — validates
the IPC plumbing in isolation before Task 7 wires in real inference.
EOF
)"
```

---

### Task 4: `GestureClient` C++ class (M2)

**Files:**
- Create: `spatial-screens/src/gesture_client.h`
- Create: `spatial-screens/src/gesture_client.cpp`
- Modify: `spatial-screens/Makefile`

**Interfaces:**
- Produces: `class GestureClient` with `bool start(const std::string& socket_path, const std::string& script_path, double connect_timeout_s = 5.0)`, `void maybe_send_frame(const uint8_t* gray8, int width, int height, double timestamp)`, `GestureEvent poll()`, `void stop()`, `bool enabled() const`. `struct GestureEvent { bool present; bool pinching; float pinch_x, pinch_y; std::string pose; }`.
- Consumes: the wire protocol from Task 2 (mirrored in C++, since there's no shared codegen between the two languages here — the byte layout must match `protocol.py`'s `_FRAME_HEADER`/`_LENGTH_PREFIX` exactly).

No automated test (no C++ test framework in this repo, per Global Constraints). Manual verification happens in Task 5 once this is wired into `main.cpp`.

- [ ] **Step 1: Write `gesture_client.h`**

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <sys/types.h>

// Hand-gesture events from the Python/MediaPipe sidecar. See
// spatial-screens/gestures/ and
// docs/specs/2026-07-03-hand-gesture-control-design.md.
//
// Optional feature: if the sidecar can't be started, GestureClient reports
// enabled() == false and poll() always returns a default (not-present)
// event. Callers must not treat gestures as a dependency.
struct GestureEvent {
    bool present = false;
    bool pinching = false;   // pinch_norm < PINCH_THRESHOLD
    float pinch_x = 0.f, pinch_y = 0.f; // normalized [0,1] pinch midpoint
    std::string pose;        // "open_palm" | "fist" | "point" | "none" | ""
};

class GestureClient {
public:
    // Starts listening on socket_path, spawns `python3 script_path --socket
    // socket_path`, and waits up to connect_timeout_s for it to connect.
    // Returns false (and logs why) on any failure; the object stays safely
    // usable in the disabled state either way.
    bool start(const std::string& socket_path, const std::string& script_path,
               double connect_timeout_s = 5.0);

    // Rate-limited to GESTURE_INFER_HZ; safe to call every render frame.
    // gray8 must be width*height bytes, one byte per pixel.
    void maybe_send_frame(const uint8_t* gray8, int width, int height, double timestamp);

    // Non-blocking. Drains all buffered events and returns the newest;
    // returns the last-known event (or a default one) if nothing new
    // arrived since the last poll(), or if gestures are disabled.
    GestureEvent poll();

    // Terminates the sidecar and closes the socket. Safe to call even if
    // start() failed or was never called.
    void stop();

    bool enabled() const { return enabled_; }

private:
    bool enabled_ = false;
    int listen_fd_ = -1;
    int conn_fd_ = -1;
    pid_t child_pid_ = -1;
    double last_send_s_ = 0.0;
    GestureEvent last_event_;
    std::string recv_buf_;
};
```

- [ ] **Step 2: Write `gesture_client.cpp`**

```cpp
#include "gesture_client.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

constexpr double GESTURE_INFER_HZ = 15.0;
constexpr float PINCH_THRESHOLD = 0.5f; // tune after Task 9's hands-on test

double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Minimal key-based scanners for the fixed, known event schema (see
// protocol.py's encode_event). Not a general JSON parser — deliberately
// so, to avoid adding a JSON library dependency for a 5-field schema we
// control on both ends.
bool json_find_bool(const std::string& s, const char* key, bool& out) {
    std::string pat = std::string("\"") + key + "\":";
    auto pos = s.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    out = s.compare(pos, 4, "true") == 0;
    return true;
}

bool json_find_number(const std::string& s, const char* key, float& out) {
    std::string pat = std::string("\"") + key + "\":";
    auto pos = s.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    out = strtof(s.c_str() + pos, nullptr);
    return true;
}

bool json_find_string(const std::string& s, const char* key, std::string& out) {
    std::string pat = std::string("\"") + key + "\":\"";
    auto pos = s.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    auto end = s.find('"', pos);
    if (end == std::string::npos) return false;
    out = s.substr(pos, end - pos);
    return true;
}

// pinch_pos is emitted as "pinch_pos":[x,y] — json_find_number would stop
// at the array's leading '[', so it needs its own extractor.
bool json_find_pair(const std::string& s, const char* key, float& x, float& y) {
    std::string pat = std::string("\"") + key + "\":[";
    auto pos = s.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    x = strtof(s.c_str() + pos, nullptr);
    auto comma = s.find(',', pos);
    if (comma == std::string::npos) return false;
    y = strtof(s.c_str() + comma + 1, nullptr);
    return true;
}

GestureEvent parse_event(const std::string& line) {
    GestureEvent ev;
    json_find_bool(line, "present", ev.present);
    float pinch_norm = 999.f;
    json_find_number(line, "pinch_norm", pinch_norm);
    ev.pinching = ev.present && pinch_norm < PINCH_THRESHOLD;
    json_find_pair(line, "pinch_pos", ev.pinch_x, ev.pinch_y);
    json_find_string(line, "pose", ev.pose);
    return ev;
}

} // namespace

bool GestureClient::start(const std::string& socket_path, const std::string& script_path,
                           double connect_timeout_s) {
    unlink(socket_path.c_str());

    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        fprintf(stderr, "gestures: socket() failed: %s\n", strerror(errno));
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(listen_fd_, 1) != 0) {
        fprintf(stderr, "gestures: bind/listen failed: %s\n", strerror(errno));
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    char* argv[] = {
        const_cast<char*>("python3"),
        const_cast<char*>(script_path.c_str()),
        const_cast<char*>("--socket"),
        const_cast<char*>(socket_path.c_str()),
        nullptr,
    };
    int rc = posix_spawnp(&child_pid_, "python3", nullptr, nullptr, argv, environ);
    if (rc != 0) {
        fprintf(stderr, "gestures: failed to spawn sidecar (%s) — gesture control disabled\n",
                strerror(rc));
        child_pid_ = -1;
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    pollfd pfd{ listen_fd_, POLLIN, 0 };
    int pres = poll(&pfd, 1, int(connect_timeout_s * 1000));
    if (pres <= 0) {
        fprintf(stderr, "gestures: sidecar did not connect within %.1fs — gesture control disabled\n",
                connect_timeout_s);
        stop();
        return false;
    }
    conn_fd_ = accept(listen_fd_, nullptr, nullptr);
    if (conn_fd_ < 0) {
        fprintf(stderr, "gestures: accept() failed: %s\n", strerror(errno));
        stop();
        return false;
    }
    fcntl(conn_fd_, F_SETFL, O_NONBLOCK);

    enabled_ = true;
    printf("gestures: sidecar connected (%s)\n", script_path.c_str());
    return true;
}

void GestureClient::maybe_send_frame(const uint8_t* gray8, int width, int height, double timestamp) {
    if (!enabled_) return;
    double t = now_s();
    if (t - last_send_s_ < 1.0 / GESTURE_INFER_HZ) return;
    last_send_s_ = t;

    const size_t data_len = size_t(width) * size_t(height);
    const uint32_t payload_len = uint32_t(8 + 4 + 4 + 1 + data_len);

    std::string msg;
    msg.resize(4 + payload_len);
    uint8_t* p = reinterpret_cast<uint8_t*>(&msg[0]);
    memcpy(p, &payload_len, 4);             p += 4;
    memcpy(p, &timestamp, 8);               p += 8;
    int32_t w = width, h = height;
    memcpy(p, &w, 4);                       p += 4;
    memcpy(p, &h, 4);                       p += 4;
    *p = 0; /* format: GRAY8, per Task 1 */ p += 1;
    memcpy(p, gray8, data_len);

    ssize_t sent = send(conn_fd_, msg.data(), msg.size(), MSG_NOSIGNAL);
    if (sent < 0) {
        fprintf(stderr, "gestures: send() failed (%s) — disabling gesture control\n", strerror(errno));
        enabled_ = false;
    }
}

GestureEvent GestureClient::poll() {
    if (!enabled_) return last_event_;

    char buf[8192];
    for (;;) {
        ssize_t n = recv(conn_fd_, buf, sizeof(buf), 0);
        if (n > 0) {
            recv_buf_.append(buf, size_t(n));
            continue;
        }
        if (n == 0) {
            fprintf(stderr, "gestures: sidecar closed the connection — disabling gesture control\n");
            enabled_ = false;
            break;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "gestures: recv() failed (%s) — disabling gesture control\n", strerror(errno));
            enabled_ = false;
        }
        break;
    }

    size_t nl;
    while ((nl = recv_buf_.find('\n')) != std::string::npos) {
        last_event_ = parse_event(recv_buf_.substr(0, nl));
        recv_buf_.erase(0, nl + 1);
    }
    return last_event_;
}

void GestureClient::stop() {
    if (child_pid_ > 0) {
        kill(child_pid_, SIGTERM);
        int status;
        for (int i = 0; i < 10; i++) {
            if (waitpid(child_pid_, &status, WNOHANG) != 0) { child_pid_ = -1; break; }
            usleep(100 * 1000);
        }
        if (child_pid_ > 0) { kill(child_pid_, SIGKILL); waitpid(child_pid_, &status, 0); }
        child_pid_ = -1;
    }
    if (conn_fd_ >= 0) { close(conn_fd_); conn_fd_ = -1; }
    if (listen_fd_ >= 0) { close(listen_fd_); listen_fd_ = -1; }
    enabled_ = false;
}
```

- [ ] **Step 3: Update the Makefile to build both source files**

In `spatial-screens/Makefile`, replace:

```make
spatial-screens: src/main.cpp
	$(CXX) $(CXXFLAGS) src/main.cpp -o $@ $(LDFLAGS)
```

with:

```make
SRCS := src/main.cpp src/gesture_client.cpp

spatial-screens: $(SRCS) src/gesture_client.h
	$(CXX) $(CXXFLAGS) $(SRCS) -o $@ $(LDFLAGS)
```

- [ ] **Step 4: Build (compile-only check — GestureClient isn't called from main() yet)**

```bash
cd /home/salvatore/Desktop/code/viture/spatial-screens
make
```

Expected: builds cleanly with no warnings from `gesture_client.cpp` (it's compiled but unused — that's fine, Task 5 wires it in).

- [ ] **Step 5: Commit**

```bash
git add src/gesture_client.h src/gesture_client.cpp Makefile
git commit -m "$(cat <<'EOF'
spatial-screens: add GestureClient (socket listen, sidecar spawn, event parsing)

Not wired into main() yet — Task 5 does that. Compiles standalone so this
lands as a reviewable, self-contained unit.
EOF
)"
```

---

### Task 5: Wire `GestureClient` into `spatial-screens` end-to-end (M2 exit criteria)

**Files:**
- Modify: `spatial-screens/src/main.cpp`

**Interfaces:**
- Consumes: `GestureClient`, `GestureEvent` (Task 4); extends `on_camera_carina` (Task 1).

- [ ] **Step 1: Include the header and add the global instance**

Add after line 45 (`#include "viture_device_carina.h"`):

```cpp
#include "gesture_client.h"
```

Add near `static XRDeviceProviderHandle g_provider = nullptr;` (line 94):

```cpp
static GestureClient g_gestures;
```

- [ ] **Step 2: Extend `on_camera_carina` to forward frames to GestureClient**

Replace the Task 1 body of `on_camera_carina` with:

```cpp
static void on_camera_carina(char* image_left0, char* /*image_right0*/,
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
    g_gestures.maybe_send_frame(reinterpret_cast<uint8_t*>(image_left0), width, height, timestamp);
}
```

- [ ] **Step 3: Add a helper to locate the sidecar script and start GestureClient after SDK init**

Add before `main()` (after `list_outputs`, around line 180):

```cpp
static std::string executable_dir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
    std::string path(buf);
    auto slash = path.find_last_of('/');
    return slash == std::string::npos ? "." : path.substr(0, slash);
}
```

Right after `if (!sdk_init()) return 1;` (line 360), add:

```cpp
    std::string gesture_socket = "/tmp/spatial-screens-gestures-" + std::to_string(getpid()) + ".sock";
    g_gestures.start(gesture_socket, executable_dir() + "/gestures/hand_tracker.py");
```

- [ ] **Step 4: Poll gestures each frame and stop on shutdown**

In the main loop, right after the `while (XPending(dpy)) { ... }` input block ends (after line 425), add:

```cpp
        // ---- gestures (state wired up in Task 8/9)
        GestureEvent gev = g_gestures.poll();
```

At shutdown, right before `xr_device_provider_stop(g_provider);` (line 586), add:

```cpp
    g_gestures.stop();
```

- [ ] **Step 5: Build and manually verify the IPC pipeline end-to-end**

```bash
cd /home/salvatore/Desktop/code/viture/spatial-screens
make
./run.sh --capture test
```

Expected console output: `gestures: sidecar connected (.../gestures/hand_tracker.py)` shortly after the SDK starts, and no `gestures: ...disabled` errors. Quit with `Ctrl+Alt+Q`; the process should exit cleanly (no hang, no zombie `python3` process — check with `ps aux | grep hand_tracker` after quitting, expect no output).

- [ ] **Step 6: Manually verify graceful degradation when the sidecar is missing**

```bash
mv gestures/hand_tracker.py gestures/hand_tracker.py.bak
./run.sh --capture test
```

Expected: `gestures: sidecar did not connect within 5.0s — gesture control disabled` printed once, then the renderer runs normally (screen renders, hotkeys work) with no crash. Quit, then restore the file:

```bash
mv gestures/hand_tracker.py.bak gestures/hand_tracker.py
```

- [ ] **Step 7: Commit**

```bash
git add src/main.cpp
git commit -m "$(cat <<'EOF'
spatial-screens: wire GestureClient into the render loop

Completes M2: camera frames flow to the sidecar and gesture events flow
back, end-to-end. Verified hardware runs survive both the sidecar's
absence and a missing-script failure without disrupting rendering.
EOF
)"
```

---

### Task 6: Hand landmark classification — `classify.py` (M3)

**Files:**
- Create: `spatial-screens/gestures/classify.py`
- Test: `spatial-screens/gestures/tests/test_classify.py`

**Interfaces:**
- Produces: `pinch_norm(landmarks) -> float`, `pinch_pos(landmarks) -> (float, float)`, `classify_pose(landmarks) -> str` (one of `"fist" | "open_palm" | "point" | "none"`). `landmarks` is a 21-element list of `(x, y)` tuples, MediaPipe's normalized-image-coordinate convention (origin top-left, y increases downward) — matches `result.multi_hand_landmarks[0].landmark[i].x/.y` in Task 7.
- Consumes: nothing (pure).

- [ ] **Step 1: Write the failing tests**

Create `spatial-screens/gestures/tests/test_classify.py`:

```python
import pytest

from classify import (
    INDEX_PIP, INDEX_TIP, MIDDLE_MCP, MIDDLE_PIP, MIDDLE_TIP,
    PINKY_PIP, PINKY_TIP, RING_PIP, RING_TIP, THUMB_TIP, WRIST,
    classify_pose, pinch_norm, pinch_pos,
)


def make_landmarks(overrides):
    lm = [(0.0, 0.0)] * 21
    for idx, pos in overrides.items():
        lm[idx] = pos
    return lm


OPEN_PALM = make_landmarks({
    WRIST: (0.5, 0.9),
    MIDDLE_MCP: (0.5, 0.6),
    INDEX_PIP: (0.45, 0.5), INDEX_TIP: (0.45, 0.2),
    MIDDLE_PIP: (0.50, 0.5), MIDDLE_TIP: (0.50, 0.15),
    RING_PIP: (0.55, 0.5), RING_TIP: (0.55, 0.2),
    PINKY_PIP: (0.60, 0.55), PINKY_TIP: (0.60, 0.3),
    THUMB_TIP: (0.3, 0.6),
})

FIST = make_landmarks({
    WRIST: (0.5, 0.9),
    MIDDLE_MCP: (0.5, 0.6),
    INDEX_PIP: (0.48, 0.65), INDEX_TIP: (0.48, 0.78),
    MIDDLE_PIP: (0.50, 0.65), MIDDLE_TIP: (0.50, 0.78),
    RING_PIP: (0.52, 0.65), RING_TIP: (0.52, 0.78),
    PINKY_PIP: (0.54, 0.68), PINKY_TIP: (0.54, 0.80),
    THUMB_TIP: (0.45, 0.75),
})

POINT = make_landmarks({
    WRIST: (0.5, 0.9),
    MIDDLE_MCP: (0.5, 0.6),
    INDEX_PIP: (0.45, 0.5), INDEX_TIP: (0.45, 0.2),      # extended
    MIDDLE_PIP: (0.50, 0.65), MIDDLE_TIP: (0.50, 0.78),  # curled
    RING_PIP: (0.52, 0.65), RING_TIP: (0.52, 0.78),      # curled
    PINKY_PIP: (0.54, 0.68), PINKY_TIP: (0.54, 0.80),    # curled
    THUMB_TIP: (0.4, 0.7),
})


def test_open_palm_classified_correctly():
    assert classify_pose(OPEN_PALM) == "open_palm"


def test_fist_classified_correctly():
    assert classify_pose(FIST) == "fist"


def test_point_classified_correctly():
    assert classify_pose(POINT) == "point"


def test_pinch_norm_small_when_fingers_touching():
    touching = make_landmarks({
        WRIST: (0.5, 0.9), MIDDLE_MCP: (0.5, 0.6),
        THUMB_TIP: (0.5, 0.5), INDEX_TIP: (0.51, 0.5),
    })
    assert pinch_norm(touching) == pytest.approx(0.01 / 0.3)


def test_pinch_norm_large_when_fingers_apart():
    apart = make_landmarks({
        WRIST: (0.5, 0.9), MIDDLE_MCP: (0.5, 0.6),
        THUMB_TIP: (0.2, 0.5), INDEX_TIP: (0.8, 0.5),
    })
    assert pinch_norm(apart) == pytest.approx(2.0)


def test_pinch_pos_is_midpoint_of_tips():
    lm = make_landmarks({THUMB_TIP: (0.2, 0.4), INDEX_TIP: (0.4, 0.6)})
    assert pinch_pos(lm) == pytest.approx((0.3, 0.5))
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cd /home/salvatore/Desktop/code/viture/spatial-screens/gestures
python3 -m pytest tests/test_classify.py -v
```

Expected: `ModuleNotFoundError: No module named 'classify'`.

- [ ] **Step 3: Implement `classify.py`**

```python
"""Pure landmark math for gesture classification — no MediaPipe/I/O
dependency, so this is fully unit-testable with synthetic coordinates.

landmarks is a 21-element list of (x, y) tuples in MediaPipe's normalized
image coordinates (origin top-left, y increases downward), indexed per the
standard MediaPipe Hands landmark layout.
"""
import math

WRIST = 0
THUMB_TIP = 4
INDEX_MCP = 5
INDEX_PIP = 6
INDEX_TIP = 8
MIDDLE_MCP = 9
MIDDLE_PIP = 10
MIDDLE_TIP = 12
RING_PIP = 14
RING_TIP = 16
PINKY_PIP = 18
PINKY_TIP = 20


def _dist(a, b):
    return math.hypot(a[0] - b[0], a[1] - b[1])


def pinch_norm(landmarks):
    """Thumb-tip/index-tip distance normalized by wrist-to-middle-MCP palm
    size — a hand-size/distance-from-camera-invariant ratio. Smaller means
    a tighter pinch."""
    palm = _dist(landmarks[WRIST], landmarks[MIDDLE_MCP])
    if palm < 1e-6:
        return float("inf")
    return _dist(landmarks[THUMB_TIP], landmarks[INDEX_TIP]) / palm


def pinch_pos(landmarks):
    """Normalized [0,1] midpoint between thumb tip and index tip."""
    t, i = landmarks[THUMB_TIP], landmarks[INDEX_TIP]
    return ((t[0] + i[0]) / 2.0, (t[1] + i[1]) / 2.0)


def _finger_curled(landmarks, pip_idx, tip_idx):
    wrist = landmarks[WRIST]
    return _dist(landmarks[tip_idx], wrist) < _dist(landmarks[pip_idx], wrist)


def classify_pose(landmarks):
    """Classify a static hand pose from the four non-thumb fingers' curl."""
    fingers = [
        (INDEX_PIP, INDEX_TIP),
        (MIDDLE_PIP, MIDDLE_TIP),
        (RING_PIP, RING_TIP),
        (PINKY_PIP, PINKY_TIP),
    ]
    curled = [_finger_curled(landmarks, pip, tip) for pip, tip in fingers]
    if all(curled):
        return "fist"
    if not any(curled):
        return "open_palm"
    if not curled[0] and all(curled[1:]):
        return "point"
    return "none"
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cd /home/salvatore/Desktop/code/viture/spatial-screens/gestures
python3 -m pytest tests/test_classify.py -v
```

Expected: 6 passed.

- [ ] **Step 5: Commit**

```bash
cd /home/salvatore/Desktop/code/viture/spatial-screens
git add gestures/classify.py gestures/tests/test_classify.py
git commit -m "$(cat <<'EOF'
spatial-screens/gestures: add pinch/pose landmark classification

Pure functions over 21-point landmark lists (pinch_norm, pinch_pos,
classify_pose), independently unit-tested against synthetic coordinates
so this doesn't require MediaPipe or hardware to verify.
EOF
)"
```

---

### Task 7: Real MediaPipe inference in the sidecar (M3 exit criteria)

**Files:**
- Modify: `spatial-screens/gestures/hand_tracker.py`
- Create: `spatial-screens/gestures/requirements.txt`

**Interfaces:**
- Consumes: `classify.pinch_norm`, `classify.pinch_pos`, `classify.classify_pose` (Task 6), `protocol.encode_event` (Task 2).

- [ ] **Step 1: Add the requirements file**

Create `spatial-screens/gestures/requirements.txt`:

```
mediapipe
opencv-python-headless
numpy
pytest
```

- [ ] **Step 2: Install dependencies**

```bash
cd /home/salvatore/Desktop/code/viture/spatial-screens/gestures
pip install -r requirements.txt
```

- [ ] **Step 3: Replace `hand_tracker.py` with the real-inference version**

```python
#!/usr/bin/env python3
"""Hand-gesture sidecar for spatial-screens.

Connects to the Unix domain socket spatial-screens listens on, receives raw
camera frames, runs MediaPipe Hands, and sends back one JSON gesture event
per processed frame. See
docs/specs/2026-07-03-hand-gesture-control-design.md for the wire protocol.

Standalone testing: python3 hand_tracker.py --socket /tmp/test.sock --echo
"""
import argparse
import socket
import sys
import time

from classify import classify_pose, pinch_norm, pinch_pos
from protocol import encode_event, read_frame

FORMAT_GRAY8 = 0


def connect(socket_path, retries=20, delay=0.25):
    for _ in range(retries):
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.connect(socket_path)
            return sock
        except (FileNotFoundError, ConnectionRefusedError):
            time.sleep(delay)
    raise RuntimeError(f"could not connect to {socket_path} after {retries} retries")


def make_reader(sock):
    def read_exact(n):
        chunks = []
        remaining = n
        while remaining > 0:
            chunk = sock.recv(remaining)
            if not chunk:
                return None
            chunks.append(chunk)
            remaining -= len(chunk)
        return b"".join(chunks)
    return read_exact


def _no_hand_event(timestamp):
    return encode_event(timestamp, False, "", [(0.0, 0.0)] * 21, 999.0, (0.0, 0.0), "none")


def run_echo(sock, read_exact):
    frame_count = 0
    while True:
        frame = read_frame(read_exact)
        if frame is None:
            break
        timestamp, width, height, fmt, _data = frame
        frame_count += 1
        print(f"echo: frame {frame_count} {width}x{height} fmt={fmt}", file=sys.stderr)
        sock.sendall(_no_hand_event(timestamp))


def _landmarks_to_pairs(hand_landmarks):
    return [(lm.x, lm.y) for lm in hand_landmarks.landmark]


def run_inference(sock, read_exact):
    import cv2
    import mediapipe as mp
    import numpy as np

    hands = mp.solutions.hands.Hands(
        static_image_mode=False, max_num_hands=1,
        min_detection_confidence=0.5, min_tracking_confidence=0.5,
    )

    while True:
        frame = read_frame(read_exact)
        if frame is None:
            break
        timestamp, width, height, fmt, data = frame

        if fmt != FORMAT_GRAY8:
            print(f"hand_tracker: unexpected format {fmt}, skipping frame", file=sys.stderr)
            continue

        gray = np.frombuffer(data, dtype=np.uint8).reshape(height, width)
        rgb = cv2.cvtColor(gray, cv2.COLOR_GRAY2RGB)
        result = hands.process(rgb)

        if result.multi_hand_landmarks:
            lm = _landmarks_to_pairs(result.multi_hand_landmarks[0])
            handedness = result.multi_handedness[0].classification[0].label.lower()
            event = encode_event(
                timestamp, True, handedness, lm,
                pinch_norm(lm), pinch_pos(lm), classify_pose(lm),
            )
        else:
            event = _no_hand_event(timestamp)
        sock.sendall(event)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", required=True)
    parser.add_argument("--echo", action="store_true",
                         help="skip MediaPipe; just acknowledge frames (IPC smoke test)")
    args = parser.parse_args()

    sock = connect(args.socket)
    read_exact = make_reader(sock)

    if args.echo:
        run_echo(sock, read_exact)
    else:
        run_inference(sock, read_exact)


if __name__ == "__main__":
    main()
```

- [ ] **Step 4: Manually verify on hardware**

```bash
cd /home/salvatore/Desktop/code/viture/spatial-screens
make
./run.sh --capture test
```

Hold your hand up in front of the glasses (in the tracking camera's view — roughly where you'd naturally hold it to look at it while wearing the glasses) and try: open palm, a fist, and a pinch. Temporarily add a debug print in `hand_tracker.py`'s `run_inference` loop (`print(f"pose={classify_pose(lm)} pinch={pinch_norm(lm):.2f}", file=sys.stderr)` right after computing `lm`) to watch classification live in the terminal running `run.sh`. Expected: `pose` reads `open_palm`/`fist`/`none` matching what your hand is actually doing, and `pinch` drops noticeably (roughly below 0.5) when thumb and index touch. Remove the debug print once confirmed.

- [ ] **Step 5: Commit**

```bash
git add gestures/hand_tracker.py gestures/requirements.txt
git commit -m "$(cat <<'EOF'
spatial-screens/gestures: wire real MediaPipe Hands inference into the sidecar

Completes M3: live pinch/pose classification verified against a real hand
in front of the glasses' tracking camera.
EOF
)"
```

---

### Task 8: Pinch-drag → distance/size (M4)

**Files:**
- Modify: `spatial-screens/src/main.cpp`

**Interfaces:**
- Consumes: `GestureEvent` from `g_gestures.poll()` (Task 5).

- [ ] **Step 1: Add pinch-tracking state**

Near the other loop-state locals in `main()` (after line 401, `int win_n = 0;`), add:

```cpp
    bool was_pinching = false;
    float pinch_prev_x = 0, pinch_prev_y = 0;
```

- [ ] **Step 2: Handle pinch-drag right after polling the event**

Replace the Task 5 placeholder:

```cpp
        // ---- gestures (state wired up in Task 8/9)
        GestureEvent gev = g_gestures.poll();
```

with:

```cpp
        // ---- gestures
        GestureEvent gev = g_gestures.poll();
        if (gev.pinching) {
            if (was_pinching) {
                float dx = gev.pinch_x - pinch_prev_x; // image space: +x right
                float dy = gev.pinch_y - pinch_prev_y; // image space: +y down
                distance = std::clamp(distance - dy * 4.0f, 0.5f, 10.f);
                diag_in  = std::clamp(diag_in + dx * 200.f, 40.f, 400.f);
                place_screen();
            }
            pinch_prev_x = gev.pinch_x;
            pinch_prev_y = gev.pinch_y;
            was_pinching = true;
        } else {
            was_pinching = false;
        }
```

`std::clamp` needs `<algorithm>`, already included at line 32.

- [ ] **Step 3: Build and manually verify**

```bash
cd /home/salvatore/Desktop/code/viture/spatial-screens
make
./run.sh --capture test
```

Pinch (thumb touching index) and slowly move your hand up/down — the virtual screen should move farther/closer. Move it left/right while still pinching — the screen should shrink/grow. Releasing the pinch and moving your hand should do nothing (confirms the engagement gating works — no drift from non-pinch hand motion).

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "$(cat <<'EOF'
spatial-screens: map pinch-drag to distance/size

Pinch is the engagement gate (per docs/specs/2026-07-03-hand-gesture-control-design.md);
vertical drag while pinched adjusts distance, horizontal adjusts size —
a 2-axis joystick while held, mirroring the existing [ ] / - = hotkeys.
EOF
)"
```

---

### Task 9: Fist-hold → recenter (M4 exit criteria)

**Files:**
- Modify: `spatial-screens/src/main.cpp`

**Interfaces:**
- Consumes: `GestureEvent.pose` (Task 5), `yaw_twist`/`place_screen` (existing, lines 60-64 and 378-385).

- [ ] **Step 1: Add fist-hold state**

Next to the pinch state added in Task 8:

```cpp
    double fist_start_s = -1; // -1 = not currently holding a fist
    bool fist_triggered = false;
```

- [ ] **Step 2: Handle fist-hold right after the pinch-drag block from Task 8**

```cpp
        if (gev.pose == "fist") {
            if (fist_start_s < 0) { fist_start_s = now_s(); fist_triggered = false; }
            else if (!fist_triggered && now_s() - fist_start_s > 0.5) {
                ori_offset = yaw_twist(head_q);
                place_screen();
                printf("gesture recenter (fist-hold)\n");
                fist_triggered = true;
            }
        } else {
            fist_start_s = -1;
            fist_triggered = false;
        }
```

This mirrors the `R` hotkey's non-shift recenter path exactly (line 417-418: `ori_offset = yaw_twist(head_q); place_screen();`), reusing the same state and function rather than duplicating logic differently.

`now_s` is the lambda defined at line 387-390, already in scope inside the loop at this point.

- [ ] **Step 3: Update the startup banner and file header comment**

In the `printf` at line 403-404, change:

```cpp
    printf("running — hotkeys work globally with Ctrl+Alt: R recenter (Shift adds "
           "VIO reset), [ ] distance, - = size, Q quit\n");
```

to:

```cpp
    printf("running — hotkeys work globally with Ctrl+Alt: R recenter (Shift adds "
           "VIO reset), [ ] distance, - = size, Q quit\n"
           "gestures (if sidecar connected): pinch-drag vertical=distance "
           "horizontal=size, fist-hold(0.5s)=recenter\n");
```

Add a line to the file header comment (after line 13, `//        Q/Esc  quit`):

```cpp
//        Gestures (if the sidecar connects): pinch-drag vertical =
//        distance, horizontal = size; fist held ~0.5s = recenter.
```

- [ ] **Step 4: Build and manually verify the full M4 exit criteria**

```bash
cd /home/salvatore/Desktop/code/viture/spatial-screens
make
./run.sh --capture test
```

Verify all three gesture-driven actions work hands-on: pinch-drag resizes/redistances the screen (from Task 8), and holding a fist for about half a second recenters the screen in front of you (printing `gesture recenter (fist-hold)`) without needing the `R` hotkey. Confirm a brief, passing fist shape (well under 0.5s) does *not* trigger a recenter.

- [ ] **Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "$(cat <<'EOF'
spatial-screens: map fist-hold to recenter, completing M4

Full hands-on verification: pinch-drag (distance/size) and fist-hold
(recenter) both control the virtual screen without touching the keyboard.
EOF
)"
```

---

### Task 10: Docs — CLAUDE.md Python note, README gesture usage (M4 wrap-up)

**Files:**
- Modify: `CLAUDE.md`
- Modify: `spatial-screens/README.md`

- [ ] **Step 1: Note the new Python dependency in `CLAUDE.md`**

In the "What this is" section, change:

```
- `sensor-viz/` — web dashboard (vanilla JS ESM + Vite + Three.js; no framework, no TypeScript)
```

to:

```
- `sensor-viz/` — web dashboard (vanilla JS ESM + Vite + Three.js; no framework, no TypeScript)
- `spatial-screens/gestures/` — Python 3 sidecar (MediaPipe Hands) that classifies pinch/pose gestures from the glasses' tracking camera; spawned by `spatial-screens` over a local Unix socket, see `docs/specs/2026-07-03-hand-gesture-control-design.md`
```

Add a line to the "Commands" section, in the `bridge/` list, actually under a new subsection — insert after the `bridge/` bullet block:

```

Gesture sidecar, in `spatial-screens/gestures/`: `pip install -r requirements.txt` once, then `python3 -m pytest tests/ -v` for the pure-logic unit tests (`protocol.py`, `classify.py`). The sidecar itself (`hand_tracker.py`) isn't run standalone — `spatial-screens` spawns it automatically.
```

- [ ] **Step 2: Document gesture controls in `spatial-screens/README.md`**

In the "Run" section, change:

```
Keys: `R` recenter and re-place the screen in front of you (`Shift+R` also
resets the VIO origin), `[` / `]` distance, `-` / `=` size, `Q`/`Esc` quit.
```

to:

```
Keys: `R` recenter and re-place the screen in front of you (`Shift+R` also
resets the VIO origin), `[` / `]` distance, `-` / `=` size, `Q`/`Esc` quit.

Gestures (if `gestures/hand_tracker.py`'s dependencies are installed —
`pip install -r gestures/requirements.txt`): pinch (thumb+index touching)
and drag vertically for distance, horizontally for size; hold a fist for
about half a second to recenter. Gestures are additive — the hotkeys above
always work as a fallback. See
`../docs/specs/2026-07-03-hand-gesture-control-design.md` for how this
works.
```

- [ ] **Step 3: Commit**

```bash
cd /home/salvatore/Desktop/code/viture
git add CLAUDE.md spatial-screens/README.md
git commit -m "$(cat <<'EOF'
docs: note the gestures/ Python sidecar and its controls

CLAUDE.md gains its first non-JS/C++ module; spatial-screens/README.md
documents the pinch-drag/fist-hold gesture controls added in this feature.
EOF
)"
```
