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
