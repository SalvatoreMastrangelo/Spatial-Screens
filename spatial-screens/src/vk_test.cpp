// vk-test — exercises the Vulkan presentation stack without the VITURE SDK.
//
//   ./vk-test [--direct] [--monitor NAME] [--seconds N]
//
// Renders an animated checkerboard quad + border. Default: EWMH-fullscreen
// window on the auto-detected glasses output (use --monitor eDP-1 to test on
// the laptop panel with no glasses). --direct takes the output away from the
// desktop via non-desktop=1 + VK_EXT_acquire_xlib_display and restores it on
// exit (Ctrl+C included).
#include "vk_renderer.h"
#include "vk_surface.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running = false; }

int main(int argc, char** argv) {
    bool direct = false;
    std::string monitor;
    double seconds = 8.0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--direct")) direct = true;
        else if (!strcmp(argv[i], "--monitor") && i + 1 < argc) monitor = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else { printf("usage: %s [--direct] [--monitor NAME] [--seconds N]\n", argv[0]); return 0; }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) { fprintf(stderr, "cannot open X display\n"); return 1; }

    auto outputs = list_outputs(dpy);
    OutputRect target{};
    bool found = false;
    for (auto& o : outputs) {
        bool is_glasses_mode = (o.h == 1200 && (o.w == 1920 || o.w == 3840));
        bool is_laptop = o.name.rfind("eDP", 0) == 0 || o.name.rfind("LVDS", 0) == 0;
        if (!monitor.empty() ? o.name == monitor : (is_glasses_mode && !is_laptop)) {
            target = o; found = true; break;
        }
    }
    if (!found) { fprintf(stderr, "target output not found\n"); return 1; }
    printf("target: %s %dx%d+%d+%d\n", target.name.c_str(), target.w, target.h,
           target.x, target.y);

    VkRend vk{};
    if (!vkr_create_instance(vk, direct)) return 1;
    SurfaceOut sout{};
    bool ok = direct && vk.has_display_ext &&
              direct_acquire(dpy, vk.instance, target.id, sout);
    if (!ok) {
        if (direct) fprintf(stderr, "direct failed — window fallback\n");
        if (!window_create(dpy, vk.instance, target.x, target.y, target.w, target.h, sout))
            return 1;
    }
    vk.phys = sout.phys;
    vk.surface = sout.surface;
    if (!vkr_init_device(vk) || !vkr_init_swapchain(vk) || !vkr_init_pipeline(vk))
        return 1;

    const uint32_t TW = 512, TH = 512;
    if (!vkr_init_texture(vk, TW, TH, TW * 4)) return 1;
    std::vector<uint32_t> px(TW * TH);

    auto t0 = std::chrono::steady_clock::now();
    int frames = 0;
    while (g_running) {
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (t > seconds) break;
        int shift = int(t * 60) % 64;
        for (uint32_t y = 0; y < TH; y++)
            for (uint32_t x = 0; x < TW; x++)
                px[y * TW + x] = (((x + shift) / 32 + y / 32) & 1) ? 0xff2d3646 : 0xff59c2ff;
        vkr_upload(vk, px.data(), px.size() * 4);

        QuadDraw d[5] = {};
        float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        memcpy(d[0].mvp, ident, sizeof(ident));
        d[0].color[0] = d[0].color[1] = d[0].color[2] = d[0].color[3] = 1.f;
        d[0].rect[2] = 0.7f;  // half extents in clip space (MVP = identity)
        d[0].rect[3] = 0.7f;
        d[0].textured = true;
        for (int i = 1; i < 5; i++) {
            memcpy(d[i].mvp, ident, sizeof(ident));
            d[i].color[0] = 0.35f; d[i].color[1] = 0.76f; d[i].color[2] = 1.f;
            d[i].color[3] = 1.f;
            d[i].textured = false;
        }
        const float bt = 0.02f, hw = 0.7f;
        d[1].rect[1] = -hw; d[1].rect[2] = hw + bt; d[1].rect[3] = bt;  // top (y-down)
        d[2].rect[1] =  hw; d[2].rect[2] = hw + bt; d[2].rect[3] = bt;  // bottom
        d[3].rect[0] = -hw; d[3].rect[2] = bt; d[3].rect[3] = hw + bt;  // left
        d[4].rect[0] =  hw; d[4].rect[2] = bt; d[4].rect[3] = hw + bt;  // right
        if (vkr_draw(vk, d, 5)) frames++;

        while (XPending(dpy)) { XEvent ev; XNextEvent(dpy, &ev); }  // drain
    }
    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("%d frames in %.1f s = %.1f fps\n", frames, el, frames / el);

    vkr_destroy(vk);
    if (sout.direct) direct_restore(dpy);
    else if (sout.window) XDestroyWindow(dpy, sout.window);
    XCloseDisplay(dpy);
    return 0;
}
