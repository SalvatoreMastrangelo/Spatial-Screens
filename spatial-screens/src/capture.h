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
    // True when the backend already blends the pointer into its frames
    // (off the render thread) — the render loop must then skip its own
    // XFixes overlay, whose round trip stalls behind server-side grabs.
    virtual bool composites_cursor() const { return false; }
    virtual bool alive() const = 0;
    virtual const char* name() const = 0;
    virtual void stop() = 0;
    // X11 layout changed (RandR event). Backends that grab by screen rect
    // re-resolve their source; others ignore it.
    virtual void on_outputs_changed(const std::vector<OutputRect>&) {}
};

std::unique_ptr<CaptureBackend> capture_create_xshm(const OutputRect& source, int hz);
std::unique_ptr<CaptureBackend> capture_create_window(Window win, int hz);
std::unique_ptr<CaptureBackend> capture_create_test();
