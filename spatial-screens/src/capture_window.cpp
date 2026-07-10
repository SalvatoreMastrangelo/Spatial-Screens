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
//
// Resize differs from XShmBackend on purpose. XShmBackend parks its grab
// thread and frees all three segments — but that would free the segment the
// render thread is still reading (capture.h: the latest_frame() pointer is
// valid until the render thread's NEXT latest_frame()/stop()). Instead we
// migrate slots LAZILY, one at a time, entirely on the grab thread: on a
// ConfigureNotify size change we refresh w_/h_ and re-name the pixmap, then
// each grab iteration rebuilds ONLY the write slot (already chosen as
// neither front_ nor reading_) if its size is stale. The reader-held slot
// is never touched by the grab thread, so its pointer stays valid; slots
// migrate to the new size as they rotate through being the write target.
// latest_frame() reports dims read off the handed-out image itself, so the
// dims can never disagree with the buffer.
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
        // Dims come straight off the handed-out image, never from shared
        // w_/h_: this guarantees the dims always match the buffer, even if
        // the grab thread has since migrated other slots to a new size.
        out.data = reinterpret_cast<const uint8_t*>(img_[reading_]->data);
        out.w = img_[reading_]->width;
        out.h = img_[reading_]->height;
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

    // Build ONE SHM slot at w x h. Same setup as XShmBackend::build(), but
    // per-slot so a resize can migrate slots individually (see class note).
    bool build_slot(int i, int w, int h) {
        img_[i] = XShmCreateImage(dpy_, DefaultVisual(dpy_, DefaultScreen(dpy_)), 24,
                                  ZPixmap, nullptr, &shm_[i], w, h);
        if (!img_[i]) return false;
        shm_[i].shmid = shmget(IPC_PRIVATE,
                               size_t(img_[i]->bytes_per_line) * img_[i]->height,
                               IPC_CREAT | 0600);
        if (shm_[i].shmid < 0) { XDestroyImage(img_[i]); img_[i] = nullptr; return false; }
        shm_[i].shmaddr = img_[i]->data = (char*)shmat(shm_[i].shmid, nullptr, 0);
        if (shm_[i].shmaddr == (char*)-1) {
            shmctl(shm_[i].shmid, IPC_RMID, nullptr);
            XDestroyImage(img_[i]); img_[i] = nullptr;
            return false;
        }
        shm_[i].readOnly = False;
        XShmAttach(dpy_, &shm_[i]);
        shmctl(shm_[i].shmid, IPC_RMID, nullptr);  // auto-free on detach
        return true;
    }

    void destroy_slot(int i) {
        if (!img_[i]) return;
        XShmDetach(dpy_, &shm_[i]);
        XDestroyImage(img_[i]);
        shmdt(shm_[i].shmaddr);
        img_[i] = nullptr;
    }

    // Initial triple-buffer build, sized w_ x h_.
    bool build() {
        for (int i = 0; i < 3; i++)
            if (!build_slot(i, w_, h_)) return false;
        return true;
    }

    void destroy_images() {
        for (int i = 0; i < 3; i++) destroy_slot(i);
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

    void grab_loop() {
        name_pixmap();
        while (run_) {
            auto t0 = std::chrono::steady_clock::now();
            drain_events();
            if (!alive_) return;
            if (resized_) {
                resized_ = false;
                // Refresh the target size and re-name the pixmap (the old
                // named pixmap does not track the window's new size). Slots
                // are NOT freed here — they migrate lazily below, so the
                // reader-held slot is never disturbed.
                query_size();
                name_pixmap();
            }
            int b = 0;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                while (b == front_ || b == reading_) b++;  // 3 slots, <=2 busy
            }
            // Slot b is neither front_ nor reading_, so no thread holds it:
            // safe to (re)build to the current size. front_ cannot become b
            // until we publish it below, so the reader cannot start reading
            // b during this window.
            if (!img_[b] || img_[b]->width != w_ || img_[b]->height != h_) {
                destroy_slot(b);
                if (!build_slot(b, w_, h_)) { alive_ = false; return; }
            }
            // XShmGetImage from the PIXMAP (a Drawable), origin 0,0.
            if (!XShmGetImage(dpy_, pixmap_, img_[b], 0, 0, AllPlanes)) {
                std::this_thread::sleep_until(t0 + std::chrono::duration<double>(interval_));
                continue;   // transient (e.g. mid-resize); keep last frame
            }
            // Force alpha opaque: set byte 3 of every pixel to 0xFF. Bound by
            // this slot's own extents, never shared w_/h_.
            uint8_t* p = reinterpret_cast<uint8_t*>(img_[b]->data);
            const int nbytes = img_[b]->bytes_per_line * img_[b]->height;
            for (int i = 3; i < nbytes; i += 4) p[i] = 0xFF;
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
    int w_ = 0, h_ = 0;        // target size; grab-thread-owned after start()
    XShmSegmentInfo shm_[3]{};
    XImage* img_[3] = {nullptr, nullptr, nullptr};
    std::mutex mtx_;           // guards front_, reading_ (slot ownership)
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
