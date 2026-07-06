#include "sbs_mode.h"

#include <cstdio>
#include <unistd.h>

#include "vk_surface.h"
#include "viture_protocol.h"

using viture::protocol::DisplayMode::MODE_3840_1200_90HZ;
using viture::protocol::DisplayMode::MODE_1920_1200_120HZ;

static bool output_is_wide(Display* dpy, const std::string& name) {
    for (auto& o : list_outputs(dpy))
        if (o.name == name) return o.w >= 3840;
    return false;
}

// set_display_mode with retries: the command shares the USB-C cable with the
// DP link, and a mode change or lease release retrains that link — a
// transient USB execution error (rc -4, seen on hardware at exit) would
// otherwise strand the panel in SBS.
static int set_mode_retry(XRDeviceProviderHandle provider, int mode, const char* what) {
    int rc = -1;
    for (int attempt = 0; attempt < 4; attempt++) {
        if (attempt) usleep(500 * 1000);
        rc = xr_device_provider_set_display_mode(provider, mode);
        printf("sbs: %s mode 0x%02x -> rc %d%s\n", what, mode, rc,
               rc == 0 || attempt == 3 ? "" : " (retrying)");
        if (rc == 0) break;
    }
    return rc;
}

int sbs_enter(XRDeviceProviderHandle provider, Display* dpy,
              const std::string& output_name, int timeout_ms) {
    // Reading BEFORE any mode command is reliable (lag appears after
    // commands); fall back to the known native mode if the read fails.
    int orig = xr_device_provider_get_display_mode(provider);
    if (orig < 0) orig = MODE_1920_1200_120HZ;

    int rc = set_mode_retry(provider, MODE_3840_1200_90HZ, "enter");
    if (rc != 0) {
        fprintf(stderr, "sbs: set_display_mode(0x45) failed (rc %d) — staying 2D\n", rc);
        return -1;
    }
    for (int waited = 0; waited < timeout_ms; waited += 100) {
        usleep(100 * 1000);
        if (output_is_wide(dpy, output_name)) {
            printf("sbs: %s is 3840-wide after %d ms\n", output_name.c_str(), waited + 100);
            return orig;
        }
    }
    fprintf(stderr, "sbs: %s never reported 3840-wide in %d ms — restoring 2D\n",
            output_name.c_str(), timeout_ms);
    set_mode_retry(provider, orig, "restore");
    return -1;
}

void sbs_exit(XRDeviceProviderHandle provider, int orig_mode) {
    if (orig_mode < 0 || !provider) return;
    set_mode_retry(provider, orig_mode, "restore");
}
