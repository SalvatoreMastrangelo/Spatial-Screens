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

// Mirrors XShmBackend (capture_xshm.cpp): own Display, own grab thread,
// triple SHM buffer, latest_frame() handing out img_[reading_]->data. The
// grab target here is a redirected window's NAMED PIXMAP (re-named on
// resize) instead of a screen rect, and liveness/resize are tracked via
// StructureNotify events on the window rather than RandR.
class WindowBackend : public CaptureBackend {
public:
    WindowBackend(Window win, int hz)
        : win_(win), interval_(1.0 / (hz > 0 ? hz : 30)) {}
    ~WindowBackend() override { stop(); }

    bool start() override {
        dpy_ = XOpenDisplay(nullptr);
        if (!dpy_) {
            fprintf(stderr, "capture(window): cannot open display\n");
            return false;
        }
        int ev, er;
        if (!XShmQueryExtension(dpy_) || !XCompositeQueryExtension(dpy_, &ev, &er)) {
            fprintf(stderr, "capture(window): MIT-SHM or Composite not available\n");
            XCloseDisplay(dpy_);
            dpy_ = nullptr;
            return false;
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
        reading_ = front_;
        front_ = -1;
        out.data = reinterpret_cast<const uint8_t*>(img_[reading_]->data);
        out.w = w_; out.h = h_;
        out.pitch = uint32_t(img_[reading_]->bytes_per_line);
        return true;
    }

    bool alive() const override { return alive_; }   // false once window destroyed
    const char* name() const override { return "window"; }

    void stop() override {
        if (thread_.joinable()) {
            run_ = false;
            thread_.join();
        }
        destroy_images();
        if (pixmap_) {
            XFreePixmap(dpy_, pixmap_);
            pixmap_ = 0;
        }
        if (dpy_) {
            XCloseDisplay(dpy_);
            dpy_ = nullptr;
        }
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

    // Copied from XShmBackend::build() (capture_xshm.cpp) — identical
    // triple-buffer SHM image setup, sized w_ x h_ instead of src_.w/h.
    bool build() {
        for (int i = 0; i < 3; i++) {
            img_[i] = XShmCreateImage(dpy_, DefaultVisual(dpy_, DefaultScreen(dpy_)), 24,
                                      ZPixmap, nullptr, &shm_[i], w_, h_);
            if (!img_[i]) return false;
            shm_[i].shmid = shmget(IPC_PRIVATE,
                                   size_t(img_[i]->bytes_per_line) * img_[i]->height,
                                   IPC_CREAT | 0600);
            shm_[i].shmaddr = img_[i]->data = (char*)shmat(shm_[i].shmid, nullptr, 0);
            shm_[i].readOnly = False;
            XShmAttach(dpy_, &shm_[i]);
            shmctl(shm_[i].shmid, IPC_RMID, nullptr);  // auto-free on detach
        }
        return true;
    }

    // Copied from XShmBackend::destroy_images() (capture_xshm.cpp) verbatim.
    void destroy_images() {
        for (int i = 0; i < 3; i++) {
            if (!img_[i]) continue;
            XShmDetach(dpy_, &shm_[i]);
            XDestroyImage(img_[i]);
            shmdt(shm_[i].shmaddr);
            img_[i] = nullptr;
        }
    }

    void drain_events() {
        while (XPending(dpy_)) {
            XEvent e;
            XNextEvent(dpy_, &e);
            if (e.type == DestroyNotify && e.xdestroywindow.window == win_) {
                alive_ = false;
            } else if (e.type == ConfigureNotify && e.xconfigure.window == win_) {
                if (e.xconfigure.width != w_ || e.xconfigure.height != h_) resized_ = true;
            }
        }
    }

    void rebuild_for_resize() {
        query_size();
        destroy_images();
        build();
        name_pixmap();
        std::lock_guard<std::mutex> lk(mtx_);
        front_ = reading_ = -1;
    }

    void grab_loop() {
        name_pixmap();
        while (run_) {
            auto t0 = std::chrono::steady_clock::now();
            drain_events();
            if (!alive_) return;
            if (resized_) {
                resized_ = false;
                rebuild_for_resize();
            }
            int b = 0;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                while (b == front_ || b == reading_) b++;  // 3 slots, <=2 busy
            }
            // XShmGetImage from the PIXMAP (a Drawable), origin 0,0.
            if (!XShmGetImage(dpy_, pixmap_, img_[b], 0, 0, AllPlanes)) {
                std::this_thread::sleep_until(t0 + std::chrono::duration<double>(interval_));
                continue;   // transient (e.g. mid-resize); keep last frame
            }
            // Force alpha opaque: set byte 3 of every pixel to 0xFF.
            uint8_t* p = reinterpret_cast<uint8_t*>(img_[b]->data);
            for (int i = 3; i < img_[b]->bytes_per_line * h_; i += 4) p[i] = 0xFF;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                front_ = b;
            }
            std::this_thread::sleep_until(t0 + std::chrono::duration<double>(interval_));
        }
    }

    Window win_;
    Pixmap pixmap_ = 0;
    Display* dpy_ = nullptr;   // grab-thread connection, owned
    double interval_;
    int w_ = 0, h_ = 0;
    XShmSegmentInfo shm_[3]{};
    XImage* img_[3] = {nullptr, nullptr, nullptr};
    std::mutex mtx_;           // guards front_, reading_
    std::thread thread_;
    std::atomic<bool> run_{false};
    std::atomic<bool> alive_{true};
    std::atomic<bool> resized_{false};
    int front_ = -1, reading_ = -1;
};

}  // namespace

std::unique_ptr<CaptureBackend> capture_create_window(Window win, int hz) {
    return std::make_unique<WindowBackend>(win, hz);
}
