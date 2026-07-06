// Panel SBS/3D mode lifecycle for spatial-screens. Confirmation is RandR-only:
// get_display_mode lags a full command cycle and must NEVER gate the switch
// (docs/testing/2026-07-05-sbs-3d-spike-handoff.md finding #1).
#pragma once
#include <string>
#include <X11/Xlib.h>
#include "viture_glasses_provider.h"

// Switch the panel to 3840x1200@90 (0x45) and wait (<= timeout_ms) until
// RandR reports `output_name` at >= 3840 wide. Returns the pre-switch mode to
// restore on exit (the BEFORE get_display_mode read, which is reliable, or
// 0x44 if that read failed). Returns -1 if the switch was rejected or never
// became visible — in that case the panel has already been restored.
int sbs_enter(XRDeviceProviderHandle provider, Display* dpy,
              const std::string& output_name, int timeout_ms = 3000);

// Restore the pre-session mode. No-op if orig_mode < 0.
void sbs_exit(XRDeviceProviderHandle provider, int orig_mode);
