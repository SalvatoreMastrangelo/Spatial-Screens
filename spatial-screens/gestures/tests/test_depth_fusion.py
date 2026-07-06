import math

from depth_fusion import (
    BASELINE_M, MIN_VALID_LANDMARKS, focal_norm, robust_depth, triangulate,
)


def _project(X, Y, Z, hfov_deg=70.0, baseline_m=BASELINE_M):
    """Forward model: where a 3D point (left-camera frame) lands in each
    normalized image. Left cam at origin, right cam at +baseline along X."""
    f = focal_norm(hfov_deg)
    xl = X * f / Z + 0.5
    yl = Y * f / Z + 0.5
    xr = (X - baseline_m) * f / Z + 0.5
    return xl, yl, xr, yl  # rectified: right shares the row (yr == yl)


def test_triangulate_recovers_known_point():
    xl, yl, xr, yr = _project(0.10, -0.05, 0.60)
    p = triangulate(xl, yl, xr, yr)
    assert p is not None
    X, Y, Z = p
    assert abs(Z - 0.60) < 1e-3
    assert abs(X - 0.10) < 1e-3
    assert abs(Y - (-0.05)) < 1e-3


def test_nearer_point_has_larger_disparity_and_smaller_Z():
    near = _project(0.0, 0.0, 0.30)
    far = _project(0.0, 0.0, 0.90)
    disp_near = near[0] - near[2]
    disp_far = far[0] - far[2]
    assert disp_near > disp_far          # nearer => bigger disparity
    assert triangulate(*near)[2] < triangulate(*far)[2]


def test_triangulate_nonpositive_disparity_returns_none():
    # xr >= xl => disparity <= 0 (diverged / behind).
    assert triangulate(0.5, 0.5, 0.5, 0.5) is None
    assert triangulate(0.4, 0.5, 0.6, 0.5) is None


def test_robust_depth_medians_valid_landmarks():
    pts_l, pts_r = [], []
    for i in range(21):
        xl, yl, xr, yr = _project(0.01 * i, 0.0, 0.50)
        pts_l.append((xl, yl))
        pts_r.append((xr, yr))
    d = robust_depth(pts_l, pts_r)
    assert d is not None
    assert abs(d - 0.50) < 1e-2


def test_robust_depth_too_few_valid_returns_none():
    # All pairs have zero disparity => none valid.
    pts = [(0.5, 0.5)] * 21
    assert robust_depth(pts, pts) is None
