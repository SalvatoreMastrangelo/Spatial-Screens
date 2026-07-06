import pytest

from classify import (
    INDEX_PIP, INDEX_TIP, MIDDLE_MCP, MIDDLE_PIP, MIDDLE_TIP,
    PINKY_PIP, PINKY_TIP, RING_PIP, RING_TIP, THUMB_TIP, WRIST,
    classify_pose, pinch_norm, pinch_pos, select_hand,
)


def make_landmarks(overrides):
    lm = [(0.0, 0.0)] * 21
    for idx, pos in overrides.items():
        lm[idx] = pos
    return lm


OPEN_PALM = make_landmarks({
    WRIST: (0.5, 0.9),
    MIDDLE_MCP: (0.5, 0.6),
    INDEX_PIP: (0.45, 0.5), INDEX_TIP: (0.45, 0.2),
    MIDDLE_PIP: (0.50, 0.5), MIDDLE_TIP: (0.50, 0.15),
    RING_PIP: (0.55, 0.5), RING_TIP: (0.55, 0.2),
    PINKY_PIP: (0.60, 0.55), PINKY_TIP: (0.60, 0.3),
    THUMB_TIP: (0.3, 0.6),
})

FIST = make_landmarks({
    WRIST: (0.5, 0.9),
    MIDDLE_MCP: (0.5, 0.6),
    INDEX_PIP: (0.48, 0.65), INDEX_TIP: (0.48, 0.78),
    MIDDLE_PIP: (0.50, 0.65), MIDDLE_TIP: (0.50, 0.78),
    RING_PIP: (0.52, 0.65), RING_TIP: (0.52, 0.78),
    PINKY_PIP: (0.54, 0.68), PINKY_TIP: (0.54, 0.80),
    THUMB_TIP: (0.45, 0.75),
})

POINT = make_landmarks({
    WRIST: (0.5, 0.9),
    MIDDLE_MCP: (0.5, 0.6),
    INDEX_PIP: (0.45, 0.5), INDEX_TIP: (0.45, 0.2),      # extended
    MIDDLE_PIP: (0.50, 0.65), MIDDLE_TIP: (0.50, 0.78),  # curled
    RING_PIP: (0.52, 0.65), RING_TIP: (0.52, 0.78),      # curled
    PINKY_PIP: (0.54, 0.68), PINKY_TIP: (0.54, 0.80),    # curled
    THUMB_TIP: (0.4, 0.7),
})

# Neither fist (all curled), open_palm (none curled), nor point (only index
# extended) — index and middle extended, ring and pinky curled. Exercises
# classify_pose's fallthrough "none" branch.
MIXED_CURL = make_landmarks({
    WRIST: (0.5, 0.9),
    MIDDLE_MCP: (0.5, 0.6),
    INDEX_PIP: (0.45, 0.5), INDEX_TIP: (0.45, 0.2),      # extended
    MIDDLE_PIP: (0.50, 0.5), MIDDLE_TIP: (0.50, 0.15),   # extended
    RING_PIP: (0.55, 0.65), RING_TIP: (0.55, 0.78),      # curled
    PINKY_PIP: (0.60, 0.68), PINKY_TIP: (0.60, 0.80),    # curled
    THUMB_TIP: (0.35, 0.6),
})


def test_open_palm_classified_correctly():
    assert classify_pose(OPEN_PALM) == "open_palm"


def test_fist_classified_correctly():
    assert classify_pose(FIST) == "fist"


def test_point_classified_correctly():
    assert classify_pose(POINT) == "point"


def test_mixed_curl_classified_as_none():
    assert classify_pose(MIXED_CURL) == "none"


def test_pinch_norm_small_when_fingers_touching():
    touching = make_landmarks({
        WRIST: (0.5, 0.9), MIDDLE_MCP: (0.5, 0.6),
        THUMB_TIP: (0.5, 0.5), INDEX_TIP: (0.51, 0.5),
    })
    assert pinch_norm(touching) == pytest.approx(0.01 / 0.3)


def test_pinch_norm_large_when_fingers_apart():
    apart = make_landmarks({
        WRIST: (0.5, 0.9), MIDDLE_MCP: (0.5, 0.6),
        THUMB_TIP: (0.2, 0.5), INDEX_TIP: (0.8, 0.5),
    })
    assert pinch_norm(apart) == pytest.approx(2.0)


def test_pinch_pos_is_midpoint_of_tips():
    lm = make_landmarks({THUMB_TIP: (0.2, 0.4), INDEX_TIP: (0.4, 0.6)})
    assert pinch_pos(lm) == pytest.approx((0.3, 0.5))


# select_hand distinguishes hands by SPATIAL wrist x (landmark 0), NOT
# MediaPipe's handedness label (which flips unreliably between the two stereo
# cameras). _LMA's wrist is at image-left x=0.1; _LMB's at image-right x=0.9.
_LMA = [(0.1, 0.1)] * 21   # wrist image-left
_LMB = [(0.9, 0.9)] * 21   # wrist image-right


def test_select_hand_spatial_no_mirror():
    hands = [("ignored", _LMA), ("ignored", _LMB)]
    assert select_hand(hands, "left", mirror=False) is _LMA   # image-left = user-left
    assert select_hand(hands, "right", mirror=False) is _LMB


def test_select_hand_spatial_mirror():
    # Forward-facing camera: the image-left hand is the user's RIGHT.
    hands = [("ignored", _LMA), ("ignored", _LMB)]
    assert select_hand(hands, "right", mirror=True) is _LMA
    assert select_hand(hands, "left", mirror=True) is _LMB


def test_select_hand_ignores_mediapipe_label():
    # Labels deliberately contradict position; position must win.
    hands = [("Right", _LMA), ("Left", _LMB)]
    assert select_hand(hands, "left", mirror=False) is _LMA
    assert select_hand(hands, "right", mirror=False) is _LMB


def test_select_hand_lone_hand_matches_one_side_only():
    # The "same hand on both sides" fix: a single image-left hand is the
    # user-left only; requesting the user-right yields None.
    hands = [("ignored", _LMA)]  # image-left, x=0.1
    assert select_hand(hands, "left", mirror=False) is _LMA
    assert select_hand(hands, "right", mirror=False) is None


def test_select_hand_empty_returns_none():
    assert select_hand([], "left") is None
    assert select_hand([], "right") is None
