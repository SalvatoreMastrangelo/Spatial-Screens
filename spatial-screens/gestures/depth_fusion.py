"""Stereo depth fusion for the gesture sidecar — pure math, no MediaPipe/I/O,
fully unit-testable with synthetic coordinates.

Recovers a hand's depth (meters, rough-scaled) by triangulating the same
landmark seen in the two tracking cameras. The VITURE SDK exposes NO stereo
calibration, so v1 ASSUMES a nominally-rectified horizontal stereo pair (same
intrinsics, parallel optical axes): focal length from an assumed FOV, a fixed
baseline. Depth is reliable near/far but not trustworthy metric cm — see
docs/specs/2026-07-06-camera-fusion-depth-design.md. The checkerboard upgrade
replaces this assumed model with no change to callers.

Image coordinates are MediaPipe normalized [0,1] (x right, y down).
"""
import math
import statistics

ASSUMED_HFOV_DEG = 70.0    # tracking-camera horizontal FOV (approx; tune on hw)
BASELINE_M = 0.06          # ~6 cm inter-camera baseline (two-hand hardware pass)
MIN_DISPARITY = 1e-3       # normalized-x units; at/below this => invalid
MIN_VALID_LANDMARKS = 6    # need this many valid points for a robust median


def focal_norm(hfov_deg=ASSUMED_HFOV_DEG):
    """Focal length in normalized-x units for an assumed horizontal FOV
    (pinhole: half-width 0.5 subtends half the FOV)."""
    return 0.5 / math.tan(math.radians(hfov_deg) / 2.0)


def triangulate(xl, yl, xr, yr, hfov_deg=ASSUMED_HFOV_DEG, baseline_m=BASELINE_M):
    """Triangulate one landmark from its normalized positions in the left/right
    rectified images. Returns (X, Y, Z) meters in the left-camera frame, or
    None if disparity <= MIN_DISPARITY (diverged / behind).

    Left cam at origin, right cam at +baseline along X: disparity
    d = xl - xr = f*B/Z, so Z = f*B/d."""
    d = xl - xr
    if d <= MIN_DISPARITY:
        return None
    f = focal_norm(hfov_deg)
    Z = f * baseline_m / d
    X = (xl - 0.5) * Z / f
    Y = (yl - 0.5) * Z / f
    return (X, Y, Z)


def robust_depth(pts_left, pts_right, hfov_deg=ASSUMED_HFOV_DEG, baseline_m=BASELINE_M):
    """Median Z over all index-aligned landmark pairs that triangulate validly.
    Returns meters, or None if fewer than MIN_VALID_LANDMARKS are valid.

    pts_left/pts_right are equal-length lists of (x, y) normalized pairs — the
    SAME landmark index in each image."""
    zs = []
    for (xl, yl), (xr, yr) in zip(pts_left, pts_right):
        p = triangulate(xl, yl, xr, yr, hfov_deg, baseline_m)
        if p is not None:
            zs.append(p[2])
    if len(zs) < MIN_VALID_LANDMARKS:
        return None
    return statistics.median(zs)
