#include "capture.h"
#include "cursor_overlay.h"

#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace {

// XShmGetImage of a large framebuffer blocks for tens of ms (~40 ms at
// 3840x2400, measured on hardware 2026-07-06) — far over a 90 Hz frame
// budget, so grabs run on their own thread with their own Display
// connection (two connections, each used by exactly one thread: no
// XInitThreads needed). Triple buffer: one slot being written, one
// published (front), one held by the reader — the pointer handed out by
// latest_frame() stays valid until the NEXT latest_frame() call.
class XShmBackend : public CaptureBackend {
public:
    XShmBackend(const OutputRect& source, int hz)
        : src_(source), interval_(1.0 / (hz > 0 ? hz : 30)) {}
    ~XShmBackend() override { stop(); }

    bool start() override {
        dpy_ = XOpenDisplay(nullptr);
        if (!dpy_) {
            fprintf(stderr, "capture(xshm): cannot open display\n");
            return false;
        }
        if (!XShmQueryExtension(dpy_)) {
            fprintf(stderr, "capture(xshm): MIT-SHM not available\n");
            XCloseDisplay(dpy_);
            dpy_ = nullptr;
            return false;
        }
        int ev, err;
        have_xfixes_ = XFixesQueryExtension(dpy_, &ev, &err);
        if (!build()) return false;
        run_ = true;
        thread_ = std::thread(&XShmBackend::grab_loop, this);
        return true;
    }

    bool latest_frame(CaptureFrame& out) override {
        std::lock_guard<std::mutex> lk(mtx_);
        if (front_ < 0) return false;
        reading_ = front_;
        front_ = -1;
        out.data = reinterpret_cast<const uint8_t*>(img_[reading_]->data);
        out.w = src_.w; out.h = src_.h;
        out.pitch = uint32_t(img_[reading_]->bytes_per_line);
        return true;
    }

    bool alive() const override { return alive_; }
    const char* name() const override { return "xshm"; }
    bool composites_cursor() const override { return have_xfixes_; }

    void stop() override {
        if (thread_.joinable()) {
            run_ = false;
            thread_.join();
        }
        destroy_images();
        if (dpy_) {
            XCloseDisplay(dpy_);
            dpy_ = nullptr;
        }
    }

    void on_outputs_changed(const std::vector<OutputRect>& outs) override {
        for (const auto& o : outs) {
            if (o.name != src_.name) continue;
            if (o.w == src_.w && o.h == src_.h) {
                std::lock_guard<std::mutex> lk(mtx_);
                src_ = o;  // moved only: new grab origin
                return;
            }
            // Resize: rebuild the segments with the grabber parked. The
            // reader's held frame pointer dies with the old segments —
            // acceptable, the render loop has already uploaded it (this
            // call and latest_frame() share the render thread).
            if (thread_.joinable()) {
                run_ = false;
                thread_.join();
            }
            destroy_images();
            {
                std::lock_guard<std::mutex> lk(mtx_);
                src_ = o;
                front_ = reading_ = -1;
            }
            if (!build()) {
                fprintf(stderr, "capture(xshm): rebuild failed after source resize\n");
                alive_ = false;
                return;
            }
            run_ = true;
            thread_ = std::thread(&XShmBackend::grab_loop, this);
            return;
        }
        fprintf(stderr, "capture(xshm): source %s disappeared\n", src_.name.c_str());
        alive_ = false;
    }

private:
    bool build() {
        for (int i = 0; i < 3; i++) {
            img_[i] = XShmCreateImage(dpy_, DefaultVisual(dpy_, DefaultScreen(dpy_)), 24,
                                      ZPixmap, nullptr, &shm_[i], src_.w, src_.h);
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

    void destroy_images() {
        for (int i = 0; i < 3; i++) {
            if (!img_[i]) continue;
            XShmDetach(dpy_, &shm_[i]);
            XDestroyImage(img_[i]);
            shmdt(shm_[i].shmaddr);
            img_[i] = nullptr;
        }
    }

    void grab_loop() {
        while (run_) {
            auto t0 = std::chrono::steady_clock::now();
            int b = 0, x, y;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                while (b == front_ || b == reading_) b++;  // 3 slots, <=2 busy
                x = src_.x;
                y = src_.y;
            }
            if (!XShmGetImage(dpy_, DefaultRootWindow(dpy_), img_[b], x, y, AllPlanes)) {
                fprintf(stderr, "capture(xshm): grab failed\n");
                alive_ = false;
                return;
            }
            if (have_xfixes_)
                composite_cursor(dpy_, reinterpret_cast<uint8_t*>(img_[b]->data),
                                 src_.w, src_.h, uint32_t(img_[b]->bytes_per_line),
                                 x, y, nullptr);
            {
                std::lock_guard<std::mutex> lk(mtx_);
                front_ = b;
            }
            std::this_thread::sleep_until(t0 + std::chrono::duration<double>(interval_));
        }
    }

    Display* dpy_ = nullptr;   // grab-thread connection, owned
    OutputRect src_;
    double interval_;
    XShmSegmentInfo shm_[3]{};
    XImage* img_[3] = {nullptr, nullptr, nullptr};
    std::mutex mtx_;           // guards src_, front_, reading_
    std::thread thread_;
    std::atomic<bool> run_{false};
    std::atomic<bool> alive_{true};
    bool have_xfixes_ = false;
    int front_ = -1, reading_ = -1;
};

}  // namespace

std::unique_ptr<CaptureBackend> capture_create_xshm(const OutputRect& source, int hz) {
    return std::make_unique<XShmBackend>(source, hz);
}
