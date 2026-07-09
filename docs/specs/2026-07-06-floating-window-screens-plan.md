# Floating Window Screens — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a spatial-screen show a single application window (XComposite-captured, its own texture) instead of only a physical-monitor slice — grabbed onto the active screen (or spawned) with `Ctrl+Alt+W`, framed, and labelled with its native resolution.

**Architecture:** Add a `WindowBackend` (`CaptureBackend` sibling reusing `XShmGetImage` on the redirected window's pixmap). Grow the single-texture renderer into a fixed array of N=8 per-source textures (+1 label) selected per-draw by `ScreenInst.source_index`. Add append-only runtime spawn, a per-window gray/white frame (green when active), and the first on-glasses glyphs (a minimal bitmap-digit `text_raster`) for the active screen's `W×H`.

**Tech Stack:** C++17, Vulkan, Xlib + XShm + **XComposite** (X11 only), MIT-SHM, the project's `make`-based build with CHECK-macro unit-test binaries.

**Design:** [`2026-07-06-floating-window-screens-design.md`](2026-07-06-floating-window-screens-design.md) (freshness-reconciled 2026-07-09).

## Global Constraints

- **Platform:** Linux x86_64, **X11 session only** (verified `XDG_SESSION_TYPE=x11`). No Wayland path.
- **Build/run:** build with `make` in `spatial-screens/`; run **only** via `./run.sh` (sets `LD_LIBRARY_PATH`); **never** while `viture-bridge` runs (single-client SDK). Pure-logic tests run standalone (`make test`).
- **C++ conventions:** snake_case functions, `g_` prefix for atomic globals, 2-space indent (match surrounding code).
- **No dependency creep:** the only new link is `-lXcomposite` (`libXcomposite-dev` + runtime `Composite` verified present 2026-07-09). No font library — glyphs are a hardcoded bitmap.
- **Slots:** `kSourceSlots = 8` capture sources — index **0 = shared monitor** texture (unchanged), **1..7 = window** textures; renderer index **`kLabelSource = 8`** = the active-screen label texture. Descriptor pool `maxSets = kSourceSlots + 1 = 9`.
- **`ScreenInst.source_index`:** `0` = monitor (sub-rect UV, today's path); `≥1` = window (full-frame UV `{0,0,1,1}`); `<0` = placeholder (dead/unresolved window → untextured quad).
- **Pixels:** 32bpp BGRX (`VK_FORMAT_B8G8R8A8_UNORM`); a 32-bit-visual window pixmap is BGRA → **force alpha opaque** on window upload.
- **Never crash on a vanished window.** Spawn is **append-only** — a closed window's screen becomes an inert placeholder; the scene vector never shrinks (so `active_screen` never invalidates).

## File Structure

- `src/source_slots.h` (**new**, header-only, pure) — `kSourceSlots`, `kLabelSource`, `SourceSlots` allocator.
- `src/text_raster.{h,cpp}` (**new**, pure) — bitmap-digit rasterizer (`0-9`, `x`-as-`×`).
- `src/text_raster_test.cpp` (**new**) — unit tests; Makefile target `text-raster-test`.
- `src/capture_window.cpp` (**new**) — `WindowBackend`.
- `src/capture_window_test.cpp` (**new**, X-gated) — smoke test; Makefile target `capture-window-test`.
- `src/config.{h,cpp}` — `ScreenCfg.source` / `window_match` + parser whitelist.
- `src/scene.{h,cpp}` — `source_index`/`src_w`/`src_h` on `ScreenInst`; window-source build; `scene_window_resize` pure helper.
- `src/vk_renderer.{h,cpp}` — per-source texture/dset array (+label); `vkr_set_source_size`/`vkr_upload_source`; `source_index` on `QuadDraw`; per-draw dset bind.
- `src/main.cpp` — source-slot + window-backend list; per-frame pump + teardown; `Ctrl+Alt+W` handler + spawn; config window resolution; label quad; per-window frame; telemetry count.
- `src/telemetry.{h,cpp}` — window-source count.
- `Makefile` — `-lXcomposite`, new objects + test targets.
- `docs/branches/feat-floating-window-screens.md` — resume doc (update as tasks land).

---

### Task 1: Config keys `source` + `window-match`

**Files:**
- Modify: `spatial-screens/src/config.h:13-24` (`ScreenCfg`)
- Modify: `spatial-screens/src/config.cpp:97-116` (`set_screen_option`)
- Test: `spatial-screens/src/stereo_math_test.cpp` (extend `test_config_keys`)

**Interfaces:**
- Consumes: existing `set_option` / `set_screen_option` grammar (`screen.N.field`).
- Produces: `ScreenCfg.source` (default `"monitor"`), `ScreenCfg.window_match` (default empty). Config keys `screen.N.source`, `screen.N.window-match`.

- [ ] **Step 1: Write the failing test** — add to `test_config_keys()` in `stereo_math_test.cpp` (after the existing `screen.1.size` checks, before the unknown-key checks):

```cpp
    // Window-source keys.
    CHECK(o.screens[0].source == "monitor");            // default
    CHECK(set_option(o, "screen.1.source", "window"));
    CHECK(o.screens[0].source == "window");
    CHECK(set_option(o, "screen.1.window-match", "code"));
    CHECK(o.screens[0].window_match == "code");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spatial-screens && make stereo-math-test`
Expected: FAIL to compile — `'struct ScreenCfg' has no member named 'source'`.

- [ ] **Step 3: Add the fields** — in `config.h`, inside `ScreenCfg` (after `float size = 24.f;`):

```cpp
    std::string source = "monitor";  // "monitor" | "window"
    std::string window_match;        // WM_CLASS/title substring, or ":active"
```

- [ ] **Step 4: Extend the parser whitelist + assignment** — in `config.cpp` `set_screen_option`, change the field validation (currently ends `f != "size"`) to also allow the new fields, and add the assignments. Replace the whitelist `if` and add two `else if` lines:

```cpp
    if (f != "monitor" && f != "azimuth" && f != "elevation" &&
        f != "distance" && f != "size" && f != "source" && f != "window-match") {
        return false;
    }
```
and after `else if (f == "size") parse_float(k, v, s.size);`:
```cpp
    else if (f == "source") s.source = v;
    else if (f == "window-match") s.window_match = v;
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd spatial-screens && make stereo-math-test && ./stereo-math-test`
Expected: PASS (prints nothing / exits 0).

- [ ] **Step 6: Commit**

```bash
git add spatial-screens/src/config.h spatial-screens/src/config.cpp spatial-screens/src/stereo_math_test.cpp
git commit -m "feat(floating-windows): screen.N.source / window-match config keys"
```

---

### Task 2: Scene — `source_index`, window-source build, resize helper

**Files:**
- Modify: `spatial-screens/src/scene.h:13-17` (`ScreenInst`) + add `scene_window_resize` decl
- Modify: `spatial-screens/src/scene.cpp:34-60` (`scene_build`)
- Test: `spatial-screens/src/stereo_math_test.cpp` (new `test_window_screens`)

**Interfaces:**
- Consumes: `ScreenCfg.source` (Task 1); existing `scene_build(cfg, monitors, fb)`.
- Produces:
  - `ScreenInst.source_index` (int, default 0), `ScreenInst.src_w`/`src_h` (int, default 0 = unbound).
  - `void scene_window_resize(ScreenInst& s, int new_w, int new_h);` — on first bind (`src_w==0`) sets aspect + dims only; on later resize scales `s.cfg.size` by the pixel-diagonal ratio and updates aspect + dims.
  - `scene_build` emits a **window placeholder** (`source_index=-1`, uv `{0,0,1,1}`, no monitor lookup) for any `cfg.source=="window"`.

- [ ] **Step 1: Write the failing test** — add to `stereo_math_test.cpp` and call it from `main()`:

```cpp
static void test_window_screens() {
    MonRect fb{"eDP-1", 0, 0, 3840, 2400};
    std::vector<MonRect> mons = {{"VS1", 0, 0, 1920, 1200}};

    ScreenCfg w; w.source = "window";           // no monitor needed
    ScreenCfg m; m.source = "monitor"; m.monitor = "VS1";
    auto scene = scene_build({w, m}, mons, fb);
    CHECK(scene.size() == 2);
    // Window screen: placeholder slot, full-frame UV.
    CHECK(scene[0].source_index == -1);
    CHECK(scene[0].uv[0] == 0.f && scene[0].uv[1] == 0.f &&
          scene[0].uv[2] == 1.f && scene[0].uv[3] == 1.f);
    // Monitor screen unchanged: slot 0, sub-rect UV.
    CHECK(scene[1].source_index == 0);
    CHECK(std::fabs(scene[1].uv[2] - 0.5f) < 1e-6f);  // 1920/3840

    // First bind sets aspect + dims, size unchanged.
    ScreenInst& s = scene[0];
    s.cfg.size = 24.f;
    scene_window_resize(s, 1920, 1080);
    CHECK(s.src_w == 1920 && s.src_h == 1080);
    CHECK(std::fabs(s.aspect - 1920.f / 1080.f) < 1e-4f);
    CHECK(std::fabs(s.cfg.size - 24.f) < 1e-4f);      // unchanged on first bind

    // Resize scales size by the pixel-diagonal ratio (2x each dim -> 2x diag).
    scene_window_resize(s, 3840, 2160);
    CHECK(std::fabs(s.cfg.size - 48.f) < 1e-3f);
    CHECK(std::fabs(s.aspect - 3840.f / 2160.f) < 1e-4f);
}
```
Add `test_window_screens();` to `main()` alongside the other `test_*()` calls.

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spatial-screens && make stereo-math-test`
Expected: FAIL to compile — no member `source_index` / `scene_window_resize` not declared.

- [ ] **Step 3: Extend `ScreenInst` + declare the helper** — in `scene.h`, `ScreenInst` becomes:

```cpp
struct ScreenInst {
    ScreenCfg cfg;
    float uv[4] = {0, 0, 1, 1};  // u0,v0,u1,v1 into the source texture
    float aspect = 16.f / 10.f;  // pixel aspect (window: w/h)
    int source_index = 0;        // 0 monitor · >=1 window · <0 placeholder
    int src_w = 0, src_h = 0;     // bound source pixel dims (0 = unbound)
};
```
and add after `scene_build`'s declaration:
```cpp
// Rebind a window screen to new source pixel dims. First bind (src_w==0):
// set aspect + dims only. Later resize: scale cfg.size by the pixel-diagonal
// ratio (panel follows the desktop resize) and update aspect + dims.
void scene_window_resize(ScreenInst& s, int new_w, int new_h);
```

- [ ] **Step 4: Implement window-source build + the helper** — in `scene.cpp`:

In `scene_build`, inside the `for (auto& c : cfg)` loop, handle window sources before the monitor lookup:
```cpp
    for (auto& c : cfg) {
        if (c.source == "window") {
            ScreenInst s;
            s.cfg = c;
            s.uv[0] = 0; s.uv[1] = 0; s.uv[2] = 1; s.uv[3] = 1;
            s.source_index = -1;   // unresolved until a window is bound
            out.push_back(s);
            continue;
        }
        const MonRect* m = find_monitor(monitors, c.monitor);
        ...
    }
```
And add the helper (after `scene_screen_pose` or near the bottom, above `pick_gaze_screen`):
```cpp
void scene_window_resize(ScreenInst& s, int new_w, int new_h) {
    if (new_w <= 0 || new_h <= 0) return;
    if (s.src_w > 0 && s.src_h > 0) {
        float old_d = std::sqrt(float(s.src_w) * s.src_w + float(s.src_h) * s.src_h);
        float new_d = std::sqrt(float(new_w) * new_w + float(new_h) * new_h);
        if (old_d > 0) s.cfg.size *= new_d / old_d;
    }
    s.src_w = new_w; s.src_h = new_h;
    s.aspect = float(new_w) / float(new_h);
}
```
(`<cmath>` is already included.)

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd spatial-screens && make stereo-math-test && ./stereo-math-test`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add spatial-screens/src/scene.h spatial-screens/src/scene.cpp spatial-screens/src/stereo_math_test.cpp
git commit -m "feat(floating-windows): ScreenInst.source_index + window build + resize helper"
```

---

### Task 3: Source-slot allocator (`source_slots.h`)

**Files:**
- Create: `spatial-screens/src/source_slots.h`
- Test: `spatial-screens/src/stereo_math_test.cpp` (new `test_source_slots`)

**Interfaces:**
- Produces: `constexpr int kSourceSlots = 8;`, `constexpr int kLabelSource = kSourceSlots;`, and:
  ```cpp
  struct SourceSlots {   // slot 0 reserved for the monitor; alloc hands out 1..kSourceSlots-1
      bool used[kSourceSlots] = {};
      int alloc();       // lowest free window slot in [1, kSourceSlots), or -1 if full
      void release(int i);
  };
  ```

- [ ] **Step 1: Write the failing test** — add to `stereo_math_test.cpp` + call from `main()`:

```cpp
#include "source_slots.h"
static void test_source_slots() {
    SourceSlots s;
    CHECK(s.alloc() == 1);           // slot 0 is the monitor, never handed out
    CHECK(s.alloc() == 2);
    s.release(1);
    CHECK(s.alloc() == 1);            // lowest free reused
    for (int i = 3; i < kSourceSlots; i++) CHECK(s.alloc() == i);  // fill 3..7
    CHECK(s.alloc() == -1);          // full (1,2 already out + 3..7)
    s.release(4);
    CHECK(s.alloc() == 4);
    CHECK(kLabelSource == kSourceSlots);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd spatial-screens && make stereo-math-test`
Expected: FAIL to compile — `source_slots.h: No such file`.

- [ ] **Step 3: Create `source_slots.h`**

```cpp
// Fixed source-slot bookkeeping for spatial-screens. Slot 0 is the shared
// monitor texture; window sources take slots 1..kSourceSlots-1; the renderer
// reserves index kLabelSource for the active-screen resolution label.
#pragma once

constexpr int kSourceSlots = 8;         // 0 monitor + 1..7 window
constexpr int kLabelSource = kSourceSlots;  // renderer label texture index

struct SourceSlots {
    bool used[kSourceSlots] = {};
    int alloc() {
        for (int i = 1; i < kSourceSlots; i++)
            if (!used[i]) { used[i] = true; return i; }
        return -1;
    }
    void release(int i) {
        if (i >= 1 && i < kSourceSlots) used[i] = false;
    }
};
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd spatial-screens && make stereo-math-test && ./stereo-math-test`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add spatial-screens/src/source_slots.h spatial-screens/src/stereo_math_test.cpp
git commit -m "feat(floating-windows): source-slot allocator (header-only, unit-tested)"
```

---

### Task 4: `text_raster` — bitmap-digit rasterizer

**Files:**
- Create: `spatial-screens/src/text_raster.h`, `spatial-screens/src/text_raster.cpp`
- Create: `spatial-screens/src/text_raster_test.cpp`
- Modify: `spatial-screens/Makefile` (new object + `text-raster-test` target + `test:`)

**Interfaces:**
- Produces:
  ```cpp
  struct RasterBuf { std::vector<uint8_t> data; int w = 0, h = 0; };  // 32bpp BGRX, tight (pitch = w*4)
  // Render chars '0'..'9' and 'x' (drawn as a small '×'); unknown chars are
  // skipped. `scale` >= 1 magnifies each 5x7 cell; 1px spacing between glyphs.
  RasterBuf text_raster_render(const std::string& s, uint32_t fg_bgrx,
                               uint32_t bg_bgrx, int scale);
  ```
- Consumes: nothing (pure).

- [ ] **Step 1: Write the failing test** — `text_raster_test.cpp`:

```cpp
#include "text_raster.h"
#include <cstdio>
static int failures = 0;
#define CHECK(c) do { if(!(c)){ printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c); failures++; } } while(0)

int main() {
    // Font cell 5x7, +1px inter-glyph spacing. "12" at scale 1:
    //   width = 2*5 + 1(spacing) = 11 ; height = 7.
    RasterBuf b = text_raster_render("12", 0x00FFFFFF, 0x00000000, 1);
    CHECK(b.w == 11);
    CHECK(b.h == 7);
    CHECK((int)b.data.size() == b.w * b.h * 4);

    // scale 2 doubles cell + spacing: width = 2*(5*2) + 1*2 = 22, height = 14.
    RasterBuf b2 = text_raster_render("12", 0x00FFFFFF, 0x00000000, 2);
    CHECK(b2.w == 22);
    CHECK(b2.h == 14);

    // 'x' is a valid glyph; unknown chars skipped (no width contribution).
    RasterBuf bx = text_raster_render("1x2", 0x00FFFFFF, 0, 1);
    CHECK(bx.w == 3*5 + 2);          // 3 glyphs + 2 spaces
    RasterBuf bu = text_raster_render("1?2", 0x00FFFFFF, 0, 1);
    CHECK(bu.w == 2*5 + 1);          // '?' skipped -> 2 glyphs + 1 space

    // Background fill honored on an all-unknown string of zero glyphs -> empty.
    RasterBuf be = text_raster_render("", 0, 0, 1);
    CHECK(be.w == 0 && be.h == 0 && be.data.empty());

    if (failures) { printf("%d FAILURES\n", failures); return 1; }
    printf("ok\n"); return 0;
}
```

- [ ] **Step 2: Wire the Makefile + run to verify it fails** — in `Makefile` add after the `gesture-manip-test` block:

```make
src/text_raster.o: src/text_raster.cpp src/text_raster.h
	$(CXX) $(CXXFLAGS) -c $< -o $@
src/text_raster_test.o: src/text_raster_test.cpp src/text_raster.h
	$(CXX) $(CXXFLAGS) -c $< -o $@
# Pure rasterizer test — no SDK/Vulkan/X deps.
text-raster-test: src/text_raster_test.o src/text_raster.o
	$(CXX) $^ -o $@
```
and add `text-raster-test` to the `test:` target list + its `./text-raster-test` run line, and to `clean:`.

Run: `cd spatial-screens && make text-raster-test`
Expected: FAIL — `text_raster.h: No such file`.

- [ ] **Step 3: Create `text_raster.h`**

```cpp
// Minimal on-glasses text: a hardcoded 5x7 bitmap font for '0'-'9' and 'x'
// (drawn as a multiplication sign). Renders a tight 32bpp BGRX buffer. Used
// for the active-screen resolution label (see floating-window-screens design).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct RasterBuf { std::vector<uint8_t> data; int w = 0, h = 0; };

RasterBuf text_raster_render(const std::string& s, uint32_t fg_bgrx,
                             uint32_t bg_bgrx, int scale);
```

- [ ] **Step 4: Create `text_raster.cpp`** — 5x7 glyphs as 7 row-bytes (low 5 bits, MSB=leftmost):

```cpp
#include "text_raster.h"

namespace {
constexpr int CW = 5, CH = 7, GAP = 1;

// Each glyph: 7 bytes, bit 4 (0x10) = leftmost column, bit 0 = rightmost.
struct Glyph { char c; uint8_t rows[CH]; };
const Glyph FONT[] = {
    {'0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {'1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2', {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
    {'3', {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
    {'4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {'5', {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6', {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
    {'7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {'9', {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    {'x', {0x00,0x11,0x0A,0x04,0x0A,0x11,0x00}},  // multiplication cross
};
const Glyph* find_glyph(char c) {
    for (auto& g : FONT) if (g.c == c) return &g;
    return nullptr;
}
}  // namespace

RasterBuf text_raster_render(const std::string& s, uint32_t fg, uint32_t bg, int scale) {
    if (scale < 1) scale = 1;
    // Count renderable glyphs for the width.
    int n = 0;
    for (char c : s) if (find_glyph(c)) n++;
    RasterBuf out;
    if (n == 0) return out;
    out.w = (n * CW + (n - 1) * GAP) * scale;
    out.h = CH * scale;
    out.data.assign(size_t(out.w) * out.h * 4, 0);
    auto put = [&](int x, int y, uint32_t px) {
        uint8_t* p = &out.data[(size_t(y) * out.w + x) * 4];
        p[0] = px & 0xFF; p[1] = (px >> 8) & 0xFF; p[2] = (px >> 16) & 0xFF; p[3] = 0xFF;
    };
    // Background fill.
    for (int y = 0; y < out.h; y++) for (int x = 0; x < out.w; x++) put(x, y, bg);
    int penx = 0;
    for (char c : s) {
        const Glyph* g = find_glyph(c);
        if (!g) continue;
        for (int gy = 0; gy < CH; gy++)
            for (int gx = 0; gx < CW; gx++)
                if (g->rows[gy] & (1 << (CW - 1 - gx)))
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++)
                            put((penx + gx) * scale + sx, gy * scale + sy, fg);
        penx += CW + GAP;
    }
    return out;
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd spatial-screens && make text-raster-test && ./text-raster-test`
Expected: `ok`, exit 0.

- [ ] **Step 6: Commit**

```bash
git add spatial-screens/src/text_raster.h spatial-screens/src/text_raster.cpp spatial-screens/src/text_raster_test.cpp spatial-screens/Makefile
git commit -m "feat(floating-windows): text_raster bitmap-digit label renderer + tests"
```

---

### Task 5: `WindowBackend` — XComposite window capture

**Files:**
- Modify: `spatial-screens/src/capture.h` (factory + `source_gone` accessor)
- Create: `spatial-screens/src/capture_window.cpp`
- Create: `spatial-screens/src/capture_window_test.cpp` (X-gated smoke)
- Modify: `spatial-screens/Makefile` (`-lXcomposite`, object, test target)

**Interfaces:**
- Consumes: `CaptureBackend`, `CaptureFrame` (`capture.h`).
- Produces: `std::unique_ptr<CaptureBackend> capture_create_window(Window win, int hz);`
  - `latest_frame()` fills `CaptureFrame{data(BGRX), w, h, pitch}` at the window's **current pixel size**.
  - `alive()` returns **false once the window is destroyed** (owner then frees the slot). `composites_cursor()` → false.

> **Read first:** `capture_xshm.cpp` in full — `WindowBackend` mirrors its structure (own `Display`, own thread, triple SHM buffer, `latest_frame` handing out `img_[reading_]->data`). The only differences are the grab target (a redirected window's named pixmap, re-named on resize) and destroy/resize tracking via `StructureNotify`.

- [ ] **Step 1: Add the factory + link** — in `capture.h`, after `capture_create_xshm(...)`:
```cpp
std::unique_ptr<CaptureBackend> capture_create_window(Window win, int hz);
```
In `Makefile:6` change `VK_LIBS` to append `-lXcomposite`:
```make
VK_LIBS := -lvulkan -lX11 -lXext -lXrandr -lXfixes -lXcomposite
```
Add `src/capture_window.o` to `OBJS` (line 20-21) and a build dep line near the other capture objects:
```make
src/capture_window.o: src/capture_window.cpp src/capture.h src/vk_surface.h
	$(CXX) $(CXXFLAGS) -c $< -o $@
```

- [ ] **Step 2: Write the X-gated smoke test** — `capture_window_test.cpp`:

```cpp
// X-gated smoke: skips cleanly (exit 0) if no display or XComposite absent.
#include "capture.h"
#include <X11/extensions/Xcomposite.h>
#include <cstdio>
#include <thread>
#include <chrono>

int main() {
    Display* d = XOpenDisplay(nullptr);
    if (!d) { printf("SKIP no display\n"); return 0; }
    int ev, er;
    if (!XCompositeQueryExtension(d, &ev, &er)) { printf("SKIP no composite\n"); return 0; }
    Window root = DefaultRootWindow(d);
    Window w = XCreateSimpleWindow(d, root, 0, 0, 200, 100, 0, 0, 0);
    XMapWindow(d, w); XSync(d, False);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto cap = capture_create_window(w, 60);
    if (!cap->start()) { printf("FAIL start\n"); return 1; }
    CaptureFrame f{};
    for (int i = 0; i < 50 && !cap->latest_frame(f); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    int rc = 0;
    if (f.w != 200 || f.h != 100) { printf("FAIL dims %dx%d\n", f.w, f.h); rc = 1; }
    cap->stop();
    XDestroyWindow(d, w); XCloseDisplay(d);
    if (!rc) printf("ok\n");
    return rc;
}
```
Add to `Makefile`:
```make
src/capture_window_test.o: src/capture_window_test.cpp src/capture.h
	$(CXX) $(CXXFLAGS) -c $< -o $@
capture-window-test: src/capture_window_test.o src/capture_window.o
	$(CXX) $^ -o $@ $(VK_LIBS) -lpthread
```

- [ ] **Step 3: Run to verify it fails** — `cd spatial-screens && make capture-window-test`
Expected: FAIL — `capture_window.cpp` undefined reference to `capture_create_window`.

- [ ] **Step 4: Implement `capture_window.cpp`** — mirror `XShmBackend`, self-contained SHM triple buffer. Skeleton (fill the SHM create/destroy/grab bodies by copying `capture_xshm.cpp:114-165`'s `build()`/`destroy_images()`/`grab` shape, substituting the pixmap drawable):

```cpp
#include "capture.h"
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xcomposite.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

namespace {
class WindowBackend : public CaptureBackend {
public:
    WindowBackend(Window win, int hz)
        : win_(win), interval_(1.0 / (hz > 0 ? hz : 30)) {}
    ~WindowBackend() override { stop(); }

    bool start() override {
        dpy_ = XOpenDisplay(nullptr);
        if (!dpy_) return false;
        int ev, er;
        if (!XShmQueryExtension(dpy_) || !XCompositeQueryExtension(dpy_, &ev, &er)) {
            XCloseDisplay(dpy_); dpy_ = nullptr; return false;
        }
        XCompositeRedirectWindow(dpy_, win_, CompositeRedirectAutomatic);
        XSelectInput(dpy_, win_, StructureNotifyMask);
        if (!query_size() || !build()) return false;
        run_ = true;
        thread_ = std::thread(&WindowBackend::grab_loop, this);
        return true;
    }

    bool latest_frame(CaptureFrame& out) override {
        std::lock_guard<std::mutex> lk(mtx_);
        if (front_ < 0) return false;
        reading_ = front_; front_ = -1;
        out.data = reinterpret_cast<const uint8_t*>(img_[reading_]->data);
        out.w = w_; out.h = h_;
        out.pitch = uint32_t(img_[reading_]->bytes_per_line);
        return true;
    }
    bool alive() const override { return alive_; }   // false once window destroyed
    const char* name() const override { return "window"; }
    void stop() override {
        if (thread_.joinable()) { run_ = false; thread_.join(); }
        destroy_images();
        if (pixmap_) { XFreePixmap(dpy_, pixmap_); pixmap_ = 0; }
        if (dpy_) { XCloseDisplay(dpy_); dpy_ = nullptr; }
    }

private:
    bool query_size() {
        XWindowAttributes a;
        if (!XGetWindowAttributes(dpy_, win_, &a)) return false;
        w_ = a.width; h_ = a.height;
        return w_ > 0 && h_ > 0;
    }
    void name_pixmap() {
        if (pixmap_) XFreePixmap(dpy_, pixmap_);
        pixmap_ = XCompositeNameWindowPixmap(dpy_, win_);
    }
    bool build() { /* copy capture_xshm.cpp build(): 3x XShmCreateImage(w_,h_,24,ZPixmap)
                      + shmget/shmat/XShmAttach/shmctl(IPC_RMID). */ return true; }
    void destroy_images() { /* copy capture_xshm.cpp destroy_images() */ }

    void drain_events() {
        while (XPending(dpy_)) {
            XEvent e; XNextEvent(dpy_, &e);
            if (e.type == DestroyNotify && e.xdestroywindow.window == win_) alive_ = false;
            else if (e.type == ConfigureNotify && e.xconfigure.window == win_) {
                if (e.xconfigure.width != w_ || e.xconfigure.height != h_) resized_ = true;
            }
        }
    }
    void rebuild_for_resize() {
        query_size(); destroy_images(); build(); name_pixmap();
        std::lock_guard<std::mutex> lk(mtx_); front_ = reading_ = -1;
    }
    void grab_loop() {
        name_pixmap();
        while (run_) {
            auto t0 = std::chrono::steady_clock::now();
            drain_events();
            if (!alive_) return;
            if (resized_) { resized_ = false; rebuild_for_resize(); }
            int b = 0;
            { std::lock_guard<std::mutex> lk(mtx_); while (b == front_ || b == reading_) b++; }
            // XShmGetImage from the PIXMAP (a Drawable), origin 0,0.
            if (!XShmGetImage(dpy_, pixmap_, img_[b], 0, 0, AllPlanes)) {
                std::this_thread::sleep_until(t0 + std::chrono::duration<double>(interval_));
                continue;   // transient (e.g. mid-resize); keep last frame
            }
            // Force alpha opaque: set byte 3 of every pixel to 0xFF.
            uint8_t* p = reinterpret_cast<uint8_t*>(img_[b]->data);
            for (int i = 3; i < img_[b]->bytes_per_line * h_; i += 4) p[i] = 0xFF;
            { std::lock_guard<std::mutex> lk(mtx_); front_ = b; }
            std::this_thread::sleep_until(t0 + std::chrono::duration<double>(interval_));
        }
    }

    Window win_; Pixmap pixmap_ = 0;
    Display* dpy_ = nullptr;
    double interval_;
    int w_ = 0, h_ = 0;
    XShmSegmentInfo shm_[3]{};
    XImage* img_[3] = {nullptr, nullptr, nullptr};
    std::mutex mtx_;
    std::thread thread_;
    std::atomic<bool> run_{false}, alive_{true}, resized_{false};
    int front_ = -1, reading_ = -1;
};
}  // namespace

std::unique_ptr<CaptureBackend> capture_create_window(Window win, int hz) {
    return std::make_unique<WindowBackend>(win, hz);
}
```
Copy the `build()` / `destroy_images()` bodies verbatim from `capture_xshm.cpp` (they are identical — SHM images sized `w_×h_`).

- [ ] **Step 5: Run the smoke test** — `cd spatial-screens && make capture-window-test && ./capture-window-test`
Expected: `ok` (or `SKIP ...` on a headless box — acceptable; must not FAIL).

- [ ] **Step 6: Verify the full app still links** — `cd spatial-screens && make`
Expected: `spatial-screens` builds and links (now with `-lXcomposite` + the new object).

- [ ] **Step 7: Commit**

```bash
git add spatial-screens/src/capture.h spatial-screens/src/capture_window.cpp spatial-screens/src/capture_window_test.cpp spatial-screens/Makefile
git commit -m "feat(floating-windows): WindowBackend (XComposite pixmap capture) + smoke test"
```

---

### Task 6a: Renderer — per-source texture/dset array (behavior-preserving)

**Files:**
- Modify: `spatial-screens/src/vk_renderer.h:18-58` (struct + API)
- Modify: `spatial-screens/src/vk_renderer.cpp` (descriptor pool/layout, texture init/upload, destroy)

**Interfaces:**
- Consumes: `kSourceSlots`, `kLabelSource` (`source_slots.h`).
- Produces:
  - `QuadDraw.source_index` (int, default 0).
  - `void vkr_set_source_size(VkRend& r, int idx, uint32_t w, uint32_t h, uint32_t pitch);`
  - `void vkr_upload_source(VkRend& r, int idx, const void* pixels, size_t bytes);`
  - `vkr_init_texture`/`vkr_upload` keep working as thin wrappers over `idx == 0`.

> **Read first:** `vk_renderer.cpp:220-240` (descriptor set layout), `326-345` (pool), `351-444` (texture create + descriptor write), `464-535` (upload + copy-to-image), `540-565` (draw/bind). This task generalizes scalar texture state to an array **without changing what's drawn** (only slot 0 is used until Task 7).

- [ ] **Step 1: Restructure `VkRend`** — in `vk_renderer.h`, replace the scalar texture block (`tex`, `tex_mem`, `tex_view`, `tex_w/h/pitch`, `staging`, `staging_mem`, `staging_ptr`, `tex_dirty`, and `dset`) with:

```cpp
#include "source_slots.h"
...
struct RSource {
    VkImage tex = VK_NULL_HANDLE;
    VkDeviceMemory tex_mem = VK_NULL_HANDLE;
    VkImageView tex_view = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0, pitch = 0;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    void* staging_ptr = nullptr;
    bool dirty = false;
    VkDescriptorSet dset = VK_NULL_HANDLE;
};
// index 0 = monitor · 1..kSourceSlots-1 = window · kLabelSource = label
RSource src[kSourceSlots + 1];
```
Add `source_index` to `QuadDraw`:
```cpp
    int source_index = 0;
```
Add the new function declarations (and keep the old ones):
```cpp
void vkr_set_source_size(VkRend& r, int idx, uint32_t w, uint32_t h, uint32_t pitch);
void vkr_upload_source(VkRend& r, int idx, const void* pixels, size_t bytes);
```

- [ ] **Step 2: Generalize pool + layout + per-slot create** — in `vk_renderer.cpp`:
  - Pool `maxSets` and the `COMBINED_IMAGE_SAMPLER` `descriptorCount` → `kSourceSlots + 1`.
  - Turn the existing texture-create + descriptor-write (`vkr_init_texture` body) into a private `create_source(VkRend&, int idx, w, h, pitch)` that fills `r.src[idx]` (image `SAMPLED|TRANSFER_DST`, view, staging `HOST_VISIBLE` mapped, allocate + write that slot's `dset`). Add a private `destroy_source(VkRend&, int idx)`.
  - `vkr_set_source_size(r, idx, w, h, pitch)`: if the slot's dims differ (or it's uncreated), `vkr_wait_uploads(r); destroy_source(r, idx); create_source(r, idx, w, h, pitch);`.
  - `vkr_init_texture(r, w, h, pitch)` → `vkr_set_source_size(r, 0, w, h, pitch); return true;`.
  - `vkr_upload_source(r, idx, pixels, bytes)`: `memcpy(r.src[idx].staging_ptr, pixels, bytes); r.src[idx].dirty = true;`.
  - `vkr_upload(r, pixels, bytes)` → `vkr_upload_source(r, 0, pixels, bytes);`.
  - In `draw_impl`, the current `if (tex_dirty)` copy-to-image block becomes a loop over all created slots: for each `idx` with `dirty`, barrier + `vkCmdCopyBufferToImage` from `src[idx].staging` to `src[idx].tex` (`bufferRowLength = src[idx].pitch/4`), clear `dirty`.
  - `vkr_destroy` / `vkr_destroy_device`: `destroy_source` every created slot.
  - **Do not yet change the descriptor bind** — keep binding `r.src[0].dset` once before the loop, so behavior is unchanged.

- [ ] **Step 3: Build + smoke** — `cd spatial-screens && make vk-test && ./vk-test` (Ctrl+C after a few seconds).
Expected: builds; the windowed checkerboard renders exactly as before (monitor slot 0 path unchanged).

- [ ] **Step 4: Full app builds** — `cd spatial-screens && make`
Expected: links clean.

- [ ] **Step 5: Commit**

```bash
git add spatial-screens/src/vk_renderer.h spatial-screens/src/vk_renderer.cpp
git commit -m "feat(floating-windows): renderer per-source texture array (behavior-preserving)"
```

---

### Task 6b: Renderer — per-draw descriptor bind + source selection

**Files:**
- Modify: `spatial-screens/src/vk_renderer.cpp` (`record_quads` / bind site ~`548`)

**Interfaces:**
- Consumes: `RSource src[]`, `QuadDraw.source_index` (Task 6a).
- Produces: each quad samples `src[source_index].tex`; untextured quads bind slot 0 harmlessly.

- [ ] **Step 1: Move the descriptor bind into the quad loop** — in `record_quads`, before pushing each quad's constants, bind that quad's source dset (fall back to slot 0 if the requested slot has no view yet):

```cpp
    for (int i = 0; i < n; i++) {
        int si = draws[i].source_index;
        if (si < 0 || si > kSourceSlots || r.src[si].tex_view == VK_NULL_HANDLE) si = 0;
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r.playout,
                                0, 1, &r.src[si].dset, 0, nullptr);
        // ... existing push-constant + draw for quad i ...
    }
```
Remove the single pre-loop `vkCmdBindDescriptorSets(..., &r.dset, ...)` at ~`548-549` (its state is now per-quad). `record_quads` needs `VkRend& r` — pass it through (it is already a parameter).

- [ ] **Step 2: Build + smoke** — `cd spatial-screens && make vk-test && ./vk-test`
Expected: checkerboard still renders (all quads use slot 0 today → identical output).

- [ ] **Step 3: Full app builds** — `cd spatial-screens && make`
Expected: links clean.

- [ ] **Step 4: Commit**

```bash
git add spatial-screens/src/vk_renderer.cpp
git commit -m "feat(floating-windows): per-draw source descriptor bind"
```

---

### Task 7: main — window-backend list, per-frame pump, teardown

**Files:**
- Modify: `spatial-screens/src/main.cpp` (source state near the scene/`cap` setup; per-frame block near `1100-1125`)

**Interfaces:**
- Consumes: `SourceSlots`, `capture_create_window` (T5), `vkr_set_source_size`/`vkr_upload_source` (T6a), `scene_window_resize` (T2).
- Produces: file-scope state `SourceSlots g_slots;` (non-atomic — render-thread only), `std::unique_ptr<CaptureBackend> win_src[kSourceSlots];`, and a `pump_window_sources()` helper called each frame after the monitor upload. These are used by Tasks 8–10.

> **Read first:** `main.cpp:485-562` (scene + `cap` chain), `main.cpp:1095-1130` (per-frame capture → `vkr_upload`). Window sources live **alongside** `cap` (the monitor backend), indexed by slot.

- [ ] **Step 1: Add source state + pump** — near the scene/`cap` declarations, add:

```cpp
#include "source_slots.h"
...
SourceSlots slots;                                   // slot 0 = monitor (never alloc'd)
std::unique_ptr<CaptureBackend> win_src[kSourceSlots];  // [1..7] window backends
```
After the existing per-frame `cap->latest_frame(f)` + `vkr_upload(vk, ...)`, add a pump over window sources:

```cpp
    for (int i = 1; i < kSourceSlots; i++) {
        if (!win_src[i]) continue;
        if (!win_src[i]->alive()) {                  // window closed
            win_src[i]->stop(); win_src[i].reset(); slots.release(i);
            for (auto& s : scene) if (s.source_index == i) s.source_index = -1;
            continue;
        }
        CaptureFrame wf;
        if (!win_src[i]->latest_frame(wf)) continue;
        // Re-create the texture + follow size on any dim change.
        bool dims_changed = false;
        for (auto& s : scene)
            if (s.source_index == i && (s.src_w != wf.w || s.src_h != wf.h)) {
                scene_window_resize(s, wf.w, wf.h); dims_changed = true;
            }
        if (dims_changed) vkr_set_source_size(vk, i, wf.w, wf.h, wf.pitch);
        vkr_upload_source(vk, i, wf.data, size_t(wf.pitch) * wf.h);
    }
```
(If `scene` / `vk` have different local names, use them.)

- [ ] **Step 2: Build** — `cd spatial-screens && make`
Expected: links clean. (No window sources exist yet → pump is a no-op; behavior unchanged.)

- [ ] **Step 3: Manual smoke on hardware** *(defer to Task 14 hardware pass if glasses are not attached now)* — launch `./run.sh`; the existing monitor rack must render unchanged (the pump loop stays empty). No regression = pass.

- [ ] **Step 4: Commit**

```bash
git add spatial-screens/src/main.cpp
git commit -m "feat(floating-windows): window-source list + per-frame pump/teardown"
```

---

### Task 8: main — `Ctrl+Alt+W` grab-focused-window + spawn

**Files:**
- Modify: `spatial-screens/src/main.cpp:429` (`hot[]`), handler block after `760`, plus small X11 helpers.

**Interfaces:**
- Consumes: T7 state (`slots`, `win_src`), `capture_create_window` (T5), scene/`active_screen`, `world_to_rack_frame` + head pose (existing), `scene_window_resize` (T2).
- Produces: an active-or-spawned window screen bound to a fresh slot.

> **Read first:** `main.cpp:425-441` (hotkey grabs), `721-760` (handler switch), `833-914` (how the active screen's `pose_ori/pose_pos` are written via `world_to_rack_frame` from a world pose + head pose — reuse this exact pattern to face a spawned screen).

- [ ] **Step 1: Register the key** — in `hot[]` (`main.cpp:429`) add `XK_w`:
```cpp
    KeySym hot[] = { XK_r, XK_bracketleft, XK_bracketright, XK_minus, XK_equal, XK_q, XK_w };
```

- [ ] **Step 2: Add X11 helpers** (file-scope, near the top helpers):
```cpp
static Window read_active_window(Display* dpy, Window root) {
    Atom prop = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", True), type; int fmt;
    unsigned long n, after; unsigned char* data = nullptr;
    if (prop == None) return 0;
    if (XGetWindowProperty(dpy, root, prop, 0, 1, False, AnyPropertyType,
                           &type, &fmt, &n, &after, &data) != Success || !data) return 0;
    Window w = n ? *reinterpret_cast<Window*>(data) : 0;
    XFree(data);
    return w;
}
static bool window_dims(Display* dpy, Window w, int& ow, int& oh) {
    XWindowAttributes a;
    if (!w || !XGetWindowAttributes(dpy, w, &a)) return false;
    ow = a.width; oh = a.height;
    return a.map_state == IsViewable && ow > 0 && oh > 0;
}
```

- [ ] **Step 3: Add the handler branch** — after the `XK_equal` branch (`main.cpp:760`):

```cpp
        else if (ks == XK_w) {
            Window aw = read_active_window(dpy, root);
            int ww, wh;
            if (!aw || aw == our_output_window || !window_dims(dpy, aw, ww, wh)) {
                fprintf(stderr, "grab-window: no valid focused window\n");
            } else {
                int slot = slots.alloc();
                if (slot < 0) { fprintf(stderr, "grab-window: slot cap reached\n"); }
                else {
                    auto b = capture_create_window(aw, opt.capture_hz);
                    if (!b->start()) { slots.release(slot); fprintf(stderr, "grab-window: start failed\n"); }
                    else {
                        win_src[slot] = std::move(b);
                        int target = active_screen;
                        if (target < 0) {              // spawn: revive a placeholder or append
                            for (size_t i = 0; i < scene.size(); i++)
                                if (scene[i].source_index < 0 && scene[i].cfg.source == "window") { target = int(i); break; }
                            if (target < 0) { scene.push_back(ScreenInst{}); target = int(scene.size()) - 1; }
                            scene[target].cfg.source = "window";
                            // Face it: place default distance along head-forward, look back at the user.
                            Vec3 fwd = qrot(head_q, {0, 0, -1});
                            Vec3 wp = { head_p.x + fwd.x * opt.distance,
                                        head_p.y + fwd.y * opt.distance,
                                        head_p.z + fwd.z * opt.distance };
                            world_to_rack_frame(rack_q, rack_p, head_q, wp,
                                                scene[target].cfg.pose_ori, scene[target].cfg.pose_pos);
                            scene[target].cfg.has_pose_override = true;
                            scene[target].cfg.size = opt.size;
                            active_screen = target;
                        }
                        scene[target].source_index = slot;
                        scene[target].uv[0] = 0; scene[target].uv[1] = 0;
                        scene[target].uv[2] = 1; scene[target].uv[3] = 1;
                        scene[target].src_w = scene[target].src_h = 0;   // pump binds real dims
                    }
                }
            }
        }
```
(Use the file's actual names for `dpy`, `root`, `opt`, `head_q/head_p`, `rack_q/rack_p`, `active_screen`, `scene`, and the glasses window handle for `our_output_window` — in direct-display mode there is no normal window, so `our_output_window` may be `0`/absent; drop that check if there is no such handle.)

- [ ] **Step 4: Build** — `cd spatial-screens && make`
Expected: links clean.

- [ ] **Step 5: Manual/hardware smoke** *(Task 14)* — focus a window, `Ctrl+Alt+W`: with a screen selected it shows that window; with none selected a new faced panel spawns showing the window.

- [ ] **Step 6: Commit**

```bash
git add spatial-screens/src/main.cpp
git commit -m "feat(floating-windows): Ctrl+Alt+W grab-focused-window + append-only spawn"
```

---

### Task 9: main — config-declared window screens resolved at startup

**Files:**
- Modify: `spatial-screens/src/main.cpp` (after `scene_build`, before the render loop)

**Interfaces:**
- Consumes: `ScreenInst.cfg.source/window_match` (T1/T2), `capture_create_window` (T5), `slots`/`win_src` (T7).
- Produces: window-source screens bound at launch; misses stay placeholders.

- [ ] **Step 1: Add a resolver + call it once after `scene_build`**:

```cpp
static Window find_window_by_match(Display* dpy, Window root, const std::string& needle) {
    if (needle == ":active") return read_active_window(dpy, root);
    Window rr, parent, *kids = nullptr; unsigned nk = 0;
    if (!XQueryTree(dpy, root, &rr, &parent, &kids, &nk)) return 0;
    Window found = 0;
    for (unsigned i = 0; i < nk && !found; i++) {
        XClassHint ch{};
        if (XGetClassHint(dpy, kids[i], &ch)) {
            std::string cls = ch.res_class ? ch.res_class : "";
            std::string nm = ch.res_name ? ch.res_name : "";
            if (ch.res_class) XFree(ch.res_class);
            if (ch.res_name) XFree(ch.res_name);
            if (cls.find(needle) != std::string::npos || nm.find(needle) != std::string::npos)
                found = kids[i];
        }
    }
    if (kids) XFree(kids);
    return found;
}
```
After `scene = scene_build(...)`:
```cpp
    for (auto& s : scene) {
        if (s.cfg.source != "window" || s.source_index >= 0) continue;
        Window w = find_window_by_match(dpy, root, s.cfg.window_match);
        int ww, wh, slot;
        if (!w || !window_dims(dpy, w, ww, wh) || (slot = slots.alloc()) < 0) {
            fprintf(stderr, "scene: window-match '%s' unresolved — placeholder\n", s.cfg.window_match.c_str());
            continue;
        }
        auto b = capture_create_window(w, opt.capture_hz);
        if (!b->start()) { slots.release(slot); continue; }
        win_src[slot] = std::move(b);
        s.source_index = slot;
        s.uv[0]=0; s.uv[1]=0; s.uv[2]=1; s.uv[3]=1; s.src_w=s.src_h=0;
    }
```

- [ ] **Step 2: Build** — `cd spatial-screens && make`
Expected: links clean.

- [ ] **Step 3: Commit**

```bash
git add spatial-screens/src/main.cpp
git commit -m "feat(floating-windows): resolve config window-match screens at startup"
```

---

### Task 10: main — active-screen resolution label quad

**Files:**
- Modify: `spatial-screens/src/main.cpp` (label state + the draw-list assembly ~`1150-1345`)

**Interfaces:**
- Consumes: `text_raster_render` (T4), `vkr_set_source_size`/`vkr_upload_source` at `kLabelSource` (T6a), `ScreenInst.src_w/src_h`, `active_screen`, the panel-frame basis used by the border bars (`main.cpp:1220-1235`).
- Produces: one co-planar textured quad below the active window panel showing `W×H`.

> **Read first:** `main.cpp:1150-1260` — how per-screen quads + the active-screen border bars are built (screen center, right/up basis, half-extents). The label reuses that basis.

- [ ] **Step 1: Track + (re)rasterize the label** — near the draw-list assembly, before emitting quads:

```cpp
    static int lbl_screen = -1, lbl_w = -1, lbl_h = -1;   // last rasterized
    bool show_label = active_screen >= 0 &&
                      scene[active_screen].source_index >= 1 &&
                      scene[active_screen].src_w > 0;
    if (show_label &&
        (lbl_screen != active_screen || lbl_w != scene[active_screen].src_w ||
         lbl_h != scene[active_screen].src_h)) {
        char buf[32];
        snprintf(buf, sizeof buf, "%dx%d", scene[active_screen].src_w, scene[active_screen].src_h);
        RasterBuf rb = text_raster_render(buf, 0x00FFFFFF, 0x00202020, 2);
        vkr_set_source_size(vk, kLabelSource, rb.w, rb.h, rb.w * 4);
        vkr_upload_source(vk, kLabelSource, rb.data.data(), rb.data.size());
        lbl_screen = active_screen; lbl_w = scene[active_screen].src_w; lbl_h = scene[active_screen].src_h;
        g_lbl_aspect = float(rb.w) / float(rb.h);   // file-scope float, used below
    }
```

- [ ] **Step 2: Emit the label quad** — inside the per-eye loop, after the active screen's border bars, when `show_label` and the current screen is `active_screen`, append one quad co-planar with the panel, just below its bottom edge (reuse the same `center`, `right`, `up`, `half_h` the border bars computed):

```cpp
        if (show_label && idx == active_screen) {
            const float lh = 0.03f;                 // label height in meters
            const float lw = lh * g_lbl_aspect;
            Vec3 lc = { center.x - up.x * (half_h + lh),
                        center.y - up.y * (half_h + lh),
                        center.z - up.z * (half_h + lh) };
            QuadDraw& q = dl[nd++];
            // set q.mvp from (lc, right, up, lw, lh) exactly like a screen quad;
            // q.uv = {0,0,1,1}; q.textured = true; q.source_index = kLabelSource;
        }
```
Build `q.mvp`/`q.rect` with the same helper the screen/border quads use (mirror the border-bar quad construction, substituting `lc`, `lw`, `lh`).

- [ ] **Step 3: Build** — `cd spatial-screens && make`
Expected: links clean.

- [ ] **Step 4: Hardware smoke** *(Task 14)* — select a window screen; its `W×H` shows just below it and updates when the source window is resized.

- [ ] **Step 5: Commit**

```bash
git add spatial-screens/src/main.cpp
git commit -m "feat(floating-windows): active-screen native-resolution label"
```

---

### Task 11: main — persistent gray/white frame on all floating windows

**Files:**
- Modify: `spatial-screens/src/main.cpp:108` (constants), `1153-1155` (draw-list cap), `1220-1235` (border-bar pass)

**Interfaces:**
- Consumes: the existing 4-bar border geometry; `ScreenInst.source_index`; `active_screen`.
- Produces: every floating-window screen (`source_index != 0`) draws a frame — green when active, gray/white otherwise.

> **Read first:** `main.cpp:1220-1235` — the 4-bar green border currently emitted only for `active_screen`. Generalize the gate + color.

- [ ] **Step 1: Add constants** — near `SELECT_BORDER_M` (`main.cpp:108`):
```cpp
static constexpr float WINDOW_BORDER_M = 0.0022f;    // floating-window frame (hardware-tunable)
static constexpr float WIN_BORDER_RGB[3] = {0.85f, 0.85f, 0.85f};  // gray/white
```

- [ ] **Step 2: Raise the draw-list cap** — at `main.cpp:1155` bump `QuadDraw draws[2][72]` to a size that covers all screens' quads + 4 bars × every floating-window screen + the label + dots. Use:
```cpp
    QuadDraw draws[2][112];
```
(Update the accompanying capacity comment at `main.cpp:1153`.)

- [ ] **Step 3: Generalize the border pass** — where the 4 border bars are emitted (currently gated on the active screen only), emit a frame for **every** screen with `source_index != 0`, choosing color by selection:
```cpp
        bool is_active = (idx == active_screen);
        bool is_window = (scene[idx].source_index != 0);
        if (is_active || is_window) {
            const float b = is_active ? SELECT_BORDER_M : WINDOW_BORDER_M;
            float br = is_active ? 0.f : WIN_BORDER_RGB[0];
            float bg = is_active ? 1.f : WIN_BORDER_RGB[1];
            float bb = is_active ? 0.f : WIN_BORDER_RGB[2];
            // ... existing 4-bar emission, using b for thickness and (br,bg,bb) for color ...
        }
```
(Keep the exact 4-bar geometry already there; only the gate, thickness `b`, and color `(br,bg,bb)` change. Monitor screens keep today's behavior: framed only when active, since `is_window` is false.)

- [ ] **Step 4: Build** — `cd spatial-screens && make`
Expected: links clean.

- [ ] **Step 5: Hardware smoke** *(Task 14)* — floating windows show a thin gray/white frame that turns green while selected; monitor rack screens are unframed unless active.

- [ ] **Step 6: Commit**

```bash
git add spatial-screens/src/main.cpp
git commit -m "feat(floating-windows): persistent gray/white frame per floating window"
```

---

### Task 12: Telemetry — window-source count

**Files:**
- Modify: `spatial-screens/src/telemetry.{h,cpp}`, `spatial-screens/src/main.cpp` (populate)

**Interfaces:**
- Consumes: `win_src[]` (T7).
- Produces: a `window_sources` field in the telemetry JSON payload.

- [ ] **Step 1: Add the field** — extend the telemetry snapshot struct/JSON in `telemetry.h`/`telemetry.cpp` with an `int window_sources = 0;` (mirror an existing int field like the source/fps count).

- [ ] **Step 2: Populate it** — where main assembles the telemetry snapshot each tick, set it:
```cpp
    int wc = 0; for (int i = 1; i < kSourceSlots; i++) if (win_src[i]) wc++;
    snap.window_sources = wc;
```

- [ ] **Step 3: Build** — `cd spatial-screens && make`
Expected: links clean.

- [ ] **Step 4: Commit**

```bash
git add spatial-screens/src/telemetry.h spatial-screens/src/telemetry.cpp spatial-screens/src/main.cpp
git commit -m "feat(floating-windows): telemetry window-source count"
```

---

### Task 13: Full build + all unit tests green

- [ ] **Step 1: Clean build** — `cd spatial-screens && make clean && make`
Expected: `spatial-screens` links (with `-lXcomposite`).

- [ ] **Step 2: Run every pure/unit test** — `cd spatial-screens && make test && make text-raster-test && ./text-raster-test && make capture-window-test && ./capture-window-test`
Expected: `stereo-math-test`, `gesture-parse-test`, `gesture-manip-test`, `text-raster-test` all pass; `capture-window-test` prints `ok` or `SKIP ...` (never `FAIL`).

- [ ] **Step 3: Commit any fixups**

```bash
git add -A && git commit -m "chore(floating-windows): green build + tests"
```

---

### Task 14: Hardware pass (on user "go", glasses attached)

> Run per `docs/branches/feat-floating-window-screens.md`. Stop any SDK-holding session first; launch via `./run.sh`. Record PASS/FAIL per item in the branch doc.

- [ ] `Ctrl+Alt+W` with a screen selected binds the focused window onto it (own texture, full-frame).
- [ ] `Ctrl+Alt+W` with nothing selected **spawns** a new panel in front, **faced**, showing the window.
- [ ] Move / scale / face the window screen via the existing gestures (selection + head-anchored).
- [ ] Resize the source app → panel follows **proportionally at the same aspect**; texture stays crisp (native res).
- [ ] Every floating window shows a thin **gray/white frame**; it turns **green while selected**; monitor screens unframed.
- [ ] The active window screen shows its native **`W×H`** just outside the border; it updates on resize / de-select.
- [ ] Close the source window → clean **placeholder** revert, **no crash**; grabbing again reuses the freed slot.
- [ ] A config `screen.N.source = window` / `window-match = <substr>` screen resolves at launch.
- [ ] Telemetry shows the live window-source count on the dashboard.
- [ ] Both-eyes/SBS: window screens render correctly in stereo.

- [ ] **On all-pass:** update the branch doc + roadmap, then use `superpowers:finishing-a-development-branch` to merge `feat/floating-window-screens` into `main` (FF).

## Self-Review (author checklist — completed)

- **Spec coverage:** WindowBackend §1→T5; per-source renderer §2→T6a/T6b; scene source_index §3→T2; grab+spawn §4→T7/T8; config §5→T1/T9; label §6→T4/T10; frame §7→T11; sizing/definition→T2 + T6a/T7; telemetry→T12; Makefile/`-lXcomposite`→T5; hardware→T14. All covered.
- **Placeholder scan:** pure tasks (1–4) carry complete code + tests; native tasks carry exact insertion points, new code, and read-first pointers with build/hardware gates (the honest gate for non-unit-testable Vulkan/X11/main glue in this repo).
- **Type consistency:** `source_index`, `src_w/src_h`, `kSourceSlots`/`kLabelSource`, `SourceSlots::alloc/release`, `scene_window_resize`, `vkr_set_source_size`/`vkr_upload_source`, `capture_create_window`, `RasterBuf`/`text_raster_render` are spelled identically across producing and consuming tasks.
