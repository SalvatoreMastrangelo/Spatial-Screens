# Phase 2: Spatial Screens for VITURE Luma Ultra

Feature roadmap for a standalone Linux program that places virtual screens in 3D space
using the Luma Ultra's 6DoF tracking, with presets (ultrawide, widescreen, multi-panel).
Written 2026-07-03, based on web research plus source inspection of the repos already
cloned under `reference/` (XRLinuxDriver, viture_virtual_display, viture-webxr-extension).

---

## 1. What the hardware and SDK realistically support today

### 1.1 Luma Ultra hardware

| Capability | Status | Source |
|---|---|---|
| Display | Sony micro-OLED, 1920x1200 per eye, up to 120 Hz, ~1250-1500 nits, 52 deg FOV | [VITURE product page](https://www.viture.com/product/viture-luma-ultra-xr-glasses), [Amazon listing](https://www.amazon.com/VITURE-Luma-Ultra-Gestures-Real-time/dp/B0FFT72CBX); FOV confirmed as `VITURE_LUMA_ULTRA_FOV = 52.0` in `reference/XRLinuxDriver/src/devices/viture.c` |
| Cameras | 1x RGB + 2x grayscale tracking/depth cameras (stereo) | [VITURE Luma series page](https://www.viture.com/luma) |
| 6DoF | Yes — visual-inertial odometry (VIO) using the stereo cameras + IMU. Officially supported in SpaceWalker on Windows/macOS and on the Pro Neckband | [VITURE Luma guide](https://www.viture.com/blog/the-ultimate-guide-to-choosing-your-new-viture-glasses-luma-pro-luma-ultra-luma-and-the-beast) |
| Hand tracking | Marketed ("AR hand gestures"), but only exposed via SpaceWalker and the Unity SDK on the Pro Neckband — not via desktop SDKs | [VITURE developer page](https://www.viture.com/developer) |
| USB IDs | VID `0x35ca`, Luma Ultra PIDs `0x1101`, `0x1104` (internal codename "Carina") | `reference/XRLinuxDriver/src/devices/viture.c` |
| Firmware-native display features | display mode, "native DoF" anchoring with recenter, display distance and size settings, 2D/3D (SBS) switch, brightness, electrochromic film | `reference/XRLinuxDriver/include/sdks/viture_glasses_provider.h` (`xr_device_provider_set_display_mode_and_native_dof`, `xr_device_provider_native_dof_recenter`, `set_display_distance`, `set_display_size`, `switch_dimension`) |

### 1.2 The key question: is 6DoF exposed to third parties?

**Yes — on Linux, today, via VITURE's closed-source native SDK.** This is the single most
important research finding, and it is verifiable locally:

- The official developer page advertises the cross-platform "XR Glasses SDK"
  (Android, Linux, Windows, macOS) with "high-performance APIs for 3DoF/6DoF tracking,
  raw IMU data, camera access, and real-time device control"
  ([viture.com/developer](https://www.viture.com/developer)).
- Ground truth: XRLinuxDriver (official VITURE collaboration) ships VITURE's
  closed-source binaries `libglasses.so` and `libcarina_vio.so` (plus OpenCV 4.2) for
  x86_64 and aarch64 — the VIO/SLAM runs **on the host CPU**, fed by stereo camera
  frames streamed over USB. See `reference/XRLinuxDriver/lib/x86_64/viture/`.
- The Carina (Luma Ultra) C API in
  `reference/XRLinuxDriver/include/sdks/viture_device_carina.h` exposes:
  - `register_callbacks_carina(handle, pose_cb, vsync_cb, imu_cb, camera_cb)` —
    pose callback delivers **7 floats: position (x,y,z) + quaternion (w,x,y,z)** with
    timestamps; also VSync events and **stereo camera frames** (left/right buffers with
    width/height).
  - `get_gl_pose_carina(handle, pose, predict_time)` — polled 6DoF pose **with
    forward-prediction in nanoseconds** (world-from-body matrix, OpenGL convention:
    x right, y up, z backward). XRLinuxDriver polls this at 1000 Hz.
  - `reset_pose_carina(handle)` — recenter/reset origin.
- Proof it works end-to-end: Breezy Desktop's Pro tier ships "device position (6DoF) —
  e.g. for the VITURE Luma Ultra glasses" on Linux
  ([breezy-desktop README](https://github.com/wheaney/breezy-desktop)), and
  XRLinuxDriver lists Luma Ultra as supported/recommended
  ([XRLinuxDriver supported devices](https://github.com/wheaney/XRLinuxDriver#supported-devices)).

Caveats:

- The SDK is **closed source** (all VITURE devices in XRLinuxDriver are marked
  "Closed source SDK", official collaboration). Redistribution terms for our own app
  need to be confirmed with VITURE; the binaries are currently distributed publicly
  inside XRLinuxDriver releases (v2.8.1+ split the libs into their own archive:
  [releases](https://github.com/wheaney/XRLinuxDriver/releases)).
- The **publicly documented** Linux SDK page still only describes the older 3DoF
  IMU API for One/Pro glasses
  ([first.viture.com/developer/viture-sdk-for-linux](https://first.viture.com/developer/viture-sdk-for-linux)).
  The Carina 6DoF API appears to be distributed through the developer program /
  partner channel rather than a public download. `sdk.viture.com` does not resolve and
  `github.com/vitureryan/viture-sdk` returns 404.
- **Hand tracking is not exposed** in the desktop SDK — only 6DoF pose, raw IMU,
  VSync, and stereo camera frames. The Unity XR SDK (6DoF + hand tracking) targets the
  **Pro Neckband only** ([viture.com/developer](https://www.viture.com/developer)).
- **Web has no official SDK.** WebHID-based 3DoF works for One-through-Luma-Pro via a
  reverse-engineered HID protocol (`reference/viture-webxr-extension/VITURE_PROTOCOL.md`),
  but that document does not cover the Luma Ultra PIDs — whether the Ultra streams the
  same 3DoF HID packets is an open item to verify during phase 1.
- As of late 2025, Linux support for the Ultra was described as "waiting on a proper
  SDK update from Viture" ([XRLinuxDriver issue #105](https://github.com/wheaney/XRLinuxDriver/issues/105),
  Dec 2025); the current XRLinuxDriver tree shows that update has since landed.

### 1.3 Platform summary

| Platform | 3DoF IMU | 6DoF pose | Camera frames | Hand tracking |
|---|---|---|---|---|
| Linux (native SDK) | Yes | **Yes (Luma Ultra, host-side VIO)** | Yes (stereo, via callback) | No |
| Windows/macOS (native SDK + SpaceWalker) | Yes | Yes | Advertised | SpaceWalker only |
| Android (SDK / SpaceWalker) | Yes | Yes (Ultra) | Advertised | SpaceWalker only |
| Web (WebHID) | Community reverse-engineered (Ultra unverified) | No | No | No |
| OpenXR (Monado) | No VITURE driver exists as of Monado 25.1.0 ([GamingOnLinux, Dec 2025](https://www.gamingonlinux.com/2025/12/open-source-xr-runtime-monado-25-1-0-released-with-expanded-hardware-support/)) | — | — | — |

WebXR on desktop Linux is a dead end for now: Chromium's WebXR/OpenXR backend is
Windows/Android-only and D3D11-bound ([chromium device/vr docs](https://chromium.googlesource.com/chromium/src/+/HEAD/device/vr/openxr/)),
with only unofficial patches ([mrxz/webxr-linux](https://github.com/mrxz/webxr-linux)).

### 1.4 Prior art / competitive landscape

- **[Breezy Desktop](https://github.com/wheaney/breezy-desktop)** (GPL-3 + paid license
  tiers; 719 stars) — the Linux gold standard. Multiple virtual monitors on
  **GNOME 45-50 Wayland** (KDE 6 Wayland also; no X11 virtual displays). 3DoF in Basic
  ($10/yr), **6DoF for Luma Ultra in Pro ($20/yr, $50 lifetime)**. Features worth
  copying: Smooth Follow, automatic recentering, side-by-side 3D support, lean-in zoom
  (their 6DoF pitch).
- **[XRLinuxDriver](https://github.com/wheaney/XRLinuxDriver)** (GPL-3) — userspace
  driver underlying Breezy. Plugin architecture (`smooth_follow`, `sideview`,
  `virtual_display`, `breezy_desktop`, `opentrack` in `include/plugins/`), outputs
  mouse/joystick/external IPC. Its `imu_pose_t` carries orientation + position with
  `has_position`. Local clone: `reference/XRLinuxDriver`.
- **[SpaceWalker Desktop](https://www.viture.com/academy/spacewalker/desktop)**
  (official, Windows/macOS + [iOS](https://apps.apple.com/us/app/spacewalker-by-viture/id6450915765)/[Android](https://play.google.com/store/apps/details?id=com.viture.spacewalker&hl=en_US))
  — multi-screen with layout switching, ultrawide, 1-click 2D-to-3D, screens locked in
  space (6DoF on Luma Ultra), Pin Mode (3DoF). **No Linux version** — this is the gap
  phase 2 fills.
- **[Xreal Nebula](https://community.xreal.com/t/nebula-for-window-v0-7-0-released/5133)**
  — up to 3 virtual screens or one ultrawide curved screen at 90 Hz on Windows/Mac.
  The de-facto feature bar for consumer AR screen tools.
- **[Immersed](https://immersed.com/faq)** — Linux agent exists but virtual displays
  only work on GNOME Wayland; X11 needs manual xrandr hacks
  ([guide](https://github.com/augustoicaro/Immersed-Linux-Virtual-Monitors)). VR-headset
  oriented, not tethered-glasses oriented.
- **Community projects** ([github.com/topics/viture](https://github.com/topics/viture)):
  [mgschwan/viture_virtual_display](https://github.com/mgschwan/viture_virtual_display)
  (MIT; local clone in `reference/` — OpenGL head-tracked screen showing a Wayland
  screencast or HDMI-in via V4L2, 3DoF), EasyVXR (simplified C lib), uxspace
  (Android/Windows spatial workspace), decky-XRGaming (Steam Deck).
- **Reviewer sentiment** ([Geeking Out review](https://www.geekingout.ca/viture-luma-ultra-review-finally-spatial-computing-worth-hype)):
  display quality universally praised; "6DoF optimization still evolving"; full spatial
  feature set gated behind SpaceWalker/neckband. Linux users on r/viture and the
  XRLinuxDriver tracker were vocal through late 2025 about the Ultra being underserved
  outside Breezy ([issue #105](https://github.com/wheaney/XRLinuxDriver/issues/105)) —
  i.e., there is real demand for exactly this project.

---

## 2. Recommended architecture

### Options considered

**Option A — Stay web: WebHID/WebSocket bridge + Three.js fullscreen renderer.**
Phase 1's stack, extended: a tiny native daemon (or XRLinuxDriver itself) reads 6DoF
pose from the VITURE SDK and republishes over WebSocket; a browser window fullscreened
on the glasses' extended display renders the 3D scene; desktop content is captured via
`getDisplayMedia` (PipeWire portal works in Chromium on Wayland).
*Pros:* maximum reuse of phase-1 code and skills; fastest UX iteration; WebHID gives a
zero-install 3DoF fallback. *Cons:* WebXR is unavailable on Linux so this is "flat
fullscreen 3D", not a real XR pipeline; browser compositing adds a frame or more of
latency and typically caps at the compositor's cadence — head-locked judder at 120 Hz
is likely; no low-level control of scanout timing/VSync (the SDK's VSync callback and
pose prediction can't be exploited properly); Ultra's 6DoF can never come from WebHID
(VIO is host-side, closed).

**Option B — Native standalone spatial renderer (recommended).**
A single native app (Rust preferred; C/C++ also fine given the reference code) that:
1. Gets 6DoF pose directly from the VITURE SDK (`get_gl_pose_carina` with prediction,
   `reset_pose_carina` for recenter) — or, as a fallback/compat layer, from
   XRLinuxDriver's IPC so older 3DoF VITURE models work too.
2. Captures monitors/windows via **PipeWire + xdg-desktop-portal** (works on GNOME,
   KDE, and Pop!_OS COSMIC — any Wayland compositor; X11 fallback via XSHM capture).
3. Opens an exclusive fullscreen Vulkan/OpenGL surface on the glasses' extended
   display (the glasses are just a 1920x1080/1200 monitor via DP alt mode) and renders
   the captured textures as quads/curved meshes positioned in world space, reprojected
   every frame from the latest predicted pose.
*Pros:* full control of latency (pose prediction + render at 120 Hz + VSync callback);
desktop-environment-agnostic — notably **works on Pop!_OS, where Breezy does not**
(Breezy requires GNOME 45+/KDE 6; Pop!_OS 22.04 ships GNOME 42, and COSMIC is neither);
no license fee; mgschwan's MIT-licensed `viture_virtual_display` is a working skeleton
of exactly this design (capture -> GL quad -> head tracking) to learn from.
*Cons:* more up-front work than A; screens are rendered copies (capture latency for
video/gaming content); must handle multi-DE quirks ourselves.

**Option C — Integrate with the existing ecosystem (Breezy plugin / Monado driver).**
Either build features into Breezy Desktop (GPL-3, paid-tier licensing, GNOME/KDE-bound)
or write a VITURE driver for Monado and build an OpenXR overlay client (e.g., the
wlx-overlay-s / xrdesktop family).
*Pros:* leverages mature code; a Monado driver would unlock the whole OpenXR ecosystem
(and eventually WebXR if Chromium ever ships Linux OpenXR). *Cons:* Breezy's licensing
and DE constraints conflict with a Pop!_OS user's needs and with shipping our own tool;
a Monado driver means wrapping closed-source VIO libs inside a
permissively-licensed runtime (legal/technical friction) or reimplementing VIO from
the exposed stereo frames + IMU — months of work; Monado has no VITURE support today.

### Recommendation

**Option B: a native Rust (or C) standalone renderer, linking the VITURE closed SDK
for 6DoF, PipeWire for capture, Vulkan/GL fullscreen output on the glasses.**

Rationale: the closed SDK is the only source of Ultra 6DoF and it is a C ABI —
trivially callable from native, impossible from the browser. The rendering path is the
whole product (latency, prediction, 120 Hz), and only native gives control over it.
Pop!_OS compatibility rules out the Breezy/GNOME path for the owner's own daily use.
Keep phase 1's web visualizer as the debug/telemetry UI: the native app should expose
the same WebSocket telemetry (pose, tracking state, FPS) so the phase-1 dashboard
becomes phase 2's monitoring console — the web investment is not thrown away.

Also keep Option C's Monado driver on the long-term horizon (V3+): it is the "right"
home for this hardware in the Linux ecosystem, and our SDK-wrapping experience from
phase 2 is exactly what that driver needs.

---

## 3. Feature roadmap

### MVP — one screen, anchored in space

- Detect Luma Ultra over USB (VID `0x35ca`, PID `0x1101`/`0x1104`); init SDK; start VIO.
- **6DoF anchoring**: one virtual screen, world-locked (position + orientation), with
  graceful degradation to 3DoF (orientation-only) when VIO is lost or on older glasses.
- **Widescreen preset**: single flat 16:9 screen at configurable distance (default
  ~1.5-2 m) and size (default ~100-150 in equivalent).
- Capture one monitor via PipeWire portal and texture it onto the screen.
- Fullscreen output on the glasses display; render loop driven by predicted pose
  (`get_gl_pose_carina` with prediction ≈ display latency) at panel refresh.
- **Recenter hotkey** (global shortcut -> `reset_pose_carina` + re-place screen in
  front of user).
- Basic config file (distance, size, capture source, hotkeys).
- Telemetry over WebSocket to the phase-1 web dashboard (pose, tracking quality, FPS).

### V2 — multi-screen workspace

- **Multiple screens** (target 3, like SpaceWalker/Nebula), each with its own capture
  source (monitor or window).
- **Presets**: ultrawide (single stretched virtual display curved around the user),
  curved panels, triple-panel cockpit, stacked (main + reference above).
- **Curved screen geometry** (cylindrical section meshes, adjustable radius).
- **Layout save/restore**: named layouts persisted to disk; auto-restore last layout
  per capture configuration.
- **Follow vs anchor modes** per screen: world-anchored (6DoF), body-follow ("lazy
  follow" with smoothing a la Breezy's Smooth Follow), head-locked (HUD).
- **Distance & size controls**: keyboard/hotkey and small control UI (reuse phase-1 web
  UI as a control panel served locally — change layout from phone or second monitor).
- Recentering improvements: auto-recenter when tracking re-acquires; "snap to gaze"
  placement of new screens.
- 3DoF-model support via XRLinuxDriver IPC or the documented HID protocol, so One/Pro/
  Luma users get the same tool minus positional tracking.
- Per-screen brightness/opacity dimming of unfocused screens.

### V3+ — spatial workspace, ecosystem plays

- **Hand-tracking interactions** — *speculative*: not exposed by the desktop SDK. Two
  routes: (a) VITURE ships desktop hand tracking later; (b) DIY on the exposed stereo
  camera frames (`XRCameraCallback`) with an off-the-shelf hand landmark model.
  Prototype only after core is stable.
- **Per-app screens**: one virtual screen per application window (portal window
  capture), with a picker to "send window to space".
- **Multi-monitor capture**: replicate a full physical multi-monitor rig spatially;
  headless/virtual output creation on Wayland (compositor-specific; GNOME and wlroots
  have paths, COSMIC TBD) so screens exist without physical monitors.
- **Comfort features**: head-motion smoothing filters, neck-saver repositioning
  (XRLinuxDriver has a `neck_saver` plugin to study), auto distance-based dimming,
  edge blending/vignetting between adjacent screens, snap-turn recenter.
- **Edge blending** between adjacent panels in ultrawide composite mode.
- SBS 3D output mode (glasses' native 3840x1080 SBS; `switch_dimension` API) for true
  stereo rendering of the scene — real depth separation between screens.
- **Monado driver upstream contribution** for VITURE devices -> OpenXR apps and
  future-proofing.

---

## 4. Risks and open questions

1. **SDK licensing/redistribution.** The Carina SDK is closed-source and appears to be
   distributed via partner channels (bundled in XRLinuxDriver releases). Can we ship it
   with our app? Action: email VITURE developer relations (developer page has contact)
   and/or ask wheaney how the collaboration works. Fallback: instruct users to install
   XRLinuxDriver and consume its IPC (GPL-3 boundary: talk over IPC, don't link).
2. **Is the Carina API stable and publicly obtainable?** The public Linux SDK page
   still documents only the old 3DoF API ([first.viture.com](https://first.viture.com/developer/viture-sdk-for-linux)).
   Version churn risk is real (Luma Ultra support landed in XRLinuxDriver only in early
   2026 after months of waiting — [issue #105](https://github.com/wheaney/XRLinuxDriver/issues/105)).
3. **VIO quality and host cost.** SLAM runs on the host CPU (OpenCV-based
   `libcarina_vio.so`): what is the CPU load, drift behavior, and re-localization
   latency on Salvatore's machine? Reviewers note 6DoF is "still evolving"
   ([Geeking Out](https://www.geekingout.ca/viture-luma-ultra-review-finally-spatial-computing-worth-hype)).
   Needs an early empirical spike.
4. **USB bandwidth.** Stereo camera frames + IMU stream over USB while DP alt mode
   carries video. Alt mode uses dedicated lanes, but hubs/docks and USB2-only cables
   could starve the camera stream and kill 6DoF. Test direct connection vs docks.
5. **Luma Ultra over WebHID (phase-1 risk).** The community HID protocol doc does not
   list Ultra PIDs; the WebXR extension explicitly supports only One-through-Luma-Pro.
   Phase 1 must verify whether the Ultra emits the classic `0xFF 0xFC` IMU packets.
   If not, phase 1's visualizer needs the same native bridge planned for phase 2.
6. **120 Hz + 1200p on Linux.** Verify the glasses' EDID modes are all usable
   (community repos exist to fix mode issues on the Beast — [viture-beast-linux](https://github.com/topics/viture)).
   Also verify SBS 3840x1080 mode switching from the SDK works on Linux.
7. **Capture latency and protected content.** PipeWire screencast adds ~1-2 frames;
   fine for productivity, marginal for gaming. DRM-protected surfaces won't capture.
8. **Wayland fragmentation.** Portal-based capture is portable, but virtual/headless
   output creation (V3) is compositor-specific; COSMIC's story is immature.
9. **Hand tracking** may never be exposed on desktop — treat all gesture UX as
   optional garnish, never a dependency.
10. **Competing with Breezy.** Breezy Pro already does 6DoF virtual desktop on
    GNOME/KDE. Our differentiation: DE-agnostic (Pop!_OS/COSMIC), free/open,
    preset-driven "spatial screens" UX, and the phase-1 web telemetry angle. Consider
    collaborating (XRLinuxDriver plugins) rather than duplicating where possible.

---

## 5. Concrete next milestones (after the phase-1 sensor visualizer)

1. **M0 — Ultra HID audit (finishes phase 1).** Confirm whether Luma Ultra streams
   3DoF IMU over plain HID (PIDs `0x1101`/`0x1104`); extend
   `reference/viture-webxr-extension/VITURE_PROTOCOL.md` findings; document in the
   phase-1 tool. Exit criteria: phase-1 visualizer shows live orientation from the
   Ultra, or a documented "needs native bridge" conclusion.
2. **M1 — 6DoF pose spike (1-2 days).** Tiny native program linking the XRLinuxDriver-
   bundled VITURE libs: init provider for the Ultra, print `get_gl_pose_carina` at
   100 Hz, exercise `reset_pose_carina`. Measure: CPU load of VIO, drift over 10 min,
   behavior when cameras are covered. Also pipe pose into the phase-1 dashboard over
   WebSocket (this doubles as the visualizer's 6DoF upgrade).
3. **M2 — Renderer spike.** Fullscreen GL/Vulkan window on the glasses output rendering
   a fixed test quad, world-locked using M1 pose with prediction; measure motion-to-
   photon subjectively at 60 vs 120 Hz; verify VSync callback utility.
4. **M3 — MVP assembly.** PipeWire monitor capture textured onto the quad + widescreen
   preset + recenter hotkey + config file. This is the "place one screen in space"
   deliverable.
5. **M4 — Preset & layout engine (start of V2).** Screen abstraction (n screens, flat/
   curved geometry, per-screen anchor mode), ultrawide preset, layout save/restore.
6. **M5 — Outreach checkpoint.** Contact VITURE devrel re: SDK redistribution; sync
   with wheaney (XRLinuxDriver discussions) to avoid duplicated effort and explore
   contributing DE-agnostic pieces upstream.

---

### Source index

- VITURE developer program: https://www.viture.com/developer
- VITURE Linux SDK (public, 3DoF-era docs): https://first.viture.com/developer/viture-sdk-for-linux
- Luma Ultra product page: https://www.viture.com/product/viture-luma-ultra-xr-glasses
- Luma series comparison: https://www.viture.com/luma and https://www.viture.com/blog/the-ultimate-guide-to-choosing-your-new-viture-glasses-luma-pro-luma-ultra-luma-and-the-beast
- SpaceWalker desktop: https://www.viture.com/academy/spacewalker/desktop
- XRLinuxDriver: https://github.com/wheaney/XRLinuxDriver (local: `reference/XRLinuxDriver`)
- Luma Ultra Linux support thread: https://github.com/wheaney/XRLinuxDriver/issues/105
- Breezy Desktop: https://github.com/wheaney/breezy-desktop
- Monado 25.1.0 (no VITURE driver): https://www.gamingonlinux.com/2025/12/open-source-xr-runtime-monado-25-1-0-released-with-expanded-hardware-support/
- WebXR-on-Linux status: https://github.com/mrxz/webxr-linux and https://chromium.googlesource.com/chromium/src/+/HEAD/device/vr/openxr/
- Xreal Nebula feature bar: https://community.xreal.com/t/nebula-for-window-v0-7-0-released/5133
- Immersed Linux status: https://immersed.com/faq
- Community VITURE projects: https://github.com/topics/viture
- Luma Ultra review (6DoF maturity): https://www.geekingout.ca/viture-luma-ultra-review-finally-spatial-computing-worth-hype
- Local ground truth: `reference/XRLinuxDriver/include/sdks/viture_device_carina.h`,
  `reference/XRLinuxDriver/src/devices/viture.c`,
  `reference/viture-webxr-extension/VITURE_PROTOCOL.md`,
  `reference/viture_virtual_display/README.md`
