// Per-eye helpers for SBS stereo. Model: two parallel per-eye panels with
// ~100% overlap -> cameras offset ±IPD/2 along head-local x, PARALLEL
// symmetric frusta (no toe-in, no off-axis shift). Infinity fuses at zero
// disparity. See docs/specs/2026-07-05-stereo-3d-design.md §2.
#pragma once
#include <cmath>
#include <cstdint>

// View-space x offset to ADD to a view (or head-locked HUD) translation.
// Eye 0 = left: camera at -ipd/2 => world shifts +ipd/2 in view space.
inline float stereo_eye_offset(float ipd_m, int eye) {
    return (eye == 0 ? 0.5f : -0.5f) * ipd_m;
}

// Frustum half-extents at the near plane for one eye: the glasses' 52°
// diagonal FOV applies per eye (each eye is a full 1920x1200 panel).
inline void stereo_eye_frustum(uint32_t eye_w, uint32_t eye_h, float diag_fov_deg,
                               float near_z, float& r, float& t) {
    float diag_px = std::sqrt(float(eye_w) * float(eye_w) + float(eye_h) * float(eye_h));
    float half = std::tan(diag_fov_deg * float(M_PI) / 360.f);
    r = half * float(eye_w) / diag_px * near_z;
    t = half * float(eye_h) / diag_px * near_z;
}
