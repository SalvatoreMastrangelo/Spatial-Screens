"""Pure landmark math for gesture classification — no MediaPipe/I/O
dependency, so this is fully unit-testable with synthetic coordinates.

landmarks is a 21-element list of (x, y) tuples in MediaPipe's normalized
image coordinates (origin top-left, y increases downward), indexed per the
standard MediaPipe Hands landmark layout.
"""
import math

# Which image side the user's LEFT hand appears on. MediaPipe's own handedness
# LABEL proved unreliable, so select_hand distinguishes hands by spatial
# x-position and this constant maps image side -> user hand. On hardware
# (2026-07-06) True read INVERTED (the user's left hand showed on the right), so
# it is False: the user's LEFT hand appears on the image LEFT — the forward-
# facing tracking camera is not mirrored. Pinned on hardware.
MIRROR_HANDEDNESS = False

WRIST = 0
THUMB_TIP = 4
INDEX_MCP = 5
INDEX_PIP = 6
INDEX_TIP = 8
MIDDLE_MCP = 9
MIDDLE_PIP = 10
MIDDLE_TIP = 12
RING_PIP = 14
RING_TIP = 16
PINKY_PIP = 18
PINKY_TIP = 20


def _dist(a, b):
    return math.hypot(a[0] - b[0], a[1] - b[1])


def pinch_norm(landmarks):
    """Thumb-tip/index-tip distance normalized by wrist-to-middle-MCP palm
    size — a hand-size/distance-from-camera-invariant ratio. Smaller means
    a tighter pinch."""
    palm = _dist(landmarks[WRIST], landmarks[MIDDLE_MCP])
    if palm < 1e-6:
        return float("inf")
    return _dist(landmarks[THUMB_TIP], landmarks[INDEX_TIP]) / palm


def pinch_pos(landmarks):
    """Normalized [0,1] midpoint between thumb tip and index tip."""
    t, i = landmarks[THUMB_TIP], landmarks[INDEX_TIP]
    return ((t[0] + i[0]) / 2.0, (t[1] + i[1]) / 2.0)


def _finger_curled(landmarks, pip_idx, tip_idx):
    wrist = landmarks[WRIST]
    return _dist(landmarks[tip_idx], wrist) < _dist(landmarks[pip_idx], wrist)


def classify_pose(landmarks):
    """Classify a static hand pose from the four non-thumb fingers' curl."""
    fingers = [
        (INDEX_PIP, INDEX_TIP),
        (MIDDLE_PIP, MIDDLE_TIP),
        (RING_PIP, RING_TIP),
        (PINKY_PIP, PINKY_TIP),
    ]
    curled = [_finger_curled(landmarks, pip, tip) for pip, tip in fingers]
    if all(curled):
        return "fist"
    if not any(curled):
        return "open_palm"
    if not curled[0] and all(curled[1:]):
        return "point"
    if not curled[0] and not curled[1] and curled[2] and curled[3]:
        return "two_up"
    return "none"


def select_hand(hands, target, mirror=MIRROR_HANDEDNESS):
    """Pick the user's `target` ('left'/'right') hand from one frame's
    detections by SPATIAL x-position, ignoring MediaPipe's handedness label.

    The label is unreliable here: the two stereo cameras view a hand from
    different angles, so MediaPipe labels the same physical hand differently in
    each frame (which made a lone hand match in both). Position is robust: sort
    detected hands by wrist x — the image-left hand is one side, the image-right
    hand the other. `mirror` maps image side -> user hand (see MIRROR_HANDEDNESS:
    forward-facing camera -> the user's left hand is on the image right).

    `hands` is a list of (handedness_label, landmarks); the label is ignored.
    Returns the target hand's landmarks, or None if it isn't in this frame.
    """
    if not hands:
        return None
    # Does the user's `target` hand sit on the image-left half?
    #   mirror=True  (forward camera): user-left is on image-RIGHT.
    #   mirror=False (mirrored view):  user-left is on image-LEFT.
    want_image_left = (target == "right") if mirror else (target == "left")
    ordered = sorted(hands, key=lambda h: h[1][WRIST][0])  # ascending image x
    if len(ordered) == 1:
        # One hand: classify by which half of the image its wrist is in, so a
        # lone hand only satisfies ONE side (the "same hand on both sides" fix).
        is_image_left = ordered[0][1][WRIST][0] < 0.5
        return ordered[0][1] if is_image_left == want_image_left else None
    # Two+ hands: take the extreme in the wanted direction.
    chosen = ordered[0] if want_image_left else ordered[-1]
    return chosen[1]
