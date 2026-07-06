// sbs_spike.cpp — throwaway hardware spike: does the Luma Ultra actually enter
// 3840-wide side-by-side (3D/SBS) mode when asked over the SDK's USB control
// channel, on Linux? This gates the whole stereo-rendering estimate; see
// docs/testing/2026-07-05-sbs-3d-spike-handoff.md.
//
// It does NOT render anything. It only: brings up the SDK provider, reads the
// current display mode, switches the panel to 3D (or an explicit --mode), holds
// so you can eyeball `xrandr`/the glasses, then ALWAYS restores the original
// mode (on normal exit, on error, and on Ctrl+C). Nothing here is meant to ship.
//
// Build:  cd spatial-screens && make sbs-spike
// Run:    ./run-spike.sh            (switch_dimension(true) → 3840x1080@60)
//         ./run-spike.sh --mode 0x45   (set_display_mode → 3840x1200@90, native height)
//         ./run-spike.sh --hold 30     (hold 3D for 30s before restoring)
// Stop viture-bridge and any spatial-screens first — the SDK is single-client.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <dirent.h>
#include <string>
#include <unistd.h>

#include "viture_glasses_provider.h"
#include "viture_device_carina.h"
#include "viture_protocol.h"

static XRDeviceProviderHandle g_provider = nullptr;
static std::atomic<int> g_stop{0};

static void on_signal(int) { g_stop.store(1); }

// Same USB-tree scan spatial-screens uses: VITURE vendor 0x35ca, first PID the
// SDK recognizes. (main.cpp:scan_viture_pid)
static int scan_viture_pid() {
    DIR* dir = opendir("/sys/bus/usb/devices");
    if (!dir) return 0;
    dirent* e;
    int found = 0;
    while ((e = readdir(dir)) && !found) {
        std::string base = std::string("/sys/bus/usb/devices/") + e->d_name;
        FILE* fv = fopen((base + "/idVendor").c_str(), "r");
        if (!fv) continue;
        char vendor[8] = {0};
        if (fgets(vendor, sizeof(vendor), fv) && strncmp(vendor, "35ca", 4) == 0) {
            FILE* fp = fopen((base + "/idProduct").c_str(), "r");
            if (fp) {
                unsigned pid = 0;
                if (fscanf(fp, "%x", &pid) == 1 &&
                    xr_device_provider_is_product_id_valid(int(pid)))
                    found = int(pid);
                fclose(fp);
            }
        }
        fclose(fv);
    }
    closedir(dir);
    return found;
}

// Human-readable name for a viture::protocol::DisplayMode value.
static const char* mode_name(int m) {
    using namespace viture::protocol::DisplayMode;
    switch (m) {
        case MODE_1920_1080_60HZ: return "1920x1080@60 (2D)";
        case MODE_3840_1080_60HZ: return "3840x1080@60 (SBS/3D)";
        case MODE_1920_1080_90HZ: return "1920x1080@90 (2D)";
        case MODE_1920_1080_120HZ: return "1920x1080@120 (2D)";
        case MODE_3840_1080_90HZ: return "3840x1080@90 (SBS/3D)";
        case MODE_1920_1080_60HZ_120HZ: return "1920x1080@60→120 (2D, interp)";
        case MODE_1920_1200_60HZ: return "1920x1200@60 (2D)";
        case MODE_3840_1200_60HZ: return "3840x1200@60 (SBS/3D, native height)";
        case MODE_1920_1200_90HZ: return "1920x1200@90 (2D)";
        case MODE_1920_1200_120HZ: return "1920x1200@120 (2D)";
        case MODE_3840_1200_90HZ: return "3840x1200@90 (SBS/3D, native height)";
        case MODE_1920_1200_60HZ_120HZ: return "1920x1200@60→120 (2D, interp)";
        case MODE_ULTRAWIDE_60HZ_120HZ: return "ultrawide@60→120 (Beast)";
        case MODE_SIDEMODE_60HZ: return "side-mode@60 (Beast)";
        default: return "unknown";
    }
}

// switch_dimension / set_display_mode share the same negative error codes.
static const char* rc_name(int rc) {
    switch (rc) {
        case 0: return "SUCCESS";
        case -1: return "param error";
        case -2: return "USB not available";
        case -3: return "display mode value incorrect";
        case -4: return "USB execution error";
        case -5: return "other error";
        default: return "?";
    }
}

static void print_mode(const char* tag) {
    int m = xr_device_provider_get_display_mode(g_provider);
    if (m < 0)
        printf("  %-10s get_display_mode -> error %d (%s)\n", tag, m, rc_name(m));
    else
        printf("  %-10s get_display_mode -> 0x%02x  %s\n", tag, m, mode_name(m));
}

int main(int argc, char** argv) {
    int explicit_mode = -1;     // --mode 0xNN : use set_display_mode instead of the toggle
    int hold_s = 20;            // --hold N : seconds in 3D before restoring
    bool no_restore = false;    // --no-restore : leave the panel in 3D (dangerous)
    bool restore_only = false;  // --restore : panic flag, put panel back to 2D and exit

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc)
            explicit_mode = (int)strtol(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--hold") && i + 1 < argc)
            hold_s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-restore"))
            no_restore = true;
        else if (!strcmp(argv[i], "--restore"))
            restore_only = true;
        else {
            printf("usage: %s [--mode 0xNN] [--hold N] [--no-restore] [--restore]\n", argv[0]);
            return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    // -- bring up the SDK (mirrors spatial-screens/src/main.cpp:sdk_init) --
    int pid = scan_viture_pid();
    if (!pid) {
        fprintf(stderr, "No supported VITURE glasses found (udev rule installed? glasses awake?).\n");
        return 1;
    }
    xr_device_provider_set_log_level(1);
    g_provider = xr_device_provider_create(pid);
    if (!g_provider) {
        fprintf(stderr, "provider create failed — is viture-bridge or spatial-screens still running? Stop it.\n");
        return 1;
    }
    if (xr_device_provider_get_device_type(g_provider) != XR_DEVICE_TYPE_VITURE_CARINA) {
        fprintf(stderr, "This spike expects a Luma Ultra (Carina) device.\n");
        xr_device_provider_destroy(g_provider);
        return 1;
    }
    // No sensor callbacks needed — this only issues USB control commands.
    if (register_callbacks_carina(g_provider, nullptr, nullptr, nullptr, nullptr) != 0 ||
        xr_device_provider_initialize(g_provider, nullptr) != 0) {
        fprintf(stderr, "SDK init failed\n");
        xr_device_provider_destroy(g_provider);
        return 1;
    }
    sleep(1);
    if (xr_device_provider_start(g_provider) != 0) {
        fprintf(stderr, "SDK start failed (permissions? another client?)\n");
        xr_device_provider_shutdown(g_provider);
        xr_device_provider_destroy(g_provider);
        return 1;
    }
    printf("SDK started (pid 0x%04x, Carina 6DoF)\n\n", pid);

    if (restore_only) {
        // Panic path: kill -9'd sessions leave the panel in SBS. Just put the
        // native 2D mode back and get out.
        using viture::protocol::DisplayMode::MODE_1920_1200_120HZ;
        int rc2 = xr_device_provider_set_display_mode(g_provider, MODE_1920_1200_120HZ);
        printf("--restore: set_display_mode(0x44) -> rc %d (%s)\n", rc2, rc_name(rc2));
        xr_device_provider_shutdown(g_provider);
        xr_device_provider_destroy(g_provider);
        return rc2 == 0 ? 0 : 1;
    }

    // -- record where we started so we can put it back exactly --
    int orig = xr_device_provider_get_display_mode(g_provider);
    printf("display mode BEFORE:\n");
    print_mode("before");
    printf("\n");

    // -- switch into 3D / SBS --
    printf(">>> Have a recovery terminal ready (SSH or laptop panel). If the\n"
           ">>> glasses go black, this spike restores 2D automatically after\n"
           ">>> --hold seconds, on Ctrl+C, and on exit. Manual panic:\n"
           ">>>   xrandr --output DP-1 --set non-desktop 0 && xrandr --output DP-1 --auto\n\n");

    int rc;
    if (explicit_mode >= 0) {
        printf("Calling set_display_mode(0x%02x  %s)...\n", explicit_mode, mode_name(explicit_mode));
        rc = xr_device_provider_set_display_mode(g_provider, explicit_mode);
    } else {
        printf("Calling switch_dimension(is_3d=true)  [documented: 3840x1080@60]...\n");
        rc = xr_device_provider_switch_dimension(g_provider, true);
    }
    printf("  -> rc %d (%s)\n\n", rc, rc_name(rc));

    if (rc != 0) {
        fprintf(stderr, "switch to 3D FAILED — the SDK rejected the mode change. "
                        "This is the key negative result; nothing to restore.\n");
    } else {
        // The panel/USB report may lag the command; sample a few times.
        printf("display mode AFTER (sampling ~3s):\n");
        for (int i = 0; i < 3 && !g_stop.load(); i++) {
            sleep(1);
            print_mode("after");
        }
        printf("\nNOW: in another terminal run `xrandr` (or `xrandr --output DP-1 --verbose`)\n"
               "and look for a 3840-wide mode on DP-1, and check whether both eyes\n"
               "show independent halves. Holding %ds (Ctrl+C to restore early)...\n", hold_s);
        for (int i = 0; i < hold_s && !g_stop.load(); i++) sleep(1);
    }

    // -- restore (always, unless explicitly told not to) --
    if (no_restore) {
        printf("\n--no-restore set: leaving panel in its current mode. Restore manually:\n"
               "  the glasses' own 2D/3D toggle, or rerun and let it restore.\n");
    } else {
        printf("\nRestoring original display mode...\n");
        int back;
        if (orig >= 0) {
            back = xr_device_provider_set_display_mode(g_provider, orig);
            printf("  set_display_mode(0x%02x %s) -> rc %d (%s)\n",
                   orig, mode_name(orig), back, rc_name(back));
        } else {
            back = xr_device_provider_switch_dimension(g_provider, false);
            printf("  switch_dimension(false) -> rc %d (%s)\n", back, rc_name(back));
        }
        sleep(1);
        print_mode("restored");
    }

    xr_device_provider_shutdown(g_provider);
    xr_device_provider_destroy(g_provider);
    printf("\ndone.\n");
    return rc == 0 ? 0 : 1;
}
