from hand_tracker import drain_to_latest, fuse_depths, match_right_hand


def _hand(x0, y0):
    # 21 landmarks in a small diagonal cluster.
    return [(x0 + i * 0.004, y0 + i * 0.003) for i in range(21)]


def _shift(lm, dx, dy=0.0):
    return [(x + dx, y + dy) for x, y in lm]


def test_match_right_hand_picks_positive_disparity_same_row():
    left = _hand(0.60, 0.50)
    match = _shift(left, -0.05)              # xr = xl - 0.05  (disparity +0.05), same row
    decoy = _shift(left, -0.05, dy=0.40)     # right row => far off
    got = match_right_hand(left, [("a", decoy), ("b", match)])
    assert got == match


def test_match_right_hand_rejects_negative_disparity():
    left = _hand(0.60, 0.50)
    diverged = _shift(left, +0.05)           # xr > xl => disparity < 0
    assert match_right_hand(left, [("a", diverged)]) is None


def test_match_right_hand_empty_returns_none():
    assert match_right_hand(_hand(0.5, 0.5), []) is None


def test_fuse_depths_matched_left_only():
    left = _hand(0.60, 0.50)
    match = _shift(left, -0.05)
    out = fuse_depths({"left": left, "right": None}, [("x", match)])
    assert out["left"] is not None and out["left"] > 0
    assert out["right"] is None


def test_fuse_depths_no_right_hands():
    left = _hand(0.60, 0.50)
    out = fuse_depths({"left": left, "right": None}, [])
    assert out == {"left": None, "right": None}


def test_drain_to_latest_returns_newest():
    frames = iter([("f1",), ("f2",), ("f3",), None])
    readable = iter([True, True, False])
    assert drain_to_latest(lambda: next(frames), lambda: next(readable, False)) == ("f3",)


def test_drain_to_latest_single_when_not_readable():
    frames = iter([("f1",)])
    assert drain_to_latest(lambda: next(frames, None), lambda: False) == ("f1",)


def test_drain_to_latest_eof_returns_none():
    assert drain_to_latest(lambda: None, lambda: False) is None
