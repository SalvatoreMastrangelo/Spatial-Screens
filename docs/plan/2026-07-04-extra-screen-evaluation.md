# Extra-screen mode for spatial-screens: mechanism evaluation and recommendation

**Date:** 2026-07-04. Research inputs: local repo docs (`docs/plan/phase2-spatial-screens.md`, `docs/specs/2026-07-04-m3-remainder-design.md`, `spatial-screens/README.md`), source inspection of `reference/XRLinuxDriver`, `reference/viture_virtual_display`, `reference/viture-webxr-extension`, live probes of this machine's X11 session, GitHub code search of `wheaney/breezy-desktop`, Mutter/portal/COSMIC sources, and web research.

Question evaluated: instead of MIRRORING an existing monitor onto the glasses'
virtual screen, create a genuine EXTRA desktop output (extended real estate
that exists only virtually) for spatial-screens to capture and place in space.

## 0. Ground truth verified on this machine (2026-07-04)

- Kernel `7.0.11-76070011-generic`; X.Org 21.1.4; GNOME Shell **42.9**, X11 session.
- RandR providers: `modesetting` (Intel, provider 0, outputs eDP-1 / DP-1 / DP-2) and `NVIDIA-G0` (provider 1, PRIME **sink**, 4 disconnected outputs). X screen 2560x2800 now, max 16384x16384.
- **Portal ScreenCast on this session: `AvailableSourceTypes = 3`** (MONITOR|WINDOW — no VIRTUAL), interface version 4.
- `org.gnome.Mutter.ScreenCast` is on the session bus; a live probe (`CreateSession` → `RecordVirtual` → `Stop`) **succeeded at the D-Bus level** — but this is misleading, see §2.4: the backend gate fires at stream start, and Mutter's X11 backend cannot create virtual monitors.
- Packages: `xserver-xorg-video-dummy` and `evdi-dkms` available but not installed; the 22.04 `evdi-dkms` is a **stale 1.12.0** (useless on this kernel). Upstream evdi **v1.15.0 (released 2026-07-01) has preliminary Linux 7.2 support** — it keeps pace with this machine's 7.0 kernel ([releases](https://github.com/DisplayLink/evdi/releases)). `xserver-xorg-video-intel` is installed but unused (session runs modesetting).

## 1. What the prior art actually does (local source inspection)

- **XRLinuxDriver**: its `virtual_display` plugin creates **no OS output** — it computes optics/FOV params and publishes them via shared memory and gamescope ReShade uniforms for an external renderer (`src/plugins/virtual_display.c`). The `breezy_desktop` plugin only writes IMU pose + device optics to `/dev/shm/breezy_desktop_imu` (`src/plugins/breezy_desktop.c`). Zero hits for Mutter/ScreenCast/evdi/xrandr/headless in the entire tree.
- **viture_virtual_display (mgschwan)**: captures an **existing** monitor via the portal (`SelectSources types=1` in `xdg_source.c:899`) or V4L2; the "extra screen" in its README is just the physical glasses output added via OS display settings. No virtual output creation.
- **Breezy Desktop** (not in `reference/`; inspected via GitHub): virtual displays are created by `ui/src/virtualdisplay.py` calling **`org.gnome.Mutter.ScreenCast` → `Session.RecordVirtual({'is-platform': true})`**, then keeping a GStreamer **`pipewiresrc ! video/x-raw,caps ! fakesink`** pipeline alive — the PipeWire negotiation is what materializes the monitor and pins its resolution, and the consumer must stay connected for the monitor's lifetime. The GNOME extension (`gnome/src/monitormanager.js`) then merely *detects/configures* monitors via `org.gnome.Mutter.DisplayConfig`. Its README is explicit: KDE Plasma 6 or GNOME 45–50 only, and **"If you're running GNOME on Xorg, you won't be able to launch virtual displays unless you switch to Wayland"** ([README](https://github.com/wheaney/breezy-desktop)). wheaney's design discussion confirms he landed on the GNOME-extension approach after experimenting with Mutter-as-headless-compositor ([discussion #1](https://github.com/wheaney/breezy-desktop/discussions/1)).
- **Immersed's Linux guide** ([augustoicaro/Immersed-Linux-Virtual-Monitors](https://github.com/augustoicaro/Immersed-Linux-Virtual-Monitors)): four X11 workarounds — intel DDX `VirtualHeads`, NVIDIA `ConnectedMonitor` xorg.conf forcing, **evdi + debugfs force**, or a physical dummy plug. Native Immersed virtual displays: GNOME Wayland only.

## 2. Mechanism comparison

| # | Mechanism | Session | Real extra desktop area? | Survives DP-1 lease? | Works with Intel modesetting + NVIDIA hybrid? | Capturable by XShm / portal? | Setup friction | Risk |
|---|---|---|---|---|---|---|---|---|
| 1a | `xrandr --fb` grow + `--setmonitor … none` | X11 | Framebuffer yes, but **Mutter won't manage it** | Poor — Mutter reasserts config on the RandR churn our lease causes | Driver-agnostic | XShm: region readable; portal: not listed (not a Mutter monitor) | None (no root) | High (fights Mutter) |
| 1b | Enable a disconnected output (`--addmode` / debugfs `force`) | X11 | Yes if the driver lights it | Yes | Intel side is DP-only → DP link training fails without a sink; NVIDIA DP also flaky | XShm yes; portal yes (real monitor) | Root (debugfs), per-boot | High, driver-dependent |
| 1c | intel DDX `VirtualHeads` (`VIRTUAL1`) | X11 | Yes, first-class | Untested — swaps the DDX under the lease path | Requires abandoning modesetting for the deprecated intel DDX session-wide | XShm yes; portal yes | xorg.conf + logout | High (regressive driver swap) |
| 1d | NVIDIA `ConnectedMonitor` xorg.conf | X11 | Yes, first-class | Yes | Output lives on the dGPU → reverse-PRIME copies, dGPU never sleeps | XShm yes; portal yes | xorg.conf + logout | Medium-high |
| 2 | `xserver-xorg-video-dummy` second X screen | X11 | Separate X *screen*, **not** part of the desktop — can't drag windows there | N/A | Yes | XShm on `:0.1` root only; portal no | xorg.conf + logout | Low but wrong UX |
| 3 | **evdi + libevdi client** | X11 (Wayland-GNOME also works — DisplayLink precedent; COSMIC unverified) | **Yes, first-class hotplugged monitor** | **Yes — separate DRM device/provider, orthogonal to the Intel DP-1 lease** | Yes — X auto-binds the evdi GPU (X.Org ≥1.20.7), content rendered by Intel, copied via PRIME | **XShm yes** (region of root fb); portal yes (real Mutter monitor); *plus* direct `libevdi` grab w/ damage | DKMS module (root once), udev rule, upstream ≥1.15 needed (apt's 1.12 too old) | Medium (out-of-tree module; upstream actively tracks kernels incl. 7.2-pre) |
| 4 | Mutter `RecordVirtual` / gnome-remote-desktop | **Wayland only** (see 2.4) | Yes — compositor-native virtual monitor, stream = content | N/A (no RandR lease on Wayland) | Yes (pure compositor) | Capture **is** the mechanism (PipeWire) | None (unsandboxed D-Bus) | Low on GNOME Wayland; **zero X11 support** |
| 5 | Portal ScreenCast `SelectSources types=VIRTUAL(4)` | Wayland (backend-dependent) | Yes | N/A | Yes | Capture is the mechanism | None; restore-token capable | Low; GNOME implements since 42.rc, advertises since 44; **COSMIC: not implemented** |
| 6 | wlroots headless output (`swaymsg create_output`, `WLR_HEADLESS_OUTPUTS`) | Wayland (wlroots comps only) | Yes | N/A | Yes | Portal (x-d-p-wlr) yes | None | Irrelevant: neither GNOME nor COSMIC is wlroots |
| 7 | COSMIC (cosmic-comp / x-d-p-cosmic) | Wayland | **Nothing today** | N/A | — | — | — | Open feature requests only |

### 2.1 X11 RandR tricks (1a/1b)

`--setmonitor` over a grown framebuffer is the Deskreen/AnyDesk trick ([deskreen#42](https://github.com/pavlobu/deskreen/issues/42), [gist](https://gist.github.com/chitholian/9cac41d22b76364360429cc2a5ffa681)). It creates a RandR 1.5 *monitor* object with no CRTC behind it. The fatal problem on GNOME: **Mutter's X11 backend derives logical monitors from outputs/CRTCs and its own stored config, not from client-set RandR monitors** — runtime `--setmonitor` is ignored for workarea/window placement, and Mutter re-applies its own layout on any RandR event ([Arch forum](https://bbs.archlinux.org/viewtopic.php?id=264701), [RH bugzilla](https://bugzilla.redhat.com/show_bug.cgi?id=1581806)). Our app *guarantees* RandR churn at launch (the `non-desktop=1` + lease flip), so the grown fb/fake monitor would be reverted or orphaned exactly when we need it. Windows can't be reliably placed there; the portal picker won't list it. Rejected.

Enabling a *disconnected* connector (1b) works on some AMD setups (that's what the Deskreen issue actually did with `DVI-D-0`); on this machine the Intel spare is **DP-2 — DisplayPort requires link training against a real sink**, so debugfs `force on` + addmode generally fails or produces a zombie CRTC; the Immersed guide itself warns DP forcing "might not work and create video issues". Rejected.

### 2.2 intel DDX VirtualHeads (1c) and NVIDIA ConnectedMonitor (1d)

Both give genuine first-class outputs GNOME treats as monitors — this is why Immersed's guide uses them. Both are rejected here:

- VirtualHeads requires switching the session from modesetting to the unmaintained `xf86-video-intel` DDX (`Option "VirtualHeads"` in `20-intel.conf` + logout). That changes the acceleration path (SNA), the PRIME wiring to the NVIDIA sink, and puts our validated `VK_EXT_acquire_xlib_display` lease path onto an untested driver. Too much collateral for one feature.
- ConnectedMonitor puts the virtual output on the **NVIDIA** provider: xorg.conf surgery + logout, keeps the dGPU permanently awake on a hybrid laptop, DP forcing is flaky, and content reaches it via reverse-PRIME. Documented as a no-code fallback only.

### 2.3 xserver-xorg-video-dummy (2)

The dummy driver creates a second X *screen* (`:0.1`), not more desktop on `:0.0` — GNOME doesn't manage it, windows can't be dragged into it, no WM runs there. It's for fully headless boxes (that's exactly how the Pop!_OS headless guides use it, e.g. [this one](https://medium.com/@nineties.style/pop-os-24-04-headless-mode-434f4d976b26)). Wrong tool. Rejected.

### 2.4 Mutter RecordVirtual (4) — and why X11 is a hard no

Mechanism (Breezy's exact recipe): `org.gnome.Mutter.ScreenCast.CreateSession` → `Session.RecordVirtual({'is-platform': true})` → `Stream.PipeWireStreamAdded` → connect a PipeWire consumer whose negotiated caps determine the monitor's resolution/refresh; the monitor exists only while the session + consumer live ([RecordVirtual docs](https://github.com/jadahl/gnome-remote-desktop/blob/master/src/org.gnome.Mutter.ScreenCast.xml), [gnome-rdp-virtual-monitor](https://github.com/psqli/gnome-rdp-virtual-monitor), [gnome-remote-desktop#79](https://gitlab.gnome.org/GNOME/gnome-remote-desktop/-/issues/79)).

X11 story, source-confirmed on the gnome-42 branch: `meta_monitor_manager_create_virtual_monitor` errors with **"Backend doesn't support creating virtual monitors"** unless the backend implements the vfunc; **`MetaMonitorManagerXrandr` does not implement it** (not in its `class_init` vfunc table), while **`MetaMonitorManagerNative` does** ([meta-monitor-manager.c](https://gitlab.gnome.org/GNOME/mutter/-/raw/gnome-42/src/backends/meta-monitor-manager.c), [meta-monitor-manager-xrandr.c](https://gitlab.gnome.org/GNOME/mutter/-/raw/gnome-42/src/backends/x11/meta-monitor-manager-xrandr.c), [meta-monitor-manager-native.c](https://gitlab.gnome.org/GNOME/mutter/-/raw/gnome-42/src/backends/native/meta-monitor-manager-native.c)). A live probe on this X11 session showed `RecordVirtual` returning a stream path — do not be fooled by that if you experiment: the D-Bus call defers monitor creation to stream start, which is where the X11 backend fails. Breezy's README and Immersed's FAQ both corroborate: virtual displays are Wayland-only on GNOME.

Note for later: mutter 42 (this machine's version) already has RecordVirtual on the native backend, so even "old" GNOME Wayland could do this; and GNOME 50 recently improved the API (predefined mode lists, HiDPI — [Phoronix](https://www.phoronix.com/news/GNOME-50-Remote-Desktop-HiDPI)).

### 2.5 Portal VIRTUAL source type (5) — the future-proof façade over (4)

The ScreenCast portal's `SelectSources` `types` bitmask defines **VIRTUAL = 4, "Extend with new virtual monitor"** ([impl portal docs](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.impl.portal.ScreenCast.html)). `xdg-desktop-portal-gnome` implemented virtual sources in **42.rc** and started advertising them in `AvailableSourceTypes` in **44.alpha** (NEWS file) — on Wayland only, since it fronts Mutter's RecordVirtual. This is *exactly* the same D-Bus dance the M3 portal backend implements, differing only in the `types` value, and it delivers the virtual monitor's content on the same PipeWire stream the backend already consumes. **Capture and screen creation collapse into one code path.** This session reports `AvailableSourceTypes = 3` (X11), confirming runtime detection is trivial.

### 2.6 Wayland-native / COSMIC (6, 7)

- wlroots compositors create headless outputs trivially (`swaymsg create_output`, `WLR_HEADLESS_OUTPUTS`), captured via `xdg-desktop-portal-wlr` — irrelevant here (GNOME today, COSMIC tomorrow; neither is wlroots).
- KDE Plasma 6 Wayland has `krfb-virtualmonitor` / kwin virtual outputs — noted for completeness only.
- **COSMIC: no virtual output support exists today.** `xdg-desktop-portal-cosmic` reports `available_source_types = MONITOR | WINDOW` (= 3) and defines but never implements `SOURCE_TYPE_VIRTUAL` (`src/screencast.rs`, master). Open feature requests: [xdg-desktop-portal-cosmic#162 "New Virtual Output"](https://github.com/pop-os/xdg-desktop-portal-cosmic/issues/162) and [#216 "Treating a Cosmic Stack as a Virtual Display"](https://github.com/pop-os/xdg-desktop-portal-cosmic/issues/216); nothing in cosmic-comp. This is a real gap in the "Wayland/COSMIC future" — plan for it (see recommendation).

### 2.7 evdi (3) — the X11 winner

[evdi](https://github.com/DisplayLink/evdi) (GPLv2 kernel module + LGPL `libevdi`) creates a DRM device whose connectors are plugged/unplugged by a userspace client: the client calls `evdi_connect()` with an **EDID blob** (which defines the modes), the kernel fires a hotplug event, and the compositor/X server treats it as a genuine monitor; the client then services `update_ready`/`grab_pixels` to receive frames with damage rects. Critical properties for us:

- **First-class monitor on GNOME X11 42**: same plumbing as DisplayLink docks (which work on Ubuntu 22.04 X11). X.Org ≥1.20.7 auto-binds hotplugged GPU screens, and GNOME auto-extends new monitors — likely zero RandR orchestration beyond placement.
- **Orthogonal to the direct-display lease**: it is a separate DRM device and RandR provider; the DP-1 lease on the Intel provider is untouched. The RandR reflow it causes at connect/disconnect is an event class spatial-screens already handles (post-lease source-rect re-resolution).
- **Capture for free**: the evdi output's region is part of the X screen (rendered by the Intel GPU, PRIME-copied to the sink), so the **existing XShm backend captures it with `--capture <output-name>` unchanged**; the portal picker will also list it (it's a real Mutter monitor). Optionally, `libevdi.grab_pixels` is itself a superior damage-driven capture path later.
- **Clean lifecycle**: connector exists only while the client holds the device — if the app crashes, the fd closes and the monitor unplugs itself. Self-healing, unlike the lease's `non-desktop=1` stranding.
- **Friction/risk**: out-of-tree DKMS module → root install once, rebuilds on kernel updates, Secure Boot signing if enabled. Must use upstream (v1.15.0, 2026-07-01, preliminary kernel-7.2 support) — the 22.04 archive's 1.12 will not build on kernel 7.0. Device access needs a udev rule (repo already has the `make install-udev` precedent). Known cosmetic issue on hybrid Intel+NVIDIA: cursor artifacts reported in the Immersed guide.
- Wayland: GNOME Wayland drives evdi outputs (DisplayLink precedent). cosmic-comp is unverified — do not count on it.

## 3. Recommendation

### (a) Current X11 session: evdi

evdi is the only mechanism on this exact stack (GNOME 42 X11, modesetting Intel + NVIDIA sink, DP-1 leased) that produces a *real, GNOME-managed, capturable* extra monitor without swapping display drivers, editing xorg.conf, logging out, or fighting Mutter. It composes perfectly with the current architecture: the extra screen appears as a normal RandR output, and everything downstream (XShm capture, portal capture, resize handling, `--capture NAME`) already works. Fallback if evdi's DKMS ever breaks on a kernel update: the NVIDIA `ConnectedMonitor` recipe, documented but not implemented.

### (b) Future Wayland/COSMIC session: portal VIRTUAL, with a COSMIC contingency

The portal `SelectSources types=VIRTUAL` path is the strategic bet: it is the M3 portal backend with one different bitmask, it is compositor-blessed, sandbox-safe, restore-token compatible, and the stream *is* the screen (no separate capture step, no reverse-PRIME copies). It works on GNOME Wayland today (implemented since 42.rc, advertised since 44). **COSMIC cannot do this yet** — so: (1) gate on `AvailableSourceTypes & 4` at runtime and degrade to mirror mode with a clear log; (2) track/upvote [x-d-p-cosmic#162](https://github.com/pop-os/xdg-desktop-portal-cosmic/issues/162); (3) consider an upstream contribution — a virtual-output + portal-VIRTUAL implementation for cosmic-comp is well-scoped and aligns with this project's Pop!_OS differentiation; (4) evdi remains the fallback for as long as Pop!_OS ships an X11 session. (Independently, Wayland *presentation* needs `VK_EXT_acquire_drm_display`/drm-lease-v1 — wlroots and recent Mutter implement the lease protocol, cosmic-comp needs verifying — but that is the existing, separately-tracked direct-mode concern, not an extra-screen concern.)

## 4. Implementation milestones for "extra screen" mode

**ES0 — evdi spike (manual, no repo code, ~half a day).**
Build upstream evdi v1.15.0 via DKMS on kernel 7.0.11; `modprobe evdi initial_device_count=1`. Use a throwaway libevdi client (C, ~100 lines: open device, `evdi_connect` with a canned 1920x1080@60 EDID, poll and ack events) to verify, in order: output appears in `xrandr` under a new provider and GNOME Settings; a window can be dragged there; `./spatial-screens --capture <new-output>` textures it (zero app changes); coexistence with the DP-1 lease (launch order both ways); teardown/reflow on client exit; kill -9 self-healing. Exit criteria: the virtual screen visible in the glasses, overhead measured, quirks (cursor artifacts?) noted in a test-handoff doc.

**ES1 — `extra-screen` helper (small, shippable).**
`spatial-screens/extra-screen/` standalone tool (mirrors the `vk-test`/`capture-test` precedent): `./extra-screen --mode 1920x1080@60 [--edid FILE]` — connects an evdi device with a canned EDID (check 2–3 pregenerated EDID blobs into the repo; no EDID-synthesis code), services `update_ready` cheaply, disconnects on SIGINT/SIGTERM. Plus `make install-evdi` documenting/automating: DKMS install from upstream, `modprobe.d` conf (`options evdi initial_device_count=1` + module load), udev rule for device access (mirrors `install-udev`). README section with the ConnectedMonitor fallback recipe. At this point users get extra-screen mode by running the helper + `--capture`.

**ES2 — integrated lifecycle in spatial-screens.**
Flag/config key `extra-screen = WxH[@Hz]` (default off; mutually exclusive with `--capture`, which it supersedes). Launch: after SDK init and *after* direct-mode lease acquisition settles (one desktop reflow, not two) → connect evdi → wait for the RandR output to appear (provider event, timeout) → place it (right-of desktop via RandR, or accept GNOME's auto-extend) → resolve it as the capture source through the normal backend factory (XShm today; portal also sees it). Exit path (Q/SIGINT/SIGTERM, and the existing teardown-on-capture-failure paths): stop capture → `evdi_disconnect` → existing RandR restore. Failure at any step degrades to mirror mode with a `log` telemetry event, never a fatal exit (matches M3's philosophy). Telemetry: `app` message gains `"source":"extra"|"mirror"`.

**ES3 — Wayland-virtual backend (when a Wayland session is on the horizon).**
`--extra-screen` under Wayland routes to the portal backend with `types=VIRTUAL` if `AvailableSourceTypes & 4`, else logs and falls back to mirror. Desired size/refresh goes into the PipeWire caps negotiation (that is how virtual monitors get their resolution). Separate restore token from the mirror token (state key `restore-token-virtual`). Optional non-portal variant via direct `RecordVirtual` (`is-platform`, persistent) if the portal dialog UX ever grates.

**ES4 — COSMIC engagement (parallel, non-code).**
Comment on x-d-p-cosmic#162 with this use case; evaluate contributing virtual-output support to cosmic-comp + the portal backend; re-test evdi under cosmic-comp when a COSMIC install exists.

## 5. Impact on the M3 design being implemented now

Small, cheap-now structural choices — none change M3's scope:

1. **Parameterize the `types` bitmask** in `capture_portal.cpp`'s `SelectSources` (constructor arg defaulting to MONITOR=1) instead of hardcoding `1`, and read `AvailableSourceTypes` during startup (log it). That single knob is 90% of ES3.
2. **Split the PipeWire consumer from the portal D-Bus dance** inside the portal backend (e.g. an internal `PipeWireStream` taking `(fd, node_id)`; support connecting to the default remote too). Then a future Mutter-`RecordVirtual` path, or an evdi-native grabber, reuses the frame plumbing.
3. **State file**: keep restore-token keying extensible (`restore-token` today; a sibling `restore-token-virtual` later) rather than assuming one token per install.
4. **Fallback chain semantics**: "portal fails → XShm → test" is per-*backend*; extra-screen mode adds an orthogonal "source acquisition" stage before it. Keep the backend factory ignorant of *what* monitor it captures (it already is — good) so ES2 only adds a pre-step that determines the capture target name.
5. **Telemetry**: reserve `source` in the `app` message schema (dashboard ignores unknown fields, so this costs nothing now).
6. The design's line "under portal, `--capture NAME` is ignored (the picker/token owns source selection)" stays true for mirror mode, but note in the code comment that extra-screen mode will later need the picker *bypassed* (VIRTUAL sources show no monitor picker; the dialog just asks consent).

## 6. Sources

- Local: `docs/plan/phase2-spatial-screens.md`, `docs/specs/2026-07-04-m3-remainder-design.md`, `spatial-screens/README.md`, `reference/XRLinuxDriver/src/plugins/{virtual_display,breezy_desktop}.c`, `reference/viture_virtual_display/xdg_source.c`; live probes: `xrandr --listproviders`, portal `AvailableSourceTypes`/`version`, `gnome-shell --version`, Mutter `RecordVirtual` D-Bus probe, `apt-cache policy`.
- Breezy Desktop: https://github.com/wheaney/breezy-desktop (README; `ui/src/virtualdisplay.py`, `gnome/src/monitormanager.js` via code search), https://github.com/wheaney/breezy-desktop/discussions/1, https://www.phoronix.com/news/GNOME-This-Week-Breezy
- Mutter: https://gitlab.gnome.org/GNOME/mutter/-/raw/gnome-42/src/backends/meta-monitor-manager.c, `.../x11/meta-monitor-manager-xrandr.c`, `.../native/meta-monitor-manager-native.c`, `.../meta-screen-cast-session.c`; https://github.com/jadahl/gnome-remote-desktop/blob/master/src/org.gnome.Mutter.ScreenCast.xml; https://gitlab.gnome.org/GNOME/gnome-remote-desktop/-/issues/79; https://github.com/psqli/gnome-rdp-virtual-monitor; https://www.phoronix.com/news/GNOME-50-Remote-Desktop-HiDPI
- Portal: https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.impl.portal.ScreenCast.html; xdg-desktop-portal-gnome NEWS (42.rc "Support virtual screen cast sources", 44.alpha "Advertise…")
- evdi: https://github.com/DisplayLink/evdi (v1.15.0 release notes, 2026-07-01: preliminary kernel 7.2), https://github.com/DisplayLink/evdi/issues/262
- Immersed guide: https://github.com/augustoicaro/Immersed-Linux-Virtual-Monitors; https://immersed.com/faq
- X11 tricks: https://github.com/pavlobu/deskreen/issues/42; https://gist.github.com/chitholian/9cac41d22b76364360429cc2a5ffa681; https://bbs.archlinux.org/viewtopic.php?id=264701; https://bugzilla.redhat.com/show_bug.cgi?id=1581806
- COSMIC: https://github.com/pop-os/xdg-desktop-portal-cosmic (src/screencast.rs: `available_source_types = MONITOR|WINDOW`, version 4), issues #162 and #216; https://github.com/emersion/xdg-desktop-portal-wlr

**Bottom line:** extra-screen on today's X11 rig = **evdi** (spike it manually first — ES0 is half a day and de-risks everything); extra-screen on future Wayland = **portal `types=VIRTUAL`**, which the M3 portal backend should anticipate now by parameterizing the source-type bitmask and decoupling the PipeWire consumer — and COSMIC needs either an upstream contribution or patience, because nothing exists there yet.
