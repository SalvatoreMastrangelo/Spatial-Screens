# Vendored VITURE XR Glasses SDK (v2.0.0)

This directory contains the **official unified VITURE XR Glasses SDK v2.0.0**
(2025, C/C++ API) for Linux x86_64 — the current-generation SDK that supports
the whole lineup including **Luma Ultra** (device type `XR_DEVICE_TYPE_VITURE_CARINA`,
PIDs `0x1101` / `0x1104`). This is *not* the legacy `viture_one_sdk` v1.x,
which was 3DoF-only and predates the Luma series.

## Contents

- `include/` — public C API headers (`viture_glasses_provider.h`,
  `viture_device.h`, `viture_device_carina.h`, `viture_protocol.h`, …)
- `lib/x86_64/` — prebuilt shared libraries:
  - `libglasses.so` — main SDK entry point (device provider, IMU, display control)
  - `libcarina_vio.so` — visual-inertial odometry (6DoF SLAM) engine used for Luma Ultra
  - `libopencv_*.so.4.2` — bundled OpenCV runtime used by the VIO engine

## What the SDK exposes for Luma Ultra (Carina)

- **6DoF pose**: position (x,y,z) + quaternion (w,x,y,z) via
  `register_callbacks_carina()` pose callback or `get_gl_pose_carina()`
  (OpenGL/EUS coordinates: x→right, y→up, z→backward), with pose prediction.
- **Raw IMU** callback (gyroscope, accelerometer, magnetometer, temperature).
- **VSync** timestamps and **stereo camera frame** callbacks.
- **Device control/state**: brightness, volume, display mode (incl. 3840×1200
  SBS modes), electrochromic film, firmware version, market name.
- `reset_pose_carina()` to re-origin the 6DoF tracking.

## Provenance

Copied from the `wheaney/XRLinuxDriver` repository (which redistributes the
official VITURE SDK binaries for its VITURE support), commit
`08b3bc93d1dff5f8843be30fbc262fb5364f7fa3` (2026-04-02):

- headers: `XRLinuxDriver/include/sdks/viture_*.h`
- libraries: `XRLinuxDriver/lib/x86_64/viture/`

Canonical source: <https://www.viture.com/developer> ("VITURE XR Glasses SDK",
Android/Linux/Windows/macOS). Binaries are © VITURE Inc., all rights reserved;
they are vendored here unmodified for local development only.

## Linking notes

`libcarina_vio.so` resolves its OpenCV deps from this same directory, so link
executables with an rpath that covers transitive loads (classic `DT_RPATH`):

```
-L sdk/lib/x86_64 -lglasses -Wl,--disable-new-dtags,-rpath,'$ORIGIN/../sdk/lib/x86_64'
```

Device access requires udev permissions for VID `35ca` (see `bridge/70-viture-xr.rules`).
