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
