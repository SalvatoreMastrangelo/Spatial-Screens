import pytest

from classify import (
    INDEX_PIP, INDEX_TIP, MIDDLE_MCP, MIDDLE_PIP, MIDDLE_TIP,
    PINKY_PIP, PINKY_TIP, RING_PIP, RING_TIP, THUMB_TIP, WRIST,
    classify_pose, pinch_norm, pinch_pos,
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


def test_open_palm_classified_correctly():
    assert classify_pose(OPEN_PALM) == "open_palm"


def test_fist_classified_correctly():
    assert classify_pose(FIST) == "fist"


def test_point_classified_correctly():
    assert classify_pose(POINT) == "point"


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
