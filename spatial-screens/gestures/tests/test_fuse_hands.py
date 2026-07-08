from fuse_hands import (nominal_disparity, build_fused_hands, assign_hands,
                        fuse_and_assign)

# 21-point hand at a given wrist x; y rows ~identical across the pair (rectified).
def hand(x, y=0.5):
    return [(x + 0.001 * i, y + 0.0005 * i) for i in range(21)]

D0 = nominal_disparity()


def test_nominal_disparity_positive_small():
    assert 0.0 < D0 < 0.3


def test_hand_in_both_is_one_fused_with_depth():
    # left sees it at x=0.55, right at x=0.44 (positive disparity => real point)
    left = [("L", hand(0.55))]
    right = [("R", hand(0.44))]
    fused = build_fused_hands(left, right, D0)
    assert len(fused) == 1
    assert fused[0]["depth"] is not None  # triangulated


def test_left_only_hand_no_depth_unshifted():
    left = [("L", hand(0.30))]
    fused = build_fused_hands(left, [], D0)
    assert len(fused) == 1 and fused[0]["depth"] is None
    assert abs(fused[0]["landmarks"][0][0] - 0.30) < 1e-9  # not shifted


def test_right_only_hand_shifted_into_left_frame():
    right = [("R", hand(0.30))]
    fused = build_fused_hands([], right, D0)
    assert len(fused) == 1 and fused[0]["depth"] is None
    assert abs(fused[0]["landmarks"][0][0] - (0.30 + D0)) < 1e-9  # +d0 shift


def test_lone_hand_in_both_does_not_double():
    # same physical hand, positive disparity => must dedup to ONE
    left = [("L", hand(0.52))]
    right = [("R", hand(0.42))]
    assert len(build_fused_hands(left, right, D0)) == 1


def test_assign_hands_splits_by_x_mirror_false():
    lo = {"landmarks": hand(0.2), "depth": None}
    hi = {"landmarks": hand(0.8), "depth": None}
    left, right = assign_hands([lo, hi], mirror=False)
    assert left is lo and right is hi  # user-left = image-left when not mirrored


def test_fuse_and_assign_end_to_end():
    left_hands = [("L", hand(0.25))]           # left-image hand, user-left side
    right_hands = [("R", hand(0.75))]          # right-image-only hand, user-right side
    left, right = fuse_and_assign(left_hands, right_hands, D0, mirror=False)
    assert left is not None and right is not None
