# Hand-overlay Visual Feedback Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add two in-display feedback elements to `spatial-screens` — a grey→blue→green pinch-status dot next to the VO tracking-status dot, and a head-locked overlay of the 21 MediaPipe hand landmarks (thumb/index tips highlighted, green on pinch) — shown whenever a hand is visible.

**Architecture:** The Python sidecar already streams all 21 landmarks in each JSON event; the C++ side just doesn't parse them. Task 1 extracts the event parser into a socket-free, unit-tested translation unit and adds landmark parsing. Task 2 draws the two new quad sets in the existing head-locked overlay in `main.cpp`, reusing the status dot's eye-space projection technique.

**Tech Stack:** C++17, Vulkan (existing push-constant quad pipeline), GNU Make. No new dependencies; no Python or SDK changes.

## Global Constraints

- Linux x86_64 only.
- C++17. `snake_case` functions; `g_` prefix for atomic globals (per CLAUDE.md).
- No changes to `sdk/` (vendored, closed-source) or to `gestures/` (Python sidecar) — the landmark data is already on the wire.
- No new third-party dependency. JSON parsing stays hand-rolled key-scanners for this fixed, both-ends-owned schema (matching the existing `json_find_*` style, see `gesture_client.cpp:28`).
- The full app builds with `make` in `spatial-screens/` and launches only via `./run.sh` (it sets `LD_LIBRARY_PATH`); running the binary directly fails to load SDK libs.
- No C++ test harness/framework exists in this repo. New pure-logic code gets a self-contained assert-and-return-nonzero test binary (the pattern this plan adds); render code is verified manually on hardware.

## Reference: MediaPipe hand landmark indices

21 landmarks, each an `[x, y]` pair normalized to `[0,1]` in image space (x-right, y-down). Pinch is measured between **index 4 (thumb tip)** and **index 8 (index-finger tip)** (`gestures/classify.py:11,14`). Those two are the highlighted dots.

---

### Task 1: Parse hand landmarks (extract testable parser + add landmark scanner)

Extract the JSON event scanners and `parse_event` out of `gesture_client.cpp` into a new socket-free unit `gesture_parse.{h,cpp}`, add a `json_find_landmarks` scanner, and pin it all with a standalone unit-test binary.

**Files:**
- Modify: `spatial-screens/src/gesture_client.h` (extend `GestureEvent`)
- Create: `spatial-screens/src/gesture_parse.h`
- Create: `spatial-screens/src/gesture_parse.cpp`
- Modify: `spatial-screens/src/gesture_client.cpp` (remove moved code, include the new header)
- Create: `spatial-screens/src/gesture_parse_test.cpp`
- Modify: `spatial-screens/Makefile`

**Interfaces:**
- Consumes: `GestureEvent` (defined in `gesture_client.h`).
- Produces:
  - `GestureEvent parse_event(const std::string& line)` — declared in `gesture_parse.h`, defined in `gesture_parse.cpp`. Called by `GestureClient::poll()`.
  - Extended `GestureEvent` with `float landmarks[21][2]` and `bool has_landmarks`, consumed by Task 2's render code.

- [ ] **Step 1: Extend `GestureEvent` in `gesture_client.h`**

In `spatial-screens/src/gesture_client.h`, add two fields to the `GestureEvent` struct (after the `pose` field):

```cpp
struct GestureEvent {
    bool present = false;
    bool pinching = false;   // pinch_norm < PINCH_THRESHOLD
    float pinch_x = 0.f, pinch_y = 0.f; // normalized [0,1] pinch midpoint
    std::string pose;        // "open_palm" | "fist" | "point" | "none" | ""
    float landmarks[21][2] = {}; // normalized [0,1] image coords (x-right, y-down)
    bool has_landmarks = false;  // true iff a full 21-point array parsed
};
```

- [ ] **Step 2: Write the failing test**

Create `spatial-screens/src/gesture_parse_test.cpp`:

```cpp
// Standalone unit test for the hand-gesture event parser. No framework:
// asserts via CHECK, returns non-zero on any failure. Build+run with
//   make gesture-parse-test && ./gesture-parse-test
#include "gesture_parse.h"
#include <cmath>
#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main() {
    // Realistic event line (compact JSON, exactly as the sidecar emits;
    // poll() strips the trailing newline before calling parse_event).
    // 21 landmark pairs; thumb tip = index 4 -> (0.40,0.50),
    // index tip = index 8 -> (0.60,0.70).
    std::string line =
      "{\"type\":\"hand\",\"t\":1.5,\"present\":true,\"handedness\":\"Right\","
      "\"landmarks\":[[0.10,0.20],[0.11,0.21],[0.12,0.22],[0.13,0.23],"
      "[0.40,0.50],[0.15,0.25],[0.16,0.26],[0.17,0.27],[0.60,0.70],"
      "[0.19,0.29],[0.20,0.30],[0.21,0.31],[0.22,0.32],[0.23,0.33],"
      "[0.24,0.34],[0.25,0.35],[0.26,0.36],[0.27,0.37],[0.28,0.38],"
      "[0.29,0.39],[0.30,0.40]],"
      "\"pinch_norm\":0.12,\"pinch_pos\":[0.5,0.6],\"pose\":\"none\"}";

    GestureEvent ev = parse_event(line);
    CHECK(ev.present == true);
    CHECK(ev.has_landmarks == true);
    CHECK(ev.pinching == true);                            // 0.12 < 0.5 threshold
    CHECK(std::fabs(ev.landmarks[4][0] - 0.40f) < 1e-4f);  // thumb tip x
    CHECK(std::fabs(ev.landmarks[4][1] - 0.50f) < 1e-4f);  // thumb tip y
    CHECK(std::fabs(ev.landmarks[8][0] - 0.60f) < 1e-4f);  // index tip x
    CHECK(std::fabs(ev.landmarks[8][1] - 0.70f) < 1e-4f);  // index tip y
    CHECK(std::fabs(ev.landmarks[20][0] - 0.30f) < 1e-4f); // last pair x
    CHECK(std::fabs(ev.landmarks[20][1] - 0.40f) < 1e-4f); // last pair y

    // No-hand event: present=false, 21 zero pairs still parse.
    std::string empty =
      "{\"type\":\"hand\",\"t\":2.0,\"present\":false,\"handedness\":\"\","
      "\"landmarks\":[[0.0,0.0],[0.0,0.0],[0.0,0.0],[0.0,0.0],[0.0,0.0],"
      "[0.0,0.0],[0.0,0.0],[0.0,0.0],[0.0,0.0],[0.0,0.0],[0.0,0.0],"
      "[0.0,0.0],[0.0,0.0],[0.0,0.0],[0.0,0.0],[0.0,0.0],[0.0,0.0],"
      "[0.0,0.0],[0.0,0.0],[0.0,0.0],[0.0,0.0]],"
      "\"pinch_norm\":999.0,\"pinch_pos\":[0.0,0.0],\"pose\":\"none\"}";
    GestureEvent ev2 = parse_event(empty);
    CHECK(ev2.present == false);
    CHECK(ev2.pinching == false);       // not present -> never pinching
    CHECK(ev2.has_landmarks == true);   // 21 pairs present (all zero)

    // Truncated landmarks array -> has_landmarks false; other fields still parse.
    std::string trunc =
      "{\"present\":true,\"landmarks\":[[0.1,0.2],[0.3,0.4]],"
      "\"pinch_norm\":0.9,\"pinch_pos\":[0.1,0.2],\"pose\":\"none\"}";
    GestureEvent ev3 = parse_event(trunc);
    CHECK(ev3.present == true);
    CHECK(ev3.has_landmarks == false);  // only 2 of 21 pairs
    CHECK(ev3.pinching == false);       // 0.9 >= 0.5

    if (failures == 0) { printf("gesture_parse_test: all checks passed\n"); return 0; }
    printf("gesture_parse_test: %d failure(s)\n", failures);
    return 1;
}
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cd spatial-screens && make gesture-parse-test`
Expected: FAIL — the `gesture-parse-test` target and `gesture_parse.h` don't exist yet (make error: no rule to make target / fatal error: gesture_parse.h: No such file or directory).

- [ ] **Step 4: Create `gesture_parse.h`**

Create `spatial-screens/src/gesture_parse.h`:

```cpp
#pragma once

#include <string>
#include "gesture_client.h"  // GestureEvent

// Parse one newline-delimited gesture event (see gestures/protocol.py's
// encode_event) into a GestureEvent. Hand-rolled key-scanners for this
// fixed, both-ends-owned schema — deliberately no JSON library. Extracted
// from gesture_client.cpp so it is unit-testable without the socket layer.
GestureEvent parse_event(const std::string& line);
```

- [ ] **Step 5: Create `gesture_parse.cpp`**

Create `spatial-screens/src/gesture_parse.cpp` (the four existing scanners moved verbatim from `gesture_client.cpp`, plus the new `json_find_landmarks`, plus `parse_event` wired to set the landmark fields):

```cpp
#include "gesture_parse.h"

#include <cstdlib>
#include <cstring>

namespace {

constexpr float PINCH_THRESHOLD = 0.5f; // tune after hands-on test

// Minimal key-based scanners for the fixed, known event schema (see
// protocol.py's encode_event). Not a general JSON parser — deliberately
// so, to avoid adding a JSON library dependency for a schema we control
// on both ends.
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

// landmarks is emitted as "landmarks":[[x,y],[x,y],...] — exactly 21 pairs.
// Walk pair by pair; return true only if all 21 parse. Any short/garbled
// array leaves out[] partially written and returns false (caller gates on
// the return value via has_landmarks).
bool json_find_landmarks(const std::string& s, float out[21][2]) {
    std::string pat = "\"landmarks\":[";
    auto found = s.find(pat);
    if (found == std::string::npos) return false;
    const char* c = s.c_str();
    size_t pos = found + pat.size();  // at the '[' of pair 0
    for (int i = 0; i < 21; i++) {
        while (c[pos] && c[pos] != '[' && c[pos] != ']') pos++;
        if (c[pos] != '[') return false;               // fewer than 21 pairs
        pos++;
        char* end = nullptr;
        out[i][0] = strtof(c + pos, &end);
        if (end == c + pos) return false;              // no x number
        pos = size_t(end - c);
        while (c[pos] && c[pos] != ',') pos++;
        if (c[pos] != ',') return false;
        pos++;
        out[i][1] = strtof(c + pos, &end);
        if (end == c + pos) return false;              // no y number
        pos = size_t(end - c);
        while (c[pos] && c[pos] != ']') pos++;
        if (c[pos] != ']') return false;               // unterminated pair
        pos++;
    }
    return true;
}

} // namespace

GestureEvent parse_event(const std::string& line) {
    GestureEvent ev;
    json_find_bool(line, "present", ev.present);
    float pinch_norm = 999.f;
    json_find_number(line, "pinch_norm", pinch_norm);
    ev.pinching = ev.present && pinch_norm < PINCH_THRESHOLD;
    json_find_pair(line, "pinch_pos", ev.pinch_x, ev.pinch_y);
    json_find_string(line, "pose", ev.pose);
    ev.has_landmarks = json_find_landmarks(line, ev.landmarks);
    return ev;
}
```

- [ ] **Step 6: Add Makefile targets for the parser unit + test**

In `spatial-screens/Makefile`:

(a) Add `src/gesture_parse.o` to the `OBJS` list (line 18-19):

```make
OBJS := src/main.o src/vk_renderer.o src/vk_surface.o src/gesture_client.o \
        src/gesture_parse.o \
        src/capture.o src/capture_xshm.o src/config.o src/capture_portal.o src/telemetry.o
```

(b) Update the `gesture_client.o` prereq line (currently line 25) to include the new header, and add build rules for the parser unit + test (these units don't touch Vulkan/shaders — lighter prereqs, mirroring the existing `gesture_client.o` rule):

```make
# gesture_client / gesture_parse don't touch Vulkan or the shaders — lighter prereqs.
src/gesture_client.o: src/gesture_client.cpp src/gesture_client.h src/gesture_parse.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/gesture_parse.o: src/gesture_parse.cpp src/gesture_parse.h src/gesture_client.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/gesture_parse_test.o: src/gesture_parse_test.cpp src/gesture_parse.h src/gesture_client.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Pure-logic parser test — links only the parser unit, no SDK/Vulkan/socket deps.
gesture-parse-test: src/gesture_parse_test.o src/gesture_parse.o
	$(CXX) $^ -o $@

test: gesture-parse-test
	./gesture-parse-test
```

(c) Add the two new binaries to `clean` (line 63) and `test` to `.PHONY` (line 65):

```make
clean:
	rm -f spatial-screens vk-test capture-test gesture-parse-test src/*.o

.PHONY: run clean test
```

- [ ] **Step 7: Run the test to verify it passes**

Run: `cd spatial-screens && make test`
Expected: compiles, then prints `gesture_parse_test: all checks passed` and exits 0.

- [ ] **Step 8: Rewire `gesture_client.cpp` to use the extracted parser**

In `spatial-screens/src/gesture_client.cpp`:

(a) Add the include near the top (after `#include "gesture_client.h"`):

```cpp
#include "gesture_client.h"
#include "gesture_parse.h"
```

(b) Delete the code now living in `gesture_parse.cpp`: remove `constexpr float PINCH_THRESHOLD = 0.5f;` from the anonymous namespace, and remove the entire `json_find_bool`, `json_find_number`, `json_find_string`, `json_find_pair`, and `parse_event` definitions. **Keep** `GESTURE_INFER_HZ` and `now_s()` — they're used by `maybe_send_frame`. The `poll()` call site `last_event_ = parse_event(recv_buf_.substr(0, nl));` is unchanged (it now resolves to the header-declared `parse_event`).

- [ ] **Step 9: Build the whole app to confirm the rewire links**

Run: `cd spatial-screens && make`
Expected: clean build, `spatial-screens` binary produced, no undefined-reference or redefinition errors (confirms `parse_event` is defined exactly once, in `gesture_parse.o`).

- [ ] **Step 10: Commit**

```bash
cd spatial-screens/..
git add docs/specs/2026-07-04-hand-overlay-design.md \
        docs/specs/2026-07-04-hand-overlay-plan.md \
        spatial-screens/src/gesture_client.h \
        spatial-screens/src/gesture_parse.h \
        spatial-screens/src/gesture_parse.cpp \
        spatial-screens/src/gesture_client.cpp \
        spatial-screens/src/gesture_parse_test.cpp \
        spatial-screens/Makefile
git commit -m "spatial-screens: parse hand landmarks into GestureEvent (extract + unit-test parser)"
```

---

### Task 2: Draw the pinch-status dot and hand-landmark overlay

Add the two overlay elements to the head-locked draw block in `main.cpp`, plus capture the tracking-camera frame dimensions for the panel aspect ratio.

**Files:**
- Modify: `spatial-screens/src/main.cpp` (globals ~199-204; `on_camera_carina` ~209-226; overlay draw block ~823-871, based on master commit `5cf2c06`)

**Interfaces:**
- Consumes: `GestureEvent` with `present`, `pinching`, `has_landmarks`, `landmarks[21][2]` (from Task 1); `g_gestures.enabled()`; the in-scope render locals `proj`, `tan_r`, `tan_t`, `dot_r`, `gev`, `ndraw`, `draws[]`.
- Produces: no new symbols; visual output only.

- [ ] **Step 1: Add tracking-camera dimension globals**

In `spatial-screens/src/main.cpp`, after `static GestureClient g_gestures;` (line 202), add two atomics (the panel aspect needs the camera frame size; `g_` prefix per CLAUDE.md):

```cpp
static GestureClient g_gestures;
static std::atomic<int> g_cam_w{0};  // tracking-camera frame size, for hand-overlay aspect
static std::atomic<int> g_cam_h{0};
```

(`<atomic>` is already included — `g_running` uses it.)

- [ ] **Step 2: Record the frame dimensions in the camera callback**

In `on_camera_carina`, set the dimensions right before forwarding the frame (after the probe block, at the existing `g_gestures.maybe_send_frame(...)` call, line 225):

```cpp
    g_cam_w.store(width, std::memory_order_relaxed);
    g_cam_h.store(height, std::memory_order_relaxed);
    g_gestures.maybe_send_frame(reinterpret_cast<uint8_t*>(image_left0), width, height, timestamp);
```

- [ ] **Step 3: Grow the overlay draw buffer**

In the render section, change the fixed quad buffer (line 823) from 5 to 32 to hold up to 24 quads (screen + VO dot + pinch dot + 21 landmark dots):

```cpp
        // ---- render
        QuadDraw draws[32];
        int ndraw = 0;
```

- [ ] **Step 4: Add the pinch-status dot**

Immediately after the VO tracking-status dot block (right after line 870, `d.circle = true;`, still inside the `if (have_pose && anchored)` block where `proj`, `tan_r`, `tan_t`, `dot_r` are in scope), append. The VO dot sits at `0.95 * tan_r`/`-0.95 * tan_t` (lower-right corner); the pinch dot goes just to its left at the same height (`0.90` vs `0.95` x-factor). The 0.90 spacing is a visual-tuning constant — nudge on hardware if the two dots read too close or too far:

```cpp
            // Pinch-status dot, just left of the VO tracking-status dot (which
            // is at x-factor 0.95). Only shown while the gesture pipeline is
            // live (a grey dot with no sidecar would falsely imply "running,
            // no hand seen").  no hand -> grey, hand seen -> blue, pinch -> green.
            if (g_gestures.enabled()) {
                const float grey[4]  = { 0.5f, 0.5f, 0.5f, 1.f };
                const float hand[4]  = { 0.30f, 0.55f, 1.f, 1.f };
                const float pinch[4] = { 0.20f, 0.90f, 0.30f, 1.f };
                const float* pcol = !gev.present ? grey : (gev.pinching ? pinch : hand);
                float peye[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,
                                   0.90f * tan_r * DOT_Z, -0.95f * tan_t * DOT_Z, -DOT_Z, 1 };
                QuadDraw& pd = draws[ndraw++];
                mat_mul(proj, peye, pd.mvp);
                memcpy(pd.color, pcol, 4 * sizeof(float));
                pd.rect[0] = 0; pd.rect[1] = 0; pd.rect[2] = dot_r; pd.rect[3] = dot_r;
                pd.textured = false;
                pd.circle = true;
            }
```

- [ ] **Step 5: Add the hand-landmark overlay**

Directly after the pinch-status dot block, append the 21-dot panel:

```cpp
            // Hand-landmark overlay: 21 dots in a head-locked lower-left panel,
            // shown only while a hand is actually seen. One shared panel mvp;
            // each landmark is a different rect center (the quad shader builds
            // the quad from rect.xy ± rect.zw), so no per-dot matrix. Thumb tip
            // (4) and index tip (8) — the pair the pinch is measured between —
            // are yellow, turning green while pinching.
            if (g_gestures.enabled() && gev.present && gev.has_landmarks) {
                int cw = g_cam_w.load(std::memory_order_relaxed);
                int ch = g_cam_h.load(std::memory_order_relaxed);
                float aspect = (cw > 0 && ch > 0) ? float(cw) / float(ch) : 4.f / 3.f;
                const float PANEL_H = 0.09f * DOT_Z;        // half-height (~10° tall)
                const float PANEL_W = PANEL_H * aspect;     // aspect-preserved half-width
                float leye[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,
                                   -0.55f * tan_r * DOT_Z, -0.45f * tan_t * DOT_Z, -DOT_Z, 1 };
                float panel_mvp[16];
                mat_mul(proj, leye, panel_mvp);
                const float lm_col[4]  = { 0.40f, 0.90f, 1.00f, 1.f }; // soft cyan
                const float tip[4]     = { 1.00f, 0.85f, 0.10f, 1.f }; // yellow
                const float tip_pin[4] = { 0.20f, 0.90f, 0.30f, 1.f }; // green
                const float lm_r = 0.010f * DOT_Z;
                for (int i = 0; i < 21 && ndraw < 32; i++) {
                    float nx = gev.landmarks[i][0];
                    float ny = gev.landmarks[i][1];
                    // Normalized image coords -> panel-local. Image y is down,
                    // eye-space y is up, so negate y. x is mapped directly; if
                    // the hand reads left-right mirrored on hardware, change
                    // (nx - 0.5f) to (0.5f - nx) here.
                    float lx =  (nx - 0.5f) * 2.f * PANEL_W;
                    float ly = -(ny - 0.5f) * 2.f * PANEL_H;
                    const float* col = (i == 4 || i == 8)
                                         ? (gev.pinching ? tip_pin : tip) : lm_col;
                    QuadDraw& ld = draws[ndraw++];
                    memcpy(ld.mvp, panel_mvp, sizeof(panel_mvp));
                    memcpy(ld.color, col, 4 * sizeof(float));
                    ld.rect[0] = lx; ld.rect[1] = ly; ld.rect[2] = lm_r; ld.rect[3] = lm_r;
                    ld.textured = false;
                    ld.circle = true;
                }
            }
```

- [ ] **Step 6: Build**

Run: `cd spatial-screens && make`
Expected: clean build, `spatial-screens` binary produced, no warnings from the new code (`-Wall -Wextra` is on).

- [ ] **Step 7: Manual verification on hardware**

Gestures need the glasses + the udev rule + the sidecar. Launch via `./run.sh` (never with `viture-bridge` running — single-client SDK). Observe (this is the acceptance checklist):

1. **Sidecar up, no hand in view:** a **grey** dot sits just left of the VO tracking-status dot (lower-right). No landmark overlay.
2. **Hand enters the tracking camera's view:** the status dot turns **blue**, and the **21-dot cloud** appears in the lower-left, roughly tracing your hand. The thumb-tip and index-tip dots are **yellow**.
3. **Pinch (thumb + index together):** the status dot turns **green** and the two highlighted fingertip dots turn **green**; they visibly converge as you pinch.
4. **Release / remove hand:** overlay disappears; status dot returns to blue (hand still seen) then grey (no hand).
5. **Sidecar unavailable** (e.g. run without the Python deps): neither the pinch-status dot nor the overlay is drawn — only the original VO dot. Existing gesture-optional behavior preserved.
6. **Orientation sanity:** moving your hand right/up moves the cloud right/up. If it reads mirrored left-right, apply the one-line x-flip noted in Step 5 and rebuild.

Note in the commit message anything tuned (mirror flip, panel position/size).

- [ ] **Step 8: Commit**

```bash
cd spatial-screens/..
git add spatial-screens/src/main.cpp
git commit -m "spatial-screens: pinch-status dot + hand-landmark overlay in head-locked HUD"
```

---

## Self-Review

**Spec coverage:**
- Pinch-status dot next to VO dot, grey→blue→green, hidden when sidecar down → Task 2 Steps 4, 7.
- Hand-landmark overlay, dots only, shown only when hand present → Task 2 Step 5.
- Thumb (4) + index (8) tips yellow, green on pinch → Task 2 Step 5.
- Landmark parsing C++-side, no Python/SDK change → Task 1 (all steps).
- Panel aspect from camera frame → Task 2 Steps 1, 2, 5.
- `draws[5]`→`draws[32]` → Task 2 Step 3.
- Testing: parser unit test + manual hardware checklist → Task 1 Steps 2/7, Task 2 Step 7.
- Edge cases (sidecar disabled, no hand, short array, buffer bound) → Task 1 test (empty/trunc cases) + Task 2 gating (`enabled()`, `present`, `has_landmarks`, `ndraw < 32`).
- Out-of-scope items (skeleton lines, world-anchoring, numeric readout) → not implemented, matching spec.

**Placeholder scan:** No TBD/TODO/"handle edge cases" — every code step carries full code and exact commands.

**Type consistency:** `parse_event(const std::string&) -> GestureEvent` used identically in Task 1 (declaration, definition, test, `poll()` call site). `GestureEvent.landmarks[21][2]` / `has_landmarks` defined in Task 1 Step 1 and consumed with the same names/types in Task 2 Step 5. `g_cam_w`/`g_cam_h` (`std::atomic<int>`) defined and used consistently. `DOT_Z`, `dot_r`, `tan_r`, `tan_t`, `proj`, `mat_mul`, `ndraw`, `draws` all reference existing in-scope `main.cpp` names verified against lines 822-869.
