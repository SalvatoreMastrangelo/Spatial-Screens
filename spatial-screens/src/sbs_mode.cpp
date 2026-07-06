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

int sbs_enter(XRDeviceProviderHandle provider, Display* dpy,
              const std::string& output_name, int timeout_ms) {
    // Reading BEFORE any mode command is reliable (lag appears after
    // commands); fall back to the known native mode if the read fails.
    int orig = xr_device_provider_get_display_mode(provider);
    if (orig < 0) orig = MODE_1920_1200_120HZ;

    int rc = xr_device_provider_set_display_mode(provider, MODE_3840_1200_90HZ);
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
    xr_device_provider_set_display_mode(provider, orig);
    return -1;
}

void sbs_exit(XRDeviceProviderHandle provider, int orig_mode) {
    if (orig_mode < 0 || !provider) return;
    int rc = xr_device_provider_set_display_mode(provider, orig_mode);
    printf("sbs: restore mode 0x%02x -> rc %d\n", orig_mode, rc);
}
