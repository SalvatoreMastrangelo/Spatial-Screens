"""Pure landmark math for gesture classification — no MediaPipe/I/O
dependency, so this is fully unit-testable with synthetic coordinates.

landmarks is a 21-element list of (x, y) tuples in MediaPipe's normalized
image coordinates (origin top-left, y increases downward), indexed per the
standard MediaPipe Hands landmark layout.
"""
import math

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


# A finger counts as curled only when its tip is CLEARLY inside its PIP joint
# — closer to the wrist by at least this margin. Without the margin, a hand
# being lowered or leaving the frame (fingers only loosely/ambiguously curled)
# read as a full "fist" and fired a spurious recenter. Tunable: lower = stricter
# (a tighter clench required); the test fist sits around 0.48, so 0.7 leaves
# headroom for a real, not-perfectly-tight fist while rejecting a limp hand.
_CURL_MARGIN = 0.7


def _finger_curled(landmarks, pip_idx, tip_idx):
    wrist = landmarks[WRIST]
    return _dist(landmarks[tip_idx], wrist) < _dist(landmarks[pip_idx], wrist) * _CURL_MARGIN


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
