"""Pure cross-camera hand fusion for the gesture sidecar: match a hand across the
stereo pair, dedup a hand seen by both cameras into one entity, canonicalize
right-only detections into the LEFT image frame, and split the fused set into the
user's left/right hands. No MediaPipe/I-O — unit-testable with synthetic
landmarks. See docs/specs/2026-07-08-centered-hand-tracking-design.md."""
import statistics

from classify import WRIST
from depth_fusion import (ASSUMED_HFOV_DEG, BASELINE_M, focal_norm, robust_depth)

NOMINAL_DEPTH_M = 0.4  # assumed hand distance for the right-only disparity shift


def nominal_disparity(hfov_deg=ASSUMED_HFOV_DEG, baseline_m=BASELINE_M,
                      z0=NOMINAL_DEPTH_M):
    """Normalized-x disparity for a hand at the nominal depth. A right-only
    detection is shifted into the left frame by x_left ~= x_right + d0."""
    return focal_norm(hfov_deg) * baseline_m / z0


def match_right_hand(left_lm, right_hands, max_row_delta=0.15):
    """Right-image landmarks of the SAME physical hand as left_lm, or None.
    Smallest mean |dy| with positive mean disparity (xl - xr), within
    max_row_delta. (Moved verbatim from hand_tracker.py.)"""
    best, best_dy = None, max_row_delta
    for _label, rlm in right_hands:
        dy = statistics.mean(abs(l[1] - r[1]) for l, r in zip(left_lm, rlm))
        disp = statistics.mean(l[0] - r[0] for l, r in zip(left_lm, rlm))
        if disp <= 0:
            continue
        if dy < best_dy:
            best, best_dy = rlm, dy
    return best


def _shift_x(landmarks, dx):
    return [(x + dx, y) for (x, y) in landmarks]


def build_fused_hands(left_hands, right_hands, d0):
    """left_hands/right_hands: [(label, landmarks), ...] from the two images.
    Returns fused hands [{"landmarks": canonical [(x,y)...], "depth": m|None}].
    A hand in both images -> one fused hand (deduped) with depth; left-only and
    right-only -> separate, depth None; right-only shifted +d0 into the left
    frame."""
    fused = []
    used = set()  # indices of right detections already matched
    for _label, llm in left_hands:
        candidates = [(i, rh) for i, rh in enumerate(right_hands) if i not in used]
        rlm = match_right_hand(llm, [rh for _i, rh in candidates])
        depth = None
        if rlm is not None:
            for i, (_lbl, r) in candidates:
                if r is rlm:
                    used.add(i)
                    break
            depth = robust_depth(llm, rlm)
        fused.append({"landmarks": llm, "depth": depth})
    for i, (_label, rlm) in enumerate(right_hands):
        if i in used:
            continue
        fused.append({"landmarks": _shift_x(rlm, d0), "depth": None})
    return fused


def assign_hands(fused, mirror):
    """Split fused hands into the user's (left, right) by canonical wrist-x, with
    the same spatial rule as classify.select_hand. Returns (left, right), each a
    fused-hand dict or None."""
    if not fused:
        return None, None
    ordered = sorted(fused, key=lambda f: f["landmarks"][WRIST][0])

    def pick(target):
        want_image_left = (target == "right") if mirror else (target == "left")
        if len(ordered) == 1:
            is_image_left = ordered[0]["landmarks"][WRIST][0] < 0.5
            return ordered[0] if is_image_left == want_image_left else None
        return ordered[0] if want_image_left else ordered[-1]

    return pick("left"), pick("right")


def fuse_and_assign(left_hands, right_hands, d0, mirror):
    return assign_hands(build_fused_hands(left_hands, right_hands, d0), mirror)
