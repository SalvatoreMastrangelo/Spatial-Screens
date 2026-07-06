"""Pure landmark math for gesture classification — no MediaPipe/I/O
dependency, so this is fully unit-testable with synthetic coordinates.

landmarks is a 21-element list of (x, y) tuples in MediaPipe's normalized
image coordinates (origin top-left, y increases downward), indexed per the
standard MediaPipe Hands landmark layout.
"""
import math

# MediaPipe reports handedness assuming a mirrored (selfie) image; the Luma's
# tracking cameras face forward, so the Left/Right labels are inverted relative
# to the user's actual hands. Flip them here. Pinned during the hardware pass.
MIRROR_HANDEDNESS = True

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
    return "none"


def select_hand(hands, target, mirror=MIRROR_HANDEDNESS):
    """Pick the user's `target` ('left'/'right') hand from one frame's
    detections. `hands` is a list of (handedness_label, landmarks) tuples.
    Returns the matching landmarks, or None if that hand isn't in this frame."""
    for label, landmarks in hands:
        user_label = label.lower()
        if mirror:
            user_label = "right" if user_label == "left" else "left"
        if user_label == target:
            return landmarks
    return None
