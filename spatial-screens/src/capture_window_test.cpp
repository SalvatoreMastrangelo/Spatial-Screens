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

    // Resize regression: growing the window mid-capture must not crash and
    // latest_frame() must report the NEW dims (per-slot lazy rebuild, with
    // the reader-held old slot left untouched). Guards the resize UAF/OOB.
    XResizeWindow(d, w, 320, 240); XSync(d, False);
    CaptureFrame g{};
    bool resized_ok = false;
    for (int i = 0; i < 200; i++) {
        if (cap->latest_frame(g) && g.w == 320 && g.h == 240) { resized_ok = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!resized_ok) { printf("FAIL resize dims %dx%d\n", g.w, g.h); rc = 1; }

    cap->stop();
    XDestroyWindow(d, w); XCloseDisplay(d);
    if (!rc) printf("ok\n");
    return rc;
}
