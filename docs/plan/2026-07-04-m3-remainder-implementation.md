# spatial-screens M3 Remainder Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finish phase-2 milestone M3 in `spatial-screens/`: PipeWire/xdg-desktop-portal capture (portal-preferred dual backend), a config file + persisted runtime state, and WebSocket telemetry that turns the phase-1 `sensor-viz` dashboard into the monitoring console.

**Architecture:** Capture becomes a `CaptureBackend` interface with three implementations (portal, XShm, test pattern) selected by an `auto` fallback chain; portal uses libdbus-1 for the ScreenCast session and a PipeWire thread-loop stream for frames. Options resolve as defaults < config file < state file < CLI. Telemetry embeds `bridge/ws_server.hpp` and speaks the bridge protocol on port 8765 plus one new `app` message consumed by a new dashboard panel.

**Tech Stack:** C++17, libdbus-1, libpipewire-0.3, Vulkan (existing renderer untouched), vanilla JS ESM (sensor-viz).

**Spec:** [`docs/specs/2026-07-04-m3-remainder-design.md`](../specs/2026-07-04-m3-remainder-design.md) — read it first.

## Global Constraints

- Linux x86_64 only (repo-wide). Dev machine: Pop!_OS 22.04, X11 GNOME session, portal ScreenCast version 4.
- C++17, `spatial-screens/src/` style: free functions, file-scope statics only where a C callback needs them, `snake_case`, 4-space indent in C++.
- **No C++ test framework in this repo** (CLAUDE.md convention). C++ tasks use manual build-and-run verification: the `capture-test` harness for capture, `--dump-config` for options, and a final glasses-on handoff for the full app. No TDD red/green steps for C++.
- sensor-viz JS: 2-space indent, single quotes, semicolons; verify with `npm run lint` and `npm test` in `sensor-viz/`. No new unit tests (the additions are DOM/IO glue; Vitest covers `math.js` only).
- Never modify anything under `sdk/` (vendored, closed-source).
- The SDK allows exactly ONE process on the device. Stop `viture-bridge` before any hardware run of `spatial-screens`. Never double-start `spatial-screens` (global hotkey grabs + SDK).
- `spatial-screens` must be launched via `./run.sh` on hardware (sets `LD_LIBRARY_PATH`). `capture-test` and `--dump-config` need neither the SDK nor `run.sh`.
- Only Tasks requiring hardware say so explicitly; everything else must be verified headlessly on this machine.
- New runtime behavior must degrade, not die: capture failures fall back (portal → XShm → test under `auto`; explicit backend → test), telemetry bind failure warns and disables, config parse problems warn per line. Only Vulkan/SDK/X-display failures may exit (through the existing teardown paths).
- Wire formats copied verbatim from the spec:
  - `app` message: `{"type":"app","fps":118.9,"sixdof":true,"anchored":true,"distance":0.75,"size":24,"backend":"portal","direct":true}`
  - config keys: `monitor`, `capture`, `capture-backend`, `distance`, `size`, `pitch-trim`, `predict-ms`, `smooth-pos`, `smooth-ori`, `window`, `ws-port`
  - state keys: `distance`, `size`, `restore-token`
  - paths: config `$XDG_CONFIG_HOME/spatial-screens.conf` (fallback `~/.config`), state `$XDG_STATE_HOME/spatial-screens/state` (fallback `~/.local/state`)
- Commit message style: `spatial-screens: <what>` (see `git log`), one commit per task unless a step says otherwise.

## File Structure

```
spatial-screens/src/
  capture.h            NEW  CaptureFrame, CaptureBackend, factory declarations
  capture.cpp          NEW  test-pattern backend + shared helpers
  capture_xshm.cpp     NEW  XShm backend (logic moved from main.cpp)
  capture_portal.h     NEW  PortalSession struct + portal_open_screencast/close (internal)
  capture_portal.cpp   NEW  portal D-Bus dance (Task 4) + PipeWire backend (Task 5)
  capture_test.cpp     NEW  standalone harness (no SDK link), like vk_test.cpp
  config.h/.cpp        NEW  Options/AppState, file parsing, XDG paths, atomic save
  telemetry.h/.cpp     NEW  wsrv::Server wrapper, bridge-protocol messages, throttles
  main.cpp             MOD  consumes all of the above; inline capture code deleted
spatial-screens/Makefile   MOD  new objects, pkg-config dbus-1 + libpipewire-0.3, capture-test target
bridge/ws_server.hpp       UNCHANGED — included via -I../bridge
sensor-viz/src/drivers/bridge-ws.js  MOD  forward 'app' messages
sensor-viz/src/ui/panels.js          MOD  showAppPanel/hideAppPanel
sensor-viz/src/main.js               MOD  wire 'app' event + teardown
sensor-viz/index.html                MOD  app-card section
docs + README + CLAUDE.md            MOD  Task 8
```

---

### Task 1: Capture interface, XShm + test backends, capture-test harness

**Files:**
- Create: `spatial-screens/src/capture.h`
- Create: `spatial-screens/src/capture.cpp`
- Create: `spatial-screens/src/capture_xshm.cpp`
- Create: `spatial-screens/src/capture_test.cpp`
- Modify: `spatial-screens/Makefile`

**Interfaces:**
- Consumes: `OutputRect` and `list_outputs(Display*)` from `src/vk_surface.h` (`struct OutputRect { std::string name; RROutput id; int x, y, w, h; }`).
- Produces (later tasks depend on these exact signatures):
  - `struct CaptureFrame { const uint8_t* data; int w, h; uint32_t pitch; }`
  - `class CaptureBackend { bool start(); bool latest_frame(CaptureFrame&); bool alive() const; const char* name() const; void stop(); void on_outputs_changed(const std::vector<OutputRect>&); }`
  - `std::unique_ptr<CaptureBackend> capture_create_xshm(Display*, const OutputRect&)`
  - `std::unique_ptr<CaptureBackend> capture_create_test()`

- [ ] **Step 1: Write `src/capture.h`**

```cpp
// Capture backends for spatial-screens: where the virtual screen's pixels
// come from. Selected by --capture-backend auto|portal|xshm|test with an
// auto fallback chain (portal -> xshm -> test); see
// docs/specs/2026-07-04-m3-remainder-design.md.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include <X11/Xlib.h>
#include "vk_surface.h"  // OutputRect

struct CaptureFrame {
    const uint8_t* data = nullptr;  // 32bpp BGRX rows (matches VK_FORMAT_B8G8R8A8_UNORM)
    int w = 0, h = 0;
    uint32_t pitch = 0;             // bytes per row
};

class CaptureBackend {
public:
    virtual ~CaptureBackend() = default;
    virtual bool start() = 0;
    // Newest complete frame; false if none is available yet. The pointer
    // stays valid until the next latest_frame() or stop() on this backend.
    virtual bool latest_frame(CaptureFrame& out) = 0;
    // false = permanent failure; the caller switches to the next backend.
    virtual bool alive() const = 0;
    virtual const char* name() const = 0;
    virtual void stop() = 0;
    // X11 layout changed (RandR event). Backends that grab by screen rect
    // re-resolve their source; others ignore it.
    virtual void on_outputs_changed(const std::vector<OutputRect>&) {}
};

std::unique_ptr<CaptureBackend> capture_create_xshm(Display* dpy, const OutputRect& source);
std::unique_ptr<CaptureBackend> capture_create_test();
```

- [ ] **Step 2: Write `src/capture.cpp` (test-pattern backend)**

The animation matches the pattern main.cpp draws today (scrolling two-tone checkerboard, `0xff2d3646`/`0xff59c2ff`, 64 px cells, 40 px/s) so glasses-on behavior is unchanged.

```cpp
#include "capture.h"

#include <chrono>
#include <cstring>

namespace {

class TestBackend : public CaptureBackend {
public:
    bool start() override {
        buf_.resize(size_t(W) * H);
        t0_ = now_s();
        return true;
    }
    bool latest_frame(CaptureFrame& out) override {
        int shift_px = int((now_s() - t0_) * 40) % 64;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                buf_[size_t(y) * W + x] =
                    (((x + shift_px) / 64 + y / 64) & 1) ? 0xff2d3646 : 0xff59c2ff;
        out.data = reinterpret_cast<const uint8_t*>(buf_.data());
        out.w = W; out.h = H; out.pitch = W * 4;
        return true;
    }
    bool alive() const override { return true; }  // the pattern cannot fail
    const char* name() const override { return "test"; }
    void stop() override {}

private:
    static double now_s() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }
    static constexpr int W = 1024, H = 576;
    std::vector<uint32_t> buf_;
    double t0_ = 0;
};

}  // namespace

std::unique_ptr<CaptureBackend> capture_create_test() {
    return std::make_unique<TestBackend>();
}
```

- [ ] **Step 3: Write `src/capture_xshm.cpp`**

This is the XShm logic from `main.cpp` (setup lines around `XShmCreateImage`, the RRScreenChangeNotify rebuild, the `XShmGetImage` grab, and teardown), relocated behind the interface. Behavior parity, one deliberate change: a rebuild failure no longer exits the app — the backend reports `alive() == false` and the caller falls back (spec: capture problems never exit mid-session).

```cpp
#include "capture.h"

#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <cstdio>
#include <cstring>

namespace {

class XShmBackend : public CaptureBackend {
public:
    XShmBackend(Display* dpy, const OutputRect& source) : dpy_(dpy), src_(source) {}
    ~XShmBackend() override { stop(); }

    bool start() override {
        if (!XShmQueryExtension(dpy_)) {
            fprintf(stderr, "capture(xshm): MIT-SHM not available\n");
            return false;
        }
        return build();
    }

    bool latest_frame(CaptureFrame& out) override {
        if (!ximg_) return false;
        if (!XShmGetImage(dpy_, DefaultRootWindow(dpy_), ximg_, src_.x, src_.y, AllPlanes))
            return false;
        out.data = reinterpret_cast<const uint8_t*>(ximg_->data);
        out.w = src_.w; out.h = src_.h;
        out.pitch = uint32_t(ximg_->bytes_per_line);
        return true;
    }

    bool alive() const override { return alive_; }
    const char* name() const override { return "xshm"; }

    void stop() override {
        if (ximg_) {
            XShmDetach(dpy_, &shm_);
            XDestroyImage(ximg_);
            shmdt(shm_.shmaddr);
            ximg_ = nullptr;
        }
    }

    void on_outputs_changed(const std::vector<OutputRect>& outs) override {
        for (const auto& o : outs) {
            if (o.name != src_.name) continue;
            if (o.w != src_.w || o.h != src_.h) {
                stop();
                src_ = o;
                if (!build()) {
                    fprintf(stderr, "capture(xshm): rebuild failed after source resize\n");
                    alive_ = false;
                }
            } else {
                src_ = o;  // moved only: new grab origin
            }
            return;
        }
        fprintf(stderr, "capture(xshm): source %s disappeared\n", src_.name.c_str());
        alive_ = false;
    }

private:
    bool build() {
        ximg_ = XShmCreateImage(dpy_, DefaultVisual(dpy_, DefaultScreen(dpy_)), 24,
                                ZPixmap, nullptr, &shm_, src_.w, src_.h);
        if (!ximg_) return false;
        shm_.shmid = shmget(IPC_PRIVATE, size_t(ximg_->bytes_per_line) * ximg_->height,
                            IPC_CREAT | 0600);
        shm_.shmaddr = ximg_->data = (char*)shmat(shm_.shmid, nullptr, 0);
        shm_.readOnly = False;
        XShmAttach(dpy_, &shm_);
        shmctl(shm_.shmid, IPC_RMID, nullptr);  // auto-free on detach
        return true;
    }

    Display* dpy_;
    OutputRect src_;
    XShmSegmentInfo shm_{};
    XImage* ximg_ = nullptr;
    bool alive_ = true;
};

}  // namespace

std::unique_ptr<CaptureBackend> capture_create_xshm(Display* dpy, const OutputRect& source) {
    return std::make_unique<XShmBackend>(dpy, source);
}
```

- [ ] **Step 4: Write `src/capture_test.cpp` (harness)**

```cpp
// capture-test — exercises a capture backend standalone: no SDK link, no
// glasses, no run.sh. Writes grabbed frames as PPMs to /tmp and prints
// per-frame timing. (Counterpart of vk_test.cpp for the capture stack.)
//
//   ./capture-test [--backend xshm|test] [--capture NAME] [--frames N]
#include "capture.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

static double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static void write_ppm(const char* path, const CaptureFrame& f) {
    FILE* fp = fopen(path, "wb");
    if (!fp) { fprintf(stderr, "cannot write %s\n", path); return; }
    fprintf(fp, "P6\n%d %d\n255\n", f.w, f.h);
    for (int y = 0; y < f.h; y++) {
        const uint8_t* row = f.data + size_t(y) * f.pitch;
        for (int x = 0; x < f.w; x++) {
            uint8_t rgb[3] = { row[x * 4 + 2], row[x * 4 + 1], row[x * 4 + 0] };  // BGRX -> RGB
            fwrite(rgb, 1, 3, fp);
        }
    }
    fclose(fp);
}

int main(int argc, char** argv) {
    std::string backend = "xshm", capture_name;
    int frames = 10;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--backend") && i + 1 < argc) backend = argv[++i];
        else if (!strcmp(argv[i], "--capture") && i + 1 < argc) capture_name = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else {
            printf("usage: %s [--backend xshm|test] [--capture NAME] [--frames N]\n", argv[0]);
            return 0;
        }
    }

    std::unique_ptr<CaptureBackend> cap;
    Display* dpy = nullptr;
    if (backend == "test") {
        cap = capture_create_test();
    } else if (backend == "xshm") {
        dpy = XOpenDisplay(nullptr);
        if (!dpy) { fprintf(stderr, "cannot open X display\n"); return 1; }
        auto outs = list_outputs(dpy);
        OutputRect src{};
        bool found = false;
        for (auto& o : outs) {
            printf("output: %-10s %dx%d+%d+%d\n", o.name.c_str(), o.w, o.h, o.x, o.y);
            if (!found && (capture_name.empty() || o.name == capture_name)) { src = o; found = true; }
        }
        if (!found) { fprintf(stderr, "no matching output\n"); return 1; }
        printf("capturing %s\n", src.name.c_str());
        cap = capture_create_xshm(dpy, src);
    } else {
        fprintf(stderr, "unknown backend %s\n", backend.c_str());
        return 1;
    }

    if (!cap->start()) { fprintf(stderr, "backend start failed\n"); return 1; }

    double last = now_s();
    for (int i = 0; i < frames && cap->alive(); ) {
        CaptureFrame f{};
        if (cap->latest_frame(f)) {
            double t = now_s();
            char path[128];
            snprintf(path, sizeof(path), "/tmp/capture-test-%03d.ppm", i);
            write_ppm(path, f);
            printf("frame %3d  %dx%d pitch=%u  dt=%.1fms  -> %s\n",
                   i, f.w, f.h, f.pitch, (t - last) * 1000.0, path);
            last = t;
            i++;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    cap->stop();
    if (dpy) XCloseDisplay(dpy);
    printf("done (backend=%s alive=%d)\n", cap->name(), cap->alive() ? 1 : 0);
    return 0;
}
```

- [ ] **Step 5: Makefile — new objects + capture-test target**

In `spatial-screens/Makefile`, change the `OBJS` line and add rules (portal object comes in Task 4; do not add it yet):

```make
OBJS := src/main.o src/vk_renderer.o src/vk_surface.o src/gesture_client.o \
        src/capture.o src/capture_xshm.o
```

Add after the `vk-test` rule:

```make
CAPTURE_OBJS := src/capture.o src/capture_xshm.o

# Capture objects don't touch Vulkan or the shaders — lighter prereqs.
src/capture.o src/capture_xshm.o src/capture_test.o: src/capture.h src/vk_surface.h

capture-test: src/capture_test.o $(CAPTURE_OBJS) src/vk_surface.o
	$(CXX) $^ -o $@ $(VK_LIBS)
```

And extend `clean`:

```make
clean:
	rm -f spatial-screens vk-test capture-test src/*.o
```

- [ ] **Step 6: Build and run the harness**

Run: `cd /home/salvatore/Desktop/code/viture/spatial-screens && make capture-test && ./capture-test --backend test --frames 3 && ./capture-test --backend xshm --frames 5`
Expected: builds clean (`-Wall -Wextra`, no warnings in new files); test backend writes 3 checkerboard PPMs; xshm backend lists outputs, writes 5 PPMs at ~33 ms spacing. Open one: `xdg-open /tmp/capture-test-004.ppm` — it must show real desktop content.

- [ ] **Step 7: Commit**

```bash
git add spatial-screens/src/capture.h spatial-screens/src/capture.cpp \
        spatial-screens/src/capture_xshm.cpp spatial-screens/src/capture_test.cpp \
        spatial-screens/Makefile
git commit -m "spatial-screens: capture backend interface, XShm/test backends, capture-test harness"
```

---

### Task 2: main.cpp consumes CaptureBackend

**Files:**
- Modify: `spatial-screens/src/main.cpp`
- Modify: `spatial-screens/Makefile` (main.o prereq)

**Interfaces:**
- Consumes: everything Task 1 produces.
- Produces: `--capture-backend auto|portal|xshm|test` CLI flag; a `switch_backend()` lambda in `main()` implementing the fallback chain — Task 5 inserts `"portal"` at the front of `chain` and Task 6 adds telemetry logs inside it. The capture tick rebuilds the texture on any dims/pitch mismatch (`vk.tex_w/tex_h/tex_pitch` vs frame).

- [ ] **Step 1: Remove inline capture code from main.cpp**

Delete, from `src/main.cpp`:
- includes `<X11/extensions/XShm.h>`, `<sys/ipc.h>`, `<sys/shm.h>` (Xrandr/Xlib stay);
- the capture-source pick block (the `bool test_pattern = capture_name == "test";` block after the glasses pick, ~lines 295–308);
- the direct-mode source re-resolve block (`// Direct mode reflowed the desktop...`, ~lines 334–344);
- the XShm setup block (`// -- capture setup (XShm full-source-monitor grabs)`, ~lines 367–383);
- the XShm rebuild branch inside the RRScreenChangeNotify handler (~lines 471–508) — keep `XRRUpdateConfiguration(&ev);` and `continue;`;
- the 30 Hz capture block's body (`if (test_pattern) {...} else if (XShmGetImage(...))`, ~lines 646–655);
- the XShm teardown line (`if (ximg) { XShmDetach...`, ~line 723);
- the `std::vector<uint32_t> pattern(...)` line and the `test_pattern`/`cap_aspect`-from-`test_pattern` logic.

- [ ] **Step 2: Add the new capture wiring**

Include the header (with the existing local includes):

```cpp
#include "capture.h"
```

CLI: add to the option parsing and usage string:

```cpp
std::string capture_backend = "auto";
// in the loop:
else if (!strcmp(argv[i], "--capture-backend") && i + 1 < argc) capture_backend = argv[++i];
// usage string gains: [--capture-backend auto|portal|xshm|test]
```

After parsing, keep `--capture test` working as an alias:

```cpp
if (capture_name == "test") { capture_backend = "test"; capture_name.clear(); }
```

Where the XShm setup block was (after the hotkey grabs — this is deliberately after `direct_acquire`, so the XShm source rect resolves against the already-reflowed desktop):

```cpp
    // -- capture backend chain: auto = xshm -> test; explicit backend -> test
    std::vector<std::string> chain;
    if (capture_backend == "auto") chain = { "xshm", "test" };
    else if (capture_backend != "test") chain = { capture_backend, "test" };
    else chain = { "test" };
    size_t chain_pos = 0;
    std::unique_ptr<CaptureBackend> cap;
    float cap_aspect = 16.f / 9.f;
    auto switch_backend = [&]() {
        while (chain_pos < chain.size()) {
            const std::string& kind = chain[chain_pos++];
            std::unique_ptr<CaptureBackend> b;
            if (kind == "xshm") {
                auto outs = list_outputs(dpy);
                OutputRect src{};
                bool found = false;
                for (auto& o : outs)
                    if (!capture_name.empty() ? o.name == capture_name
                                              : o.name != glasses.name) { src = o; found = true; break; }
                if (found) b = capture_create_xshm(dpy, src);
                else fprintf(stderr, "capture: no xshm source monitor\n");
            } else if (kind == "test") {
                b = capture_create_test();
            } else {
                fprintf(stderr, "capture: unknown backend %s\n", kind.c_str());
            }
            if (b && b->start()) {
                cap = std::move(b);
                printf("capture: %s\n", cap->name());
                return;
            }
            if (b) fprintf(stderr, "capture: %s failed to start — falling back\n", kind.c_str());
        }
    };
    switch_backend();
```

Texture init (replaces the old `tex_w/tex_h/tex_pitch` + `vkr_init_texture` block): wait briefly for a first frame so the texture matches real dims (XShm/test deliver immediately; portal takes a beat once Task 5 lands), else start with placeholder dims and let the mismatch-rebuild fix it:

```cpp
    // -- capture texture: prefer real dims from a first frame
    CaptureFrame first{};
    for (int i = 0; i < 200 && cap && !cap->latest_frame(first); i++)
        usleep(10 * 1000);
    uint32_t tex_w = first.data ? uint32_t(first.w) : 1024;
    uint32_t tex_h = first.data ? uint32_t(first.h) : 576;
    uint32_t tex_pitch = first.data ? first.pitch : 1024 * 4;
    if (!vkr_init_texture(vk, tex_w, tex_h, tex_pitch)) {
        if (cap) cap->stop();
        vkr_destroy_device(vk);
        if (sout.direct) direct_release(vk.instance);
        vkr_destroy(vk);
        if (sout.direct) direct_restore(dpy);
        return 1;
    }
    if (first.data) {
        vkr_upload(vk, first.data, size_t(first.pitch) * first.h);
        cap_aspect = float(first.w) / float(first.h);
    }
```

RRScreenChangeNotify handler becomes:

```cpp
            if (ev.type == rr_event_base + RRScreenChangeNotify) {
                XRRUpdateConfiguration(&ev);
                if (cap) cap->on_outputs_changed(list_outputs(dpy));
                continue;
            }
```

The 30 Hz capture tick becomes (texture rebuild failure is a Vulkan failure, not a capture failure — it still exits through teardown, matching the existing invalid-draw-state fix):

```cpp
        // ---- capture (30 Hz is plenty; pose stays at panel rate)
        double tnow = now_s();
        if (tnow - last_cap_t > 1.0 / 30.0) {
            last_cap_t = tnow;
            if (cap && !cap->alive()) {
                fprintf(stderr, "capture: %s died — switching\n", cap->name());
                cap->stop();
                switch_backend();
            }
            CaptureFrame f{};
            if (cap && cap->latest_frame(f)) {
                if (uint32_t(f.w) != vk.tex_w || uint32_t(f.h) != vk.tex_h ||
                    f.pitch != vk.tex_pitch) {
                    vkr_destroy_texture(vk);
                    if (!vkr_init_texture(vk, f.w, f.h, f.pitch)) {
                        fprintf(stderr, "capture texture rebuild failed\n");
                        g_running = false;
                    } else {
                        cap_aspect = float(f.w) / float(f.h);
                    }
                }
                if (g_running) vkr_upload(vk, f.data, size_t(f.pitch) * f.h);
            }
        }
```

Teardown (where the XShm teardown line was):

```cpp
    if (cap) cap->stop();
```

Also update the file's header comment (usage line + "X11 capture of a source monitor (XShm) or a test pattern" wording) to mention `--capture-backend`.

- [ ] **Step 3: Makefile prereq**

`src/main.o` now also depends on `capture.h`:

```make
src/main.o: src/gesture_client.h src/capture.h
```

- [ ] **Step 4: Build both binaries**

Run: `cd /home/salvatore/Desktop/code/viture/spatial-screens && make clean && make && make vk-test capture-test`
Expected: all build with no new warnings. (`./spatial-screens` itself needs glasses — full behavior parity is verified in Task 8's hardware pass. A quick `./spatial-screens` without glasses must still print `No supported VITURE glasses found.` and exit cleanly.)

- [ ] **Step 5: Commit**

```bash
git add spatial-screens/src/main.cpp spatial-screens/Makefile
git commit -m "spatial-screens: main.cpp consumes CaptureBackend (chain: xshm -> test)"
```

---

### Task 3: Config file + state file

**Files:**
- Create: `spatial-screens/src/config.h`
- Create: `spatial-screens/src/config.cpp`
- Modify: `spatial-screens/src/main.cpp` (option parsing, exit-time state save)
- Modify: `spatial-screens/Makefile`

**Interfaces:**
- Produces (Tasks 5/6 depend on these):
  - `struct Options { std::string monitor, capture, capture_backend; float distance, size, pitch_trim, predict_ms, smooth_pos, smooth_ori; bool window; int ws_port; }` (defaults: `"auto"`, 0.75, 24, 0, 0, 0.10, 0.40, false, 8765)
  - `struct AppState { float distance = -1.f; float size = -1.f; std::string restore_token; }` (negative float = unset)
  - `std::string config_default_path();` / `std::string state_file_path();`
  - `bool set_option(Options&, const std::string& key, const std::string& value);`
  - `void load_options_file(const std::string& path, Options&, bool warn_missing);`
  - `void load_state(AppState&);` / `bool save_state(const AppState&);`
- Consumes: nothing from other tasks.

- [ ] **Step 1: Write `src/config.h`**

```cpp
// Options resolution for spatial-screens: compiled defaults < config file
// (~/.config/spatial-screens.conf, user-authored, read-only) < state file
// (~/.local/state/spatial-screens/state, app-written) < CLI flags.
// See docs/specs/2026-07-04-m3-remainder-design.md §2.
#pragma once
#include <string>

struct Options {
    std::string monitor;                  // glasses output ("" = autodetect)
    std::string capture;                  // capture monitor ("" = first non-glasses)
    std::string capture_backend = "auto"; // auto|portal|xshm|test
    float distance = 0.75f;               // meters
    float size = 24.f;                    // diagonal inches
    float pitch_trim = 0.f;               // degrees
    float predict_ms = 0.f;
    float smooth_pos = 0.10f;
    float smooth_ori = 0.40f;
    bool window = false;
    int ws_port = 8765;                   // 0 = telemetry disabled
};

// Live-tuned values + portal restore token. Whole file rewritten atomically
// on clean exit and whenever a new restore token arrives.
struct AppState {
    float distance = -1.f;                // < 0 = unset
    float size = -1.f;
    std::string restore_token;
};

std::string config_default_path();  // $XDG_CONFIG_HOME|~/.config + /spatial-screens.conf
std::string state_file_path();      // $XDG_STATE_HOME|~/.local/state + /spatial-screens/state

// Applies one "key value" pair using the config-key spelling (e.g.
// "capture-backend"). Returns false for unknown keys; warns to stderr (and
// still returns true) for unparsable numeric values.
bool set_option(Options& o, const std::string& key, const std::string& value);

// INI-style "key = value", '#' comments, no sections. Missing file is fine
// (warn only if warn_missing); bad lines warn and are skipped.
void load_options_file(const std::string& path, Options& o, bool warn_missing);

void load_state(AppState& s);
bool save_state(const AppState& s);
```

- [ ] **Step 2: Write `src/config.cpp`**

```cpp
#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace {

std::string xdg_dir(const char* env, const char* home_suffix) {
    const char* v = getenv(env);
    if (v && *v) return v;
    const char* home = getenv("HOME");
    return std::string(home ? home : ".") + home_suffix;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool parse_float(const std::string& key, const std::string& v, float& out) {
    char* end = nullptr;
    float f = strtof(v.c_str(), &end);
    if (end == v.c_str() || *end != '\0') {
        fprintf(stderr, "config: %s: not a number: '%s' (ignored)\n", key.c_str(), v.c_str());
        return false;
    }
    out = f;
    return true;
}

bool parse_bool(const std::string& v) { return v == "true" || v == "1" || v == "yes"; }

// Shared line-format parser for config and state files.
void parse_kv_file(const std::string& path, bool warn_missing,
                   bool (*apply)(void*, const std::string&, const std::string&), void* ctx) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        if (warn_missing) fprintf(stderr, "config: cannot open %s\n", path.c_str());
        return;
    }
    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) {
            fprintf(stderr, "config: %s:%d: expected 'key = value' (skipped)\n",
                    path.c_str(), lineno);
            continue;
        }
        std::string key = trim(s.substr(0, eq)), val = trim(s.substr(eq + 1));
        if (!apply(ctx, key, val))
            fprintf(stderr, "config: %s:%d: unknown key '%s' (skipped)\n",
                    path.c_str(), lineno, key.c_str());
    }
    fclose(f);
}

}  // namespace

std::string config_default_path() {
    return xdg_dir("XDG_CONFIG_HOME", "/.config") + "/spatial-screens.conf";
}

std::string state_file_path() {
    return xdg_dir("XDG_STATE_HOME", "/.local/state") + "/spatial-screens/state";
}

bool set_option(Options& o, const std::string& k, const std::string& v) {
    if (k == "monitor") o.monitor = v;
    else if (k == "capture") o.capture = v;
    else if (k == "capture-backend") o.capture_backend = v;
    else if (k == "distance") parse_float(k, v, o.distance);
    else if (k == "size") parse_float(k, v, o.size);
    else if (k == "pitch-trim") parse_float(k, v, o.pitch_trim);
    else if (k == "predict-ms") parse_float(k, v, o.predict_ms);
    else if (k == "smooth-pos") parse_float(k, v, o.smooth_pos);
    else if (k == "smooth-ori") parse_float(k, v, o.smooth_ori);
    else if (k == "window") o.window = parse_bool(v);
    else if (k == "ws-port") { float f; if (parse_float(k, v, f)) o.ws_port = int(f); }
    else return false;
    return true;
}

void load_options_file(const std::string& path, Options& o, bool warn_missing) {
    parse_kv_file(path, warn_missing,
                  [](void* ctx, const std::string& k, const std::string& v) {
                      return set_option(*static_cast<Options*>(ctx), k, v);
                  }, &o);
}

void load_state(AppState& s) {
    parse_kv_file(state_file_path(), false,
                  [](void* ctx, const std::string& k, const std::string& v) {
                      AppState& st = *static_cast<AppState*>(ctx);
                      if (k == "distance") parse_float(k, v, st.distance);
                      else if (k == "size") parse_float(k, v, st.size);
                      else if (k == "restore-token") st.restore_token = v;
                      else return false;
                      return true;
                  }, &s);
}

bool save_state(const AppState& s) {
    std::string path = state_file_path();
    std::string dir = path.substr(0, path.find_last_of('/'));
    // mkdir -p for the two possible missing levels (~/.local/state, then ours)
    std::string parent = dir.substr(0, dir.find_last_of('/'));
    mkdir(parent.c_str(), 0755);
    mkdir(dir.c_str(), 0755);
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) {
        fprintf(stderr, "state: cannot write %s\n", tmp.c_str());
        return false;
    }
    fprintf(f, "# written by spatial-screens — do not edit while it runs\n");
    if (s.distance > 0) fprintf(f, "distance = %.3f\n", s.distance);
    if (s.size > 0) fprintf(f, "size = %.1f\n", s.size);
    if (!s.restore_token.empty()) fprintf(f, "restore-token = %s\n", s.restore_token.c_str());
    fclose(f);
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        fprintf(stderr, "state: rename to %s failed\n", path.c_str());
        return false;
    }
    return true;
}
```

- [ ] **Step 3: Rewire main.cpp option parsing**

Replace the current flag-parsing block (the `std::string monitor_name, capture_name;` declarations through the end of the `for` loop, keeping the explanatory comments about defaults above it) with:

```cpp
    Options o;
    AppState app_state;
    std::string config_path = config_default_path();
    bool config_explicit = false, dump_config = false;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--config") && i + 1 < argc) { config_path = argv[++i]; config_explicit = true; }
    load_options_file(config_path, o, config_explicit);
    load_state(app_state);
    if (app_state.distance > 0) o.distance = app_state.distance;
    if (app_state.size > 0) o.size = app_state.size;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        bool ok = false;
        if (!strncmp(a, "--", 2)) {
            a += 2;
            if (!strcmp(a, "config")) { i++; ok = true; }  // handled in the pre-pass
            else if (!strcmp(a, "window")) { o.window = true; ok = true; }
            else if (!strcmp(a, "dump-config")) { dump_config = true; ok = true; }
            else if (!strcmp(a, "probe-camera")) { g_probe_camera = true; g_probe_frames_remaining = 10; ok = true; }
            else if (i + 1 < argc) ok = set_option(o, a, argv[++i]);
        }
        if (!ok) {
            printf("usage: %s [--monitor NAME] [--capture NAME|test] "
                   "[--capture-backend auto|portal|xshm|test] [--distance M] [--size IN] "
                   "[--pitch-trim DEG] [--predict-ms MS] [--smooth-pos 0..1] [--smooth-ori 0..1] "
                   "[--ws-port N] [--window] [--config PATH] [--dump-config] [--probe-camera]\n"
                   "config: %s   state: %s\n",
                   argv[0], config_default_path().c_str(), state_file_path().c_str());
            return 0;
        }
    }
    if (o.capture == "test") { o.capture_backend = "test"; o.capture.clear(); }
    if (dump_config) {
        printf("# effective options (config %s, state %s)\n",
               config_path.c_str(), state_file_path().c_str());
        printf("monitor = %s\ncapture = %s\ncapture-backend = %s\n",
               o.monitor.c_str(), o.capture.c_str(), o.capture_backend.c_str());
        printf("distance = %.3f\nsize = %.1f\npitch-trim = %.2f\npredict-ms = %.2f\n",
               o.distance, o.size, o.pitch_trim, o.predict_ms);
        printf("smooth-pos = %.2f\nsmooth-ori = %.2f\nwindow = %s\nws-port = %d\n",
               o.smooth_pos, o.smooth_ori, o.window ? "true" : "false", o.ws_port);
        return 0;
    }
    // Local aliases: the render loop mutates distance/size at runtime.
    std::string monitor_name = o.monitor, capture_name = o.capture;
    std::string capture_backend = o.capture_backend;
    float distance = o.distance, diag_in = o.size, pitch_trim = o.pitch_trim;
    float predict_ms = o.predict_ms, smooth_pos = o.smooth_pos, smooth_ori = o.smooth_ori;
    bool force_window = o.window;
```

(Delete Task 2's separate `capture_backend`/alias lines if they exist verbatim — this block subsumes them. `#include "config.h"` joins the local includes.)

At shutdown (right before `printf("shutting down…\n");`):

```cpp
    app_state.distance = distance;
    app_state.size = diag_in;
    save_state(app_state);
```

- [ ] **Step 4: Makefile**

```make
OBJS := src/main.o src/vk_renderer.o src/vk_surface.o src/gesture_client.o \
        src/capture.o src/capture_xshm.o src/config.o

src/config.o: src/config.cpp src/config.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/main.o: src/gesture_client.h src/capture.h src/config.h
```

- [ ] **Step 5: Verify precedence headlessly**

```bash
cd /home/salvatore/Desktop/code/viture/spatial-screens && make
./spatial-screens --dump-config                       # all compiled defaults
printf 'distance = 1.5\n# comment\nbogus-key = 1\n' > /tmp/ss-test.conf
./spatial-screens --config /tmp/ss-test.conf --dump-config
./spatial-screens --config /tmp/ss-test.conf --distance 2.0 --dump-config
```

Expected: run 1 prints `distance = 0.750`; run 2 warns `unknown key 'bogus-key'` and prints `distance = 1.500`; run 3 prints `distance = 2.000` (CLI wins). If `~/.local/state/spatial-screens/state` exists from earlier experiments, its values must show up between config and CLI.

- [ ] **Step 6: Commit**

```bash
git add spatial-screens/src/config.h spatial-screens/src/config.cpp \
        spatial-screens/src/main.cpp spatial-screens/Makefile
git commit -m "spatial-screens: config file + persisted state (defaults < config < state < CLI)"
```

---

### Task 4: Portal ScreenCast session over D-Bus

**Files:**
- Create: `spatial-screens/src/capture_portal.h`
- Create: `spatial-screens/src/capture_portal.cpp` (D-Bus half; PipeWire half lands in Task 5)
- Modify: `spatial-screens/src/capture_test.cpp` (add `--portal-probe`)
- Modify: `spatial-screens/Makefile` (dbus-1 via pkg-config)

**Interfaces:**
- Produces (Task 5 depends on these exact signatures):

```cpp
struct PortalSession {
    void* conn = nullptr;          // DBusConnection*, opaque to callers
    std::string session_handle;    // object path of the live session
    uint32_t node_id = 0;          // PipeWire node to connect to
    int pw_fd = -1;                // PipeWire remote fd (caller owns)
    std::string restore_token;     // new token from Start ("" if none)
};
bool portal_open_screencast(const std::string& old_token, PortalSession& out);
void portal_close_session(PortalSession& s);
```

- Consumes: nothing from other tasks (libdbus-1 only — dev headers already installed).

- [ ] **Step 1: Write `src/capture_portal.h`**

```cpp
// xdg-desktop-portal ScreenCast session plumbing (libdbus-1) + the PipeWire
// capture backend built on it. The D-Bus dance is blocking and startup-only;
// Start()'s wait is generous because the user may be interacting with the
// system monitor-picker dialog. Exposed separately from capture.h so
// capture-test can probe the portal without a backend.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class CaptureBackend;

struct PortalSession {
    void* conn = nullptr;          // DBusConnection*, opaque to callers
    std::string session_handle;
    uint32_t node_id = 0;
    int pw_fd = -1;                // caller owns; portal_close_session() does not close it
    std::string restore_token;    // new token from Start ("" if the portal sent none)
};

// old_token: previous restore token ("" for none) — skips the picker dialog
// when the portal accepts it. Returns false (with the reason on stderr) on
// any failure, including the user cancelling the dialog.
bool portal_open_screencast(const std::string& old_token, PortalSession& out);
void portal_close_session(PortalSession& s);

// Task 5: the PipeWire-backed CaptureBackend.
std::unique_ptr<CaptureBackend> capture_create_portal(
    const std::string& old_token,
    std::function<void(const std::string&)> on_new_token);
```

- [ ] **Step 2: Write the D-Bus half of `src/capture_portal.cpp`**

```cpp
#include "capture_portal.h"
#include "capture.h"

#include <dbus/dbus.h>

#include <chrono>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------- D-Bus plumbing ----

namespace {

constexpr const char* PORTAL_DEST = "org.freedesktop.portal.Desktop";
constexpr const char* PORTAL_PATH = "/org/freedesktop/portal/desktop";
constexpr const char* SCREENCAST_IFACE = "org.freedesktop.portal.ScreenCast";
constexpr const char* REQUEST_IFACE = "org.freedesktop.portal.Request";
constexpr const char* SESSION_IFACE = "org.freedesktop.portal.Session";

std::string unique_name_component(DBusConnection* conn) {
    std::string s = dbus_bus_get_unique_name(conn);  // e.g. ":1.42" -> "1_42"
    if (!s.empty() && s[0] == ':') s.erase(0, 1);
    for (char& c : s) if (c == '.') c = '_';
    return s;
}

// ---- a{sv} builders

void dict_open(DBusMessageIter* it, DBusMessageIter* arr) {
    dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY, "{sv}", arr);
}
void dict_add(DBusMessageIter* arr, const char* key, int type, const char* sig, const void* val) {
    DBusMessageIter e, v;
    dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, sig, &v);
    dbus_message_iter_append_basic(&v, type, val);
    dbus_message_iter_close_container(&e, &v);
    dbus_message_iter_close_container(arr, &e);
}
void dict_add_str(DBusMessageIter* a, const char* k, const char* s) { dict_add(a, k, DBUS_TYPE_STRING, "s", &s); }
void dict_add_u32(DBusMessageIter* a, const char* k, uint32_t u) { dict_add(a, k, DBUS_TYPE_UINT32, "u", &u); }
void dict_add_bool(DBusMessageIter* a, const char* k, bool b) { dbus_bool_t v = b; dict_add(a, k, DBUS_TYPE_BOOLEAN, "b", &v); }

// ---- Response signal handling

DBusMessage* wait_response(DBusConnection* conn, const std::string& path_a,
                           const std::string& path_b, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!dbus_connection_read_write(conn, 100)) return nullptr;  // bus died
        while (DBusMessage* m = dbus_connection_pop_message(conn)) {
            if (dbus_message_is_signal(m, REQUEST_IFACE, "Response")) {
                const char* p = dbus_message_get_path(m);
                if (p && (path_a == p || path_b == p)) return m;
            }
            dbus_message_unref(m);
        }
    }
    return nullptr;
}

// Response signature: (u code, a{sv} results). Leaves *results at the array.
bool parse_response(DBusMessage* msg, uint32_t& code, DBusMessageIter* results) {
    DBusMessageIter it;
    if (!dbus_message_iter_init(msg, &it) ||
        dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UINT32) return false;
    dbus_message_iter_get_basic(&it, &code);
    if (!dbus_message_iter_next(&it) ||
        dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return false;
    *results = it;
    return true;
}

// Positions *value_out inside the variant of results[key].
bool results_find(DBusMessageIter results_arr, const char* key, DBusMessageIter* value_out) {
    DBusMessageIter arr;
    dbus_message_iter_recurse(&results_arr, &arr);
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter e;
        dbus_message_iter_recurse(&arr, &e);
        const char* k = nullptr;
        dbus_message_iter_get_basic(&e, &k);
        if (k && !strcmp(k, key)) {
            dbus_message_iter_next(&e);
            dbus_message_iter_recurse(&e, value_out);
            return true;
        }
        dbus_message_iter_next(&arr);
    }
    return false;
}

// One ScreenCast request-style call: AddMatch on the predicted request path,
// send, then wait for Request.Response (portals may return a different
// request handle — accept either path).
DBusMessage* screencast_call(DBusConnection* conn, const char* method,
                             const std::string& token, int timeout_ms,
                             const std::function<void(DBusMessageIter*)>& build_args) {
    std::string sender = unique_name_component(conn);
    std::string predicted = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
    std::string match = std::string("type='signal',interface='") + REQUEST_IFACE +
                        "',member='Response',path='" + predicted + "'";
    dbus_bus_add_match(conn, match.c_str(), nullptr);
    dbus_connection_flush(conn);

    DBusMessage* call = dbus_message_new_method_call(PORTAL_DEST, PORTAL_PATH,
                                                     SCREENCAST_IFACE, method);
    DBusMessageIter it;
    dbus_message_iter_init_append(call, &it);
    build_args(&it);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, call, 5000, &err);
    dbus_message_unref(call);
    if (!reply) {
        fprintf(stderr, "portal: %s failed: %s\n", method, err.message ? err.message : "?");
        dbus_error_free(&err);
        return nullptr;
    }
    std::string actual = predicted;
    DBusMessageIter rit;
    if (dbus_message_iter_init(reply, &rit) &&
        dbus_message_iter_get_arg_type(&rit) == DBUS_TYPE_OBJECT_PATH) {
        const char* rp = nullptr;
        dbus_message_iter_get_basic(&rit, &rp);
        if (rp) actual = rp;
    }
    dbus_message_unref(reply);
    if (actual != predicted) {
        std::string m2 = std::string("type='signal',interface='") + REQUEST_IFACE +
                         "',member='Response',path='" + actual + "'";
        dbus_bus_add_match(conn, m2.c_str(), nullptr);
        dbus_connection_flush(conn);
    }
    DBusMessage* resp = wait_response(conn, predicted, actual, timeout_ms);
    if (!resp) fprintf(stderr, "portal: no Response for %s within %d ms\n", method, timeout_ms);
    return resp;
}

void append_session_and_dict(DBusMessageIter* it, const char* session,
                             const std::function<void(DBusMessageIter*)>& fill) {
    dbus_message_iter_append_basic(it, DBUS_TYPE_OBJECT_PATH, &session);
    DBusMessageIter arr;
    dict_open(it, &arr);
    fill(&arr);
    dbus_message_iter_close_container(it, &arr);
}

}  // namespace

// ------------------------------------------------- portal session API ----

bool portal_open_screencast(const std::string& old_token, PortalSession& out) {
    DBusError err;
    dbus_error_init(&err);
    // Private connection: we pump it ourselves; sharing the process-wide one
    // would steal signals from any other in-process D-Bus user.
    DBusConnection* conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (!conn) {
        fprintf(stderr, "portal: session bus unavailable: %s\n", err.message ? err.message : "?");
        dbus_error_free(&err);
        return false;
    }
    dbus_connection_set_exit_on_disconnect(conn, FALSE);
    out.conn = conn;

    // 1. CreateSession
    DBusMessage* resp = screencast_call(conn, "CreateSession", "ss_req1", 5000,
        [&](DBusMessageIter* it) {
            DBusMessageIter arr;
            dict_open(it, &arr);
            dict_add_str(&arr, "handle_token", "ss_req1");
            dict_add_str(&arr, "session_handle_token", "ss_session");
            dbus_message_iter_close_container(it, &arr);
        });
    uint32_t code = 1;
    DBusMessageIter results, v;
    if (!resp || !parse_response(resp, code, &results) || code != 0 ||
        !results_find(results, "session_handle", &v)) {
        fprintf(stderr, "portal: CreateSession failed (code %u)\n", code);
        if (resp) dbus_message_unref(resp);
        portal_close_session(out);
        return false;
    }
    const char* sh = nullptr;
    dbus_message_iter_get_basic(&v, &sh);
    out.session_handle = sh ? sh : "";
    dbus_message_unref(resp);

    // 2. SelectSources — monitors only, cursor embedded (XShm never captured
    // the cursor; for a virtual monitor you look at, seeing it is the point),
    // persist_mode 2 (until revoked) + previous restore_token if we have one.
    auto select_args = [&](const char* token, uint32_t cursor_mode) {
        return [&, token, cursor_mode](DBusMessageIter* it) {
            append_session_and_dict(it, out.session_handle.c_str(), [&](DBusMessageIter* arr) {
                dict_add_str(arr, "handle_token", token);
                dict_add_u32(arr, "types", 1);        // 1 = MONITOR
                dict_add_bool(arr, "multiple", false);
                dict_add_u32(arr, "cursor_mode", cursor_mode);  // 2 = EMBEDDED
                dict_add_u32(arr, "persist_mode", 2);
                if (!old_token.empty()) dict_add_str(arr, "restore_token", old_token.c_str());
            });
        };
    };
    resp = screencast_call(conn, "SelectSources", "ss_req2", 5000, select_args("ss_req2", 2));
    bool ok = resp && parse_response(resp, code, &results) && code == 0;
    if (resp) dbus_message_unref(resp);
    if (!ok) {
        // Retry once with a plain hidden cursor in case EMBEDDED is unsupported.
        resp = screencast_call(conn, "SelectSources", "ss_req2b", 5000, select_args("ss_req2b", 1));
        ok = resp && parse_response(resp, code, &results) && code == 0;
        if (resp) dbus_message_unref(resp);
        if (!ok) {
            fprintf(stderr, "portal: SelectSources failed (code %u)\n", code);
            portal_close_session(out);
            return false;
        }
    }

    // 3. Start — long timeout: the picker dialog may be up.
    // Signature: (o session, s parent_window, a{sv} options).
    resp = screencast_call(conn, "Start", "ss_req3", 120000, [&](DBusMessageIter* it) {
        const char* session = out.session_handle.c_str();
        const char* parent = "";
        dbus_message_iter_append_basic(it, DBUS_TYPE_OBJECT_PATH, &session);
        dbus_message_iter_append_basic(it, DBUS_TYPE_STRING, &parent);
        DBusMessageIter arr;
        dict_open(it, &arr);
        dict_add_str(&arr, "handle_token", "ss_req3");
        dbus_message_iter_close_container(it, &arr);
    });
    if (!resp || !parse_response(resp, code, &results) || code != 0) {
        fprintf(stderr, "portal: Start failed (code %u)%s\n", code,
                code == 1 ? " — cancelled by user" : "");
        if (resp) dbus_message_unref(resp);
        portal_close_session(out);
        return false;
    }
    if (results_find(results, "restore_token", &v)) {
        const char* tok = nullptr;
        dbus_message_iter_get_basic(&v, &tok);
        if (tok) out.restore_token = tok;
    }
    if (!results_find(results, "streams", &v)) {
        fprintf(stderr, "portal: Start returned no streams\n");
        dbus_message_unref(resp);
        portal_close_session(out);
        return false;
    }
    {
        DBusMessageIter sa;
        dbus_message_iter_recurse(&v, &sa);  // a(ua{sv})
        if (dbus_message_iter_get_arg_type(&sa) != DBUS_TYPE_STRUCT) {
            fprintf(stderr, "portal: empty streams array\n");
            dbus_message_unref(resp);
            portal_close_session(out);
            return false;
        }
        DBusMessageIter st;
        dbus_message_iter_recurse(&sa, &st);
        dbus_message_iter_get_basic(&st, &out.node_id);
    }
    dbus_message_unref(resp);

    // 4. OpenPipeWireRemote — a plain method call (fd in the reply, no Request).
    DBusMessage* call = dbus_message_new_method_call(PORTAL_DEST, PORTAL_PATH,
                                                     SCREENCAST_IFACE, "OpenPipeWireRemote");
    DBusMessageIter it;
    dbus_message_iter_init_append(call, &it);
    append_session_and_dict(&it, out.session_handle.c_str(), [](DBusMessageIter*) {});
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, call, 5000, &err);
    dbus_message_unref(call);
    if (!reply || !dbus_message_get_args(reply, &err, DBUS_TYPE_UNIX_FD, &out.pw_fd,
                                         DBUS_TYPE_INVALID)) {
        fprintf(stderr, "portal: OpenPipeWireRemote failed: %s\n",
                err.message ? err.message : "?");
        dbus_error_free(&err);
        if (reply) dbus_message_unref(reply);
        portal_close_session(out);
        return false;
    }
    dbus_message_unref(reply);
    printf("portal: screencast ready (node %u%s)\n", out.node_id,
           out.restore_token.empty() ? "" : ", restore token received");
    return true;
}

void portal_close_session(PortalSession& s) {
    DBusConnection* conn = static_cast<DBusConnection*>(s.conn);
    if (!conn) return;
    if (!s.session_handle.empty()) {
        DBusMessage* call = dbus_message_new_method_call(
            PORTAL_DEST, s.session_handle.c_str(), SESSION_IFACE, "Close");
        dbus_connection_send(conn, call, nullptr);
        dbus_connection_flush(conn);
        dbus_message_unref(call);
    }
    dbus_connection_close(conn);
    dbus_connection_unref(conn);
    s.conn = nullptr;
    s.session_handle.clear();
}
```

- [ ] **Step 3: `--portal-probe` in capture-test**

Add to `src/capture_test.cpp` (new include `#include "capture_portal.h"`; new flag in the arg loop and usage string):

```cpp
        else if (!strcmp(argv[i], "--portal-probe")) {
            PortalSession s{};
            printf("opening portal screencast (a system picker dialog may appear)…\n");
            if (!portal_open_screencast("", s)) return 1;
            printf("portal ok: node=%u fd=%d token='%s' session=%s\n",
                   s.node_id, s.pw_fd, s.restore_token.c_str(), s.session_handle.c_str());
            if (!s.restore_token.empty()) {
                printf("retrying with restore token (no dialog should appear)…\n");
                PortalSession s2{};
                if (portal_open_screencast(s.restore_token, s2)) {
                    printf("restore ok: node=%u token='%s'\n", s2.node_id, s2.restore_token.c_str());
                    portal_close_session(s2);
                }
            }
            portal_close_session(s);
            return 0;
        }
```

- [ ] **Step 4: Makefile — dbus flags**

```make
CAP_PC := dbus-1
CAP_CFLAGS := $(shell pkg-config --cflags $(CAP_PC))
CAP_LIBS := $(shell pkg-config --libs $(CAP_PC))
CXXFLAGS += $(CAP_CFLAGS)
```

Add `src/capture_portal.o` to `OBJS` and `CAPTURE_OBJS`; add `$(CAP_LIBS)` to `LDFLAGS`; change the `capture-test` link line to `$(CXX) $^ -o $@ $(VK_LIBS) $(CAP_LIBS) -lpthread`; add the prereq lines:

```make
src/capture_portal.o: src/capture_portal.cpp src/capture_portal.h src/capture.h
	$(CXX) $(CXXFLAGS) -c $< -o $@
src/capture_test.o: src/capture_portal.h
```

(Task 5 extends `CAP_PC` with `libpipewire-0.3`.)

- [ ] **Step 5: Verify the dance live**

Run: `cd /home/salvatore/Desktop/code/viture/spatial-screens && make capture-test && ./capture-test --portal-probe`
Expected: GNOME's screen-share picker appears; pick a monitor → `portal ok: node=<N> fd=<F> token='<uuid>'…`, then the restore retry prints `restore ok` WITHOUT showing the dialog again. Also verify the cancel path: run again fresh (empty token), hit Cancel in the dialog → `portal: Start failed (code 1) — cancelled by user`, exit code 1, no crash.

- [ ] **Step 6: Commit**

```bash
git add spatial-screens/src/capture_portal.h spatial-screens/src/capture_portal.cpp \
        spatial-screens/src/capture_test.cpp spatial-screens/Makefile
git commit -m "spatial-screens: portal ScreenCast session over libdbus (probe via capture-test)"
```

---

### Task 5: PipeWire stream + PortalBackend + main integration

**Files:**
- Modify: `spatial-screens/src/capture_portal.cpp` (PipeWire half + `capture_create_portal`)
- Modify: `spatial-screens/src/capture_test.cpp` (allow `--backend portal`)
- Modify: `spatial-screens/src/main.cpp` (portal first in `auto` chain, token persistence)
- Modify: `spatial-screens/Makefile` (libpipewire-0.3)

**Interfaces:**
- Consumes: `portal_open_screencast`/`portal_close_session` (Task 4), `CaptureBackend` (Task 1), `AppState`/`save_state` (Task 3).
- Produces: working `capture_create_portal(old_token, on_new_token)` — `on_new_token` fires on the main thread during `start()` when the portal hands out a fresh restore token.

- [ ] **Step 1: Install the build dependency**

Run: `sudo apt install -y libpipewire-0.3-dev`
Expected: installs 1.0.x headers; `pkg-config --modversion libpipewire-0.3` prints `1.0.2` (or similar).

- [ ] **Step 2: Makefile**

```make
CAP_PC := dbus-1 libpipewire-0.3
```

(Everything else from Task 4's Makefile step already flows through `CAP_CFLAGS`/`CAP_LIBS`.)

- [ ] **Step 3: Add the PipeWire backend to `src/capture_portal.cpp`**

Append (new includes at the top of the file: `<pipewire/pipewire.h>`, `<spa/param/video/format-utils.h>`, `<atomic>`, `<mutex>`, `<vector>`, `<fcntl.h>`, `<unistd.h>`):

```cpp
// ------------------------------------------------- PipeWire backend ----

namespace {

class PortalBackend : public CaptureBackend {
public:
    PortalBackend(std::string old_token, std::function<void(const std::string&)> on_new_token)
        : old_token_(std::move(old_token)), on_new_token_(std::move(on_new_token)) {}
    ~PortalBackend() override { stop(); }

    bool start() override {
        if (!portal_open_screencast(old_token_, session_)) return false;
        if (!session_.restore_token.empty() && on_new_token_)
            on_new_token_(session_.restore_token);

        pw_init(nullptr, nullptr);
        loop_ = pw_thread_loop_new("ss-capture", nullptr);
        ctx_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
        if (!loop_ || !ctx_ || pw_thread_loop_start(loop_) != 0) {
            fprintf(stderr, "capture(portal): pipewire loop setup failed\n");
            return false;
        }
        pw_thread_loop_lock(loop_);
        core_ = pw_context_connect_fd(ctx_, fcntl(session_.pw_fd, F_DUPFD_CLOEXEC, 5),
                                      nullptr, 0);
        if (!core_) {
            pw_thread_loop_unlock(loop_);
            fprintf(stderr, "capture(portal): connect_fd failed\n");
            return false;
        }
        stream_ = pw_stream_new(core_, "spatial-screens",
                                pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
                                                  PW_KEY_MEDIA_CATEGORY, "Capture",
                                                  PW_KEY_MEDIA_ROLE, "Screen", nullptr));
        static const pw_stream_events EVENTS = {
            PW_VERSION_STREAM_EVENTS,
            /*.destroy =*/ nullptr,
            /*.state_changed =*/ &PortalBackend::on_state_changed,
            /*.control_info =*/ nullptr,
            /*.io_changed =*/ nullptr,
            /*.param_changed =*/ &PortalBackend::on_param_changed,
            /*.add_buffer =*/ nullptr,
            /*.remove_buffer =*/ nullptr,
            /*.process =*/ &PortalBackend::on_process,
        };
        pw_stream_add_listener(stream_, &listener_, &EVENTS, this);

        // Locals, not &SPA_RECTANGLE(...) temporaries — C++ has no compound literals.
        spa_rectangle sz_def = SPA_RECTANGLE(1920, 1080), sz_min = SPA_RECTANGLE(1, 1),
                      sz_max = SPA_RECTANGLE(8192, 8192);
        spa_fraction fr_def = SPA_FRACTION(30, 1), fr_min = SPA_FRACTION(0, 1),
                     fr_max = SPA_FRACTION(240, 1);
        uint8_t podbuf[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(podbuf, sizeof(podbuf));
        const spa_pod* params[1] = { (const spa_pod*)spa_pod_builder_add_object(&b,
            SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(3,
                SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA),
            SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&sz_def, &sz_min, &sz_max),
            SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&fr_def, &fr_min, &fr_max)) };
        alive_ = true;  // BEFORE the wait loop: errors during negotiation clear it
        if (pw_stream_connect(stream_, PW_DIRECTION_INPUT, session_.node_id,
                              (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT |
                                                PW_STREAM_FLAG_MAP_BUFFERS),
                              params, 1) != 0) {
            pw_thread_loop_unlock(loop_);
            fprintf(stderr, "capture(portal): stream connect failed\n");
            return false;
        }
        // Wait for format negotiation so callers can size the texture.
        struct timespec abst;
        pw_thread_loop_get_time(loop_, &abst, 5 * SPA_NSEC_PER_SEC);
        while (!have_format_ && alive_)
            if (pw_thread_loop_timed_wait_full(loop_, &abst) != 0) break;
        pw_thread_loop_unlock(loop_);
        if (!have_format_) {
            fprintf(stderr, "capture(portal): no video format within 5 s\n");
            return false;
        }
        return true;
    }

    // Swap-out under the lock: read_buf_ is owned by the consumer side, so
    // the PipeWire thread can never touch (or reallocate) the bytes behind
    // the pointer we hand out. Returns false when no NEW frame arrived
    // since the last call — the caller just keeps its current texture.
    bool latest_frame(CaptureFrame& out) override {
        std::lock_guard<std::mutex> lk(mtx_);
        if (front_ < 0) return false;
        read_buf_.swap(buf_[front_]);
        front_ = -1;
        out.data = read_buf_.data();
        out.w = w_; out.h = h_; out.pitch = pitch_;
        return true;
    }

    bool alive() const override { return alive_; }
    const char* name() const override { return "portal"; }

    void stop() override {
        if (loop_) {
            pw_thread_loop_lock(loop_);
            if (stream_) { pw_stream_destroy(stream_); stream_ = nullptr; }
            if (core_) { pw_core_disconnect(core_); core_ = nullptr; }
            pw_thread_loop_unlock(loop_);
            pw_thread_loop_stop(loop_);
            if (ctx_) { pw_context_destroy(ctx_); ctx_ = nullptr; }
            pw_thread_loop_destroy(loop_);
            loop_ = nullptr;
        }
        if (session_.pw_fd >= 0) { close(session_.pw_fd); session_.pw_fd = -1; }
        portal_close_session(session_);
        alive_ = false;
    }

private:
    static void on_param_changed(void* ud, uint32_t id, const spa_pod* param) {
        auto* self = static_cast<PortalBackend*>(ud);
        if (id != SPA_PARAM_Format || !param) return;
        uint32_t mt, mst;
        if (spa_format_parse(param, &mt, &mst) < 0 ||
            mt != SPA_MEDIA_TYPE_video || mst != SPA_MEDIA_SUBTYPE_raw) return;
        spa_video_info_raw info{};
        if (spa_format_video_raw_parse(param, &info) < 0) return;
        {
            std::lock_guard<std::mutex> lk(self->mtx_);
            self->w_ = int(info.size.width);
            self->h_ = int(info.size.height);
            self->pitch_ = info.size.width * 4;  // real stride comes per-buffer
        }
        self->have_format_ = true;
        pw_thread_loop_signal(self->loop_, false);
    }

    static void on_state_changed(void* ud, pw_stream_state /*old*/, pw_stream_state st,
                                 const char* error) {
        auto* self = static_cast<PortalBackend*>(ud);
        if (st == PW_STREAM_STATE_ERROR ||
            (st == PW_STREAM_STATE_UNCONNECTED && self->saw_streaming_)) {
            fprintf(stderr, "capture(portal): stream %s%s%s\n",
                    st == PW_STREAM_STATE_ERROR ? "error" : "disconnected",
                    error ? ": " : "", error ? error : "");
            self->alive_ = false;
            pw_thread_loop_signal(self->loop_, false);
        }
        if (st == PW_STREAM_STATE_STREAMING) self->saw_streaming_ = true;
    }

    static void on_process(void* ud) {
        auto* self = static_cast<PortalBackend*>(ud);
        pw_buffer* pb = pw_stream_dequeue_buffer(self->stream_);
        if (!pb) return;
        spa_buffer* sb = pb->buffer;
        if (sb->datas[0].data && self->have_format_) {
            uint32_t stride = sb->datas[0].chunk->stride;
            if (!stride) stride = uint32_t(self->w_) * 4;
            std::lock_guard<std::mutex> lk(self->mtx_);
            int back = self->front_ == 0 ? 1 : 0;
            self->buf_[back].assign(
                static_cast<const uint8_t*>(sb->datas[0].data),
                static_cast<const uint8_t*>(sb->datas[0].data) + size_t(stride) * self->h_);
            self->pitch_ = stride;
            self->front_ = back;
        }
        pw_stream_queue_buffer(self->stream_, pb);
    }

    std::string old_token_;
    std::function<void(const std::string&)> on_new_token_;
    PortalSession session_{};
    pw_thread_loop* loop_ = nullptr;
    pw_context* ctx_ = nullptr;
    pw_core* core_ = nullptr;
    pw_stream* stream_ = nullptr;
    spa_hook listener_{};
    std::atomic<bool> have_format_{false};
    std::atomic<bool> alive_{false};
    bool saw_streaming_ = false;
    std::mutex mtx_;
    std::vector<uint8_t> buf_[2];   // written by the PipeWire thread
    std::vector<uint8_t> read_buf_; // owned by the consumer after swap-out
    int front_ = -1;                // -1 = no unconsumed frame
    int w_ = 0, h_ = 0;
    uint32_t pitch_ = 0;
};

}  // namespace

std::unique_ptr<CaptureBackend> capture_create_portal(
    const std::string& old_token,
    std::function<void(const std::string&)> on_new_token) {
    return std::make_unique<PortalBackend>(std::move(old_token), std::move(on_new_token));
}
```

- [ ] **Step 4: capture-test grows `--backend portal`**

In the backend selection of `capture_test.cpp`:

```cpp
    } else if (backend == "portal") {
        cap = capture_create_portal("", [](const std::string& tok) {
            printf("portal: new restore token: %s\n", tok.c_str());
        });
    } else if (backend == "xshm") {
```

(usage string: `--backend xshm|portal|test`)

- [ ] **Step 5: main.cpp — portal first, token persisted**

In the Task 2 `switch_backend` chain setup, put portal at the front of `auto`:

```cpp
    if (capture_backend == "auto") chain = { "portal", "xshm", "test" };
```

And add the portal case inside the lambda's `if` ladder (before `xshm`):

```cpp
            if (kind == "portal") {
                if (!capture_name.empty())
                    printf("capture: --capture is ignored under portal "
                           "(the picker/restore token owns source selection)\n");
                b = capture_create_portal(app_state.restore_token,
                                          [&](const std::string& tok) {
                                              app_state.restore_token = tok;
                                              save_state(app_state);
                                          });
            } else if (kind == "xshm") {
```

(`#include "capture_portal.h"` joins the local includes. `app_state` is in scope from Task 3; `save_state` here also persists any not-yet-saved distance/size — harmless, values are current.)

- [ ] **Step 6: Verify portal capture headlessly**

```bash
cd /home/salvatore/Desktop/code/viture/spatial-screens && make capture-test
./capture-test --backend portal --frames 10        # picker appears; pick a monitor
./capture-test --backend portal --frames 5         # dialog again (harness passes no token by design)
./capture-test --backend xshm --frames 3           # regression: still fine
```

Expected: 10 PPMs of real desktop content (open one; the cursor must be visible in it — embedded cursor mode), per-frame `dt` roughly matching the desktop refresh; second run also works. `make` (full app) still builds.
Also verify the runtime-death path: start `./capture-test --backend portal --frames 200`, then revoke the grant mid-run (GNOME Settings → Privacy → Screen Sharing, or `pkill -f xdg-desktop-portal` and let it respawn) → harness prints `capture(portal): stream …` and exits via `alive=0` rather than crashing.

- [ ] **Step 7: Commit**

```bash
git add spatial-screens/src/capture_portal.cpp spatial-screens/src/capture_test.cpp \
        spatial-screens/src/main.cpp spatial-screens/Makefile
git commit -m "spatial-screens: PipeWire portal capture backend (default in the auto chain)"
```

---

### Task 6: WebSocket telemetry

**Files:**
- Create: `spatial-screens/src/telemetry.h`
- Create: `spatial-screens/src/telemetry.cpp`
- Modify: `spatial-screens/src/main.cpp`
- Modify: `spatial-screens/Makefile` (`-I../bridge`, telemetry.o)

**Interfaces:**
- Consumes: `wsrv::Server` from `bridge/ws_server.hpp` (`bool start(uint16_t, MessageHandler)`, `void stop()`, `void broadcast(const std::string&)`; `MessageHandler = std::function<void(const std::string&)>` called on the WS thread); `Options::ws_port` (Task 3).
- Produces: the `Telemetry` class below. Task 7's dashboard panel consumes the `app` wire message.

- [ ] **Step 1: Write `src/telemetry.h`**

```cpp
// WebSocket telemetry speaking the viture-bridge protocol (documented in
// bridge/main.cpp) so the phase-1 sensor-viz dashboard monitors
// spatial-screens unmodified, plus one new message type:
//   {"type":"app", fps, sixdof, anchored, distance, size, backend, direct}
// The bridge and spatial-screens never run together (single-client SDK), so
// both default to port 8765. All send_* methods rate-limit internally and
// no-op when disabled — safe to call every frame.
#pragma once
#include <atomic>
#include <cstdint>

#include "ws_server.hpp"

class Telemetry {
public:
    // port 0 = disabled by request. Bind failure warns and stays disabled —
    // telemetry is never fatal.
    bool start(uint16_t port);
    void stop();
    bool enabled() const { return enabled_; }

    void send_hello(const char* market, int pid, const char* firmware, int device_type); // <= 1/2s
    void send_pose(const float p[3], const float q[4], double t);                        // <= 60 Hz
    void send_app(float fps, bool sixdof, bool anchored, float distance,
                  float size_in, const char* backend, bool direct);                      // <= 2 Hz
    // Unthrottled. text must not contain quotes or backslashes (no escaping,
    // same contract as the bridge's log messages).
    void log(const char* level, const char* text);

    // True once per dashboard {"type":"reset_pose"} request (consume-on-read;
    // set on the WS thread, read on the render loop).
    bool reset_requested() { return reset_req_.exchange(false); }

private:
    wsrv::Server ws_;
    std::atomic<bool> reset_req_{false};
    bool enabled_ = false;
    long last_hello_ms_ = 0, last_pose_ms_ = 0, last_app_ms_ = 0;
};
```

- [ ] **Step 2: Write `src/telemetry.cpp`**

```cpp
#include "telemetry.h"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace {
long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
}  // namespace

bool Telemetry::start(uint16_t port) {
    if (port == 0) {
        printf("telemetry: disabled (--ws-port 0)\n");
        return false;
    }
    enabled_ = ws_.start(port, [this](const std::string& msg) {
        if (msg.find("\"reset_pose\"") != std::string::npos) reset_req_ = true;
    });
    if (enabled_)
        printf("telemetry: ws://127.0.0.1:%u (sensor-viz dashboard)\n", port);
    else
        fprintf(stderr, "telemetry: bind failed on port %u — continuing without\n", port);
    return enabled_;
}

void Telemetry::stop() {
    if (enabled_) ws_.stop();
    enabled_ = false;
}

void Telemetry::send_hello(const char* market, int pid, const char* fw, int device_type) {
    if (!enabled_) return;
    long t = now_ms();
    if (t - last_hello_ms_ < 2000) return;
    last_hello_ms_ = t;
    char buf[512];
    snprintf(buf, sizeof(buf),
             R"({"type":"hello","model":"%s","market_name":"%s","pid":%d,"firmware":"%s","device_type":%d,"app":"spatial-screens"})",
             market[0] ? market : "VITURE", market, pid, fw, device_type);
    ws_.broadcast(buf);
}

void Telemetry::send_pose(const float p[3], const float q[4], double t_s) {
    if (!enabled_) return;
    long t = now_ms();
    if (t - last_pose_ms_ < 1000 / 60) return;
    last_pose_ms_ = t;
    char buf[512];
    snprintf(buf, sizeof(buf),
             R"({"type":"pose","t":%.6f,"px":%.5f,"py":%.5f,"pz":%.5f,"qw":%.6f,"qx":%.6f,"qy":%.6f,"qz":%.6f})",
             t_s, p[0], p[1], p[2], q[0], q[1], q[2], q[3]);
    ws_.broadcast(buf);
}

void Telemetry::send_app(float fps, bool sixdof, bool anchored, float distance,
                         float size_in, const char* backend, bool direct) {
    if (!enabled_) return;
    long t = now_ms();
    if (t - last_app_ms_ < 500) return;
    last_app_ms_ = t;
    char buf[512];
    snprintf(buf, sizeof(buf),
             R"({"type":"app","fps":%.1f,"sixdof":%s,"anchored":%s,"distance":%.2f,"size":%.0f,"backend":"%s","direct":%s})",
             fps, sixdof ? "true" : "false", anchored ? "true" : "false",
             distance, size_in, backend, direct ? "true" : "false");
    ws_.broadcast(buf);
}

void Telemetry::log(const char* level, const char* text) {
    if (!enabled_) return;
    char buf[512];
    snprintf(buf, sizeof(buf), R"({"type":"log","level":"%s","text":"%s"})", level, text);
    ws_.broadcast(buf);
}
```

- [ ] **Step 3: Makefile**

```make
CXXFLAGS := -O2 -std=c++17 -Wall -Wextra -I$(SDK_DIR)/include -I../bridge
```

Add `src/telemetry.o` to `OBJS` plus:

```make
src/telemetry.o: src/telemetry.cpp src/telemetry.h ../bridge/ws_server.hpp
	$(CXX) $(CXXFLAGS) -c $< -o $@
src/main.o: src/gesture_client.h src/capture.h src/config.h src/telemetry.h
```

- [ ] **Step 4: Wire into main.cpp**

1. `#include "telemetry.h"` with the local includes; store the found pid: change `static XRDeviceProviderHandle g_provider` block to add `static int g_pid = 0;`, and in `sdk_init()` set `g_pid = pid;` after the scan succeeds.
2. In `main()`, right after the config/CLI block resolves options:

```cpp
    Telemetry tele;
    tele.start(uint16_t(o.ws_port));
```

3. Inside `switch_backend`, after `printf("capture: %s\n", ...)`:

```cpp
            char msg[128];
            snprintf(msg, sizeof(msg), "capture backend: %s", cap->name());
            tele.log("info", msg);
```

(the lambda already captures by reference — `tele` must be declared before `switch_backend`).
4. After `sdk_init()` succeeds, fetch identity once:

```cpp
    char market[64] = {0};
    int mlen = sizeof(market);
    xr_device_provider_get_market_name(g_pid, market, &mlen);
    char fw[128] = {0};
    int fwlen = sizeof(fw);
    xr_device_provider_get_glasses_version(g_provider, fw, &fwlen);
```

5. After the gesture-sidecar `g_gestures.start(...)` call:

```cpp
    tele.log("info", g_gestures.enabled() ? "gesture sidecar connected"
                                          : "gesture sidecar unavailable");
```

6. In the render loop, next to the hotkey handling (before the gesture block), consume dashboard recenters — same behavior as `Ctrl+Alt+Shift+R`:

```cpp
        if (tele.reset_requested()) {
            reset_pose_carina(g_provider);
            ori_offset = yaw_twist(head_q);
            place_screen();
            printf("recentered + VIO reset (dashboard)\n");
            tele.log("info", "pose reset via dashboard");
        }
```

7. In the recenter hotkey branch (`ks == XK_r`) and the fist-hold gesture recenter, add `tele.log("info", "recentered");` next to the existing printfs. In the capture tick's texture-rebuild branch (Task 2), add `tele.log("info", "capture source resized - texture rebuilt");` after the successful `vkr_init_texture`.
8. After the pose-smoothing block (still inside `if (get_gl_pose_carina(...) == 0)`... place it just after the block, guarded):

```cpp
        if (have_pose) {
            float tp[3] = { head_p.x, head_p.y, head_p.z };
            float tq[4] = { head_q.w, head_q.x, head_q.y, head_q.z };
            tele.send_pose(tp, tq, tnow);
        }
```

Note: `tnow` is computed in the capture tick below in today's code — hoist `double tnow = now_s();` to just above the gesture block so pose/hello/app can use it, and drop the duplicate declaration in the capture tick.
9. In the fps section, keep a `float last_fps = 0;` (declared with `frames`): set `last_fps = float(frames / (tnow - last_fps_t));` inside the 2 s print; every frame (right after the fps section):

```cpp
        tele.send_hello(market, g_pid, fw, XR_DEVICE_TYPE_VITURE_CARINA);
        tele.send_app(last_fps, sixdof_live, anchored, distance, diag_in,
                      cap ? cap->name() : "none", sout.direct);
```

10. Teardown, next to `g_gestures.stop();`:

```cpp
    tele.stop();
```

- [ ] **Step 5: Build + headless sanity**

Run: `cd /home/salvatore/Desktop/code/viture/spatial-screens && make`
Expected: clean build. Then `./spatial-screens --ws-port 0 --dump-config` still exits before any server starts; a plain no-glasses run prints `telemetry: ws://127.0.0.1:8765 …` then `No supported VITURE glasses found.` — confirm the port is released after exit (`ss -tlnp | grep 8765` → empty). Live dashboard verification happens in Task 8's hardware pass.

- [ ] **Step 6: Commit**

```bash
git add spatial-screens/src/telemetry.h spatial-screens/src/telemetry.cpp \
        spatial-screens/src/main.cpp spatial-screens/Makefile
git commit -m "spatial-screens: WebSocket telemetry on 8765 (bridge protocol + app status)"
```

---

### Task 7: sensor-viz app panel

**Files:**
- Modify: `sensor-viz/src/drivers/bridge-ws.js`
- Modify: `sensor-viz/src/ui/panels.js`
- Modify: `sensor-viz/src/main.js`
- Modify: `sensor-viz/index.html`

**Interfaces:**
- Consumes: the `app` wire message (Task 6, exact shape in Global Constraints).
- Produces: `showAppPanel(msg)` / `hideAppPanel()` exported from `panels.js`.

- [ ] **Step 1: Forward `app` messages in `bridge-ws.js`**

In the `_handle(msg)` switch, before `default:`:

```js
    case 'app':
      // spatial-screens status (fps, tracking, screen placement).
      this._emit('app', msg);
      break;
```

- [ ] **Step 2: Panel markup in `index.html`**

After the `</section>` of `#device-card` (before `#log-card`):

```html
    <section id="app-card" class="card" hidden>
      <div class="card-head"><h2>spatial-screens</h2></div>
      <dl class="kv mono">
        <dt>FPS</dt><dd id="app-fps">—</dd>
        <dt>Tracking</dt><dd id="app-sixdof">—</dd>
        <dt>Anchored</dt><dd id="app-anchored">—</dd>
        <dt>Distance</dt><dd id="app-distance">—</dd>
        <dt>Size</dt><dd id="app-size">—</dd>
        <dt>Capture</dt><dd id="app-backend">—</dd>
        <dt>Output</dt><dd id="app-mode">—</dd>
      </dl>
    </section>
```

- [ ] **Step 3: Panel logic in `panels.js`**

Append:

```js
export function showAppPanel(msg) {
  $('app-card').hidden = false;
  $('app-fps').textContent = msg.fps != null ? msg.fps.toFixed(0) : '—';
  $('app-sixdof').textContent = msg.sixdof ? '6DoF LIVE' : 'orientation only';
  $('app-anchored').textContent = msg.anchored ? 'yes' : 'no';
  $('app-distance').textContent = msg.distance != null ? `${fmt(msg.distance, 2)} m` : '—';
  $('app-size').textContent = msg.size != null ? `${msg.size}"` : '—';
  $('app-backend').textContent = msg.backend ?? '—';
  $('app-mode').textContent = msg.direct ? 'direct' : 'window';
}

export function hideAppPanel() {
  $('app-card').hidden = true;
}
```

- [ ] **Step 4: Wire in `main.js`**

Add `showAppPanel, hideAppPanel` to the `./ui/panels.js` import list. In `connectBridge()`, with the other `d.on(...)` lines:

```js
  d.on('app', (msg) => showAppPanel(msg));
```

In `teardown()`, after `showDeviceState({});`:

```js
  hideAppPanel();
```

- [ ] **Step 5: Lint + tests + build**

Run: `cd /home/salvatore/Desktop/code/viture/sensor-viz && npm run lint && npm test && npm run build`
Expected: lint clean, existing Vitest suite passes, build succeeds. (Live rendering of the panel is checked in Task 8's hardware pass; until then the panel stays hidden because only spatial-screens emits `app`.)

- [ ] **Step 6: Commit**

```bash
git add sensor-viz/src/drivers/bridge-ws.js sensor-viz/src/ui/panels.js \
        sensor-viz/src/main.js sensor-viz/index.html
git commit -m "sensor-viz: spatial-screens status panel (app telemetry messages)"
```

---

### Task 8: Docs + glasses-on verification handoff

**Files:**
- Modify: `spatial-screens/README.md`
- Modify: `docs/plan/roadmap.md`
- Modify: `CLAUDE.md`
- Create: `docs/testing/2026-07-04-m3-remainder-test-handoff.md`

**Interfaces:** none — documentation and the hardware checklist.

- [ ] **Step 1: README**

In `spatial-screens/README.md`:
- Title/scope: retitle to `# spatial-screens (phase 2 — M3)` and update the scope list: mark the M3-remainder line done (`✅ M3: PipeWire portal capture (portal-preferred with XShm fallback), config file + persisted state, WebSocket telemetry to the phase-1 dashboard`), leaving the M4 line as `⏭`.
- Options paragraph: add `--capture-backend auto|portal|xshm|test` (default `auto`: portal → xshm → test; portal remembers the picked monitor via a restore token, and `--capture NAME` applies to xshm only), `--ws-port N` (default 8765; 0 disables), `--config PATH`, `--dump-config`.
- New `## Config` section: config at `~/.config/spatial-screens.conf` (`key = value`, same names as the long flags, `#` comments); state at `~/.local/state/spatial-screens/state` (app-written: live-tuned distance/size + portal restore token); precedence `defaults < config < state < CLI`.
- New `## Telemetry` section: bridge protocol on `ws://127.0.0.1:8765`, the sensor-viz dashboard's "Connect Bridge" button works while spatial-screens runs (never simultaneously with viture-bridge — single-client SDK); the dashboard's Recenter/reset_pose button triggers the full VIO-reset recenter.
- Notes/deps: replace the "X11 session only for capture" note (portal capture is session-agnostic; XShm fallback is X11-only) and add `libpipewire-0.3-dev` (and note dbus-1 dev headers) to build deps.

- [ ] **Step 2: roadmap.md**

In `docs/plan/roadmap.md`, under Phase 2, add below the Sequencing list:

```markdown
### Status

- M0–M2: done (bridge = 6DoF spike; direct-mode Vulkan renderer glasses-validated 2026-07-04).
- M3: done — portal/XShm capture chain, config + state files, WS telemetry to the
  phase-1 dashboard (spec: `docs/specs/2026-07-04-m3-remainder-design.md`).
  Hand-gesture control (pinch-drag, fist-hold) also merged.
- Next: M4 preset & layout engine; M5 outreach.
```

- [ ] **Step 3: CLAUDE.md**

Replace the stale line:

```markdown
- Phase 2 (spatial virtual screens) exists only as a plan in `docs/plan/phase2-spatial-screens.md` — nothing is implemented yet
```

with:

```markdown
- `spatial-screens/` — phase-2 native app (C++17 + Vulkan): one world-anchored virtual screen on the glasses, direct-display by default, portal/XShm capture chain, gesture sidecar (`gestures/`, Python/MediaPipe), config in `~/.config/spatial-screens.conf`, WS telemetry on 8765. Build deps beyond the SDK: `libvulkan-dev`, `libpipewire-0.3-dev`, dbus-1 dev headers. Like the bridge: launch via `./run.sh`, never with viture-bridge running (single-client SDK)
```

- [ ] **Step 4: Write the hardware handoff checklist**

Create `docs/testing/2026-07-04-m3-remainder-test-handoff.md`:

```markdown
# M3 remainder — glasses-on verification (handoff)

Build: `cd spatial-screens && make`. Stop `viture-bridge` first. Launch only
via `./run.sh`. Never double-start. After any hard kill in direct mode:
`xrandr --output DP-1 --set non-desktop 0 && xrandr --output DP-1 --auto`.

- [ ] 1. `./run.sh --pitch-trim 16` — portal picker appears once; pick the
      laptop monitor. Virtual screen shows that monitor live, cursor visible,
      ~118–120 fps in direct mode. Console says `capture: portal`.
- [ ] 2. Quit (Ctrl+Alt+Q), relaunch — NO picker dialog (restore token from
      `~/.local/state/spatial-screens/state`), same monitor streams.
- [ ] 3. Live-tune with hotkeys (Ctrl+Alt+[ ] - =), quit, relaunch — tuned
      distance/size persist. Then `--distance 0.75` on the CLI overrides them.
- [ ] 4. `--capture-backend xshm` — behavior parity with the pre-M3 build
      (monitor content, resize/reflow survival via RandR events).
- [ ] 5. Fallback: `systemctl --user stop xdg-desktop-portal` then launch —
      console shows portal failure → `capture: xshm`, app still works.
      Restart the portal service afterwards.
- [ ] 6. Dashboard: `cd sensor-viz && npm run dev`, open http://localhost:5173,
      Connect Bridge while spatial-screens runs → device info + live pose in
      the 3D view + "spatial-screens" panel (fps ≈118, 6DoF LIVE, distance/
      size/backend/direct correct, values track hotkey changes within ~1 s).
- [ ] 7. Dashboard Recenter button → screen re-places in front of you; event
      log shows "pose reset via dashboard".
- [ ] 8. Gestures still work (pinch-drag distance, fist-hold recenter) — the
      capture refactor must not disturb the camera callback path.
- [ ] 9. Kill -9 the app mid-run → relaunch works; portal session from the
      dead run doesn't wedge the new one (GNOME may show a stale sharing
      indicator until the next session — cosmetic only).
```

- [ ] **Step 5: Full-repo verification sweep**

```bash
cd /home/salvatore/Desktop/code/viture/spatial-screens && make clean && make && make vk-test capture-test
cd ../sensor-viz && npm run lint && npm test && npm run build
cd ../bridge && make
```

Expected: everything builds; lint/tests pass; bridge unaffected.

- [ ] **Step 6: Commit**

```bash
git add spatial-screens/README.md docs/plan/roadmap.md CLAUDE.md \
        docs/testing/2026-07-04-m3-remainder-test-handoff.md
git commit -m "spatial-screens: document M3 completion (README, roadmap, CLAUDE.md, test handoff)"
```

- [ ] **Step 7: Hardware pass (requires the glasses + user)**

Work through `docs/testing/2026-07-04-m3-remainder-test-handoff.md` with the user, ticking the checkboxes in the doc; fix-forward anything that fails and commit fixes as `spatial-screens: <finding> (M3 hardware pass)`.
