#!/usr/bin/env python3
"""Hand-gesture sidecar for spatial-screens.

Connects to the Unix domain socket spatial-screens listens on, receives raw
camera frames, runs MediaPipe Hands, and sends back one JSON gesture event
per processed frame. See
docs/specs/2026-07-03-hand-gesture-control-design.md for the wire protocol.

Standalone testing: python3 hand_tracker.py --socket /tmp/test.sock --echo
"""
import argparse
import os
import socket
import statistics
import sys
import time
import urllib.request

from protocol import encode_event, read_frame
from depth_fusion import robust_depth

FORMAT_GRAY8 = 0

# The installed mediapipe (0.10.35) has dropped the legacy `mp.solutions.hands`
# API entirely (it bundled its model internally); only the newer Tasks API
# (`mediapipe.tasks.python.vision.HandLandmarker`) remains, which requires an
# explicit `.task` model asset file. Auto-download it on first use so `make &&
# ./run.sh` keeps working with no manual setup step.
_MODEL_URL = (
    "https://storage.googleapis.com/mediapipe-models/hand_landmarker/"
    "hand_landmarker/float16/latest/hand_landmarker.task"
)
_MODEL_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "models", "hand_landmarker.task")


def _ensure_model():
    if not os.path.exists(_MODEL_PATH):
        os.makedirs(os.path.dirname(_MODEL_PATH), exist_ok=True)
        print(f"hand_tracker: downloading hand landmark model to {_MODEL_PATH}", file=sys.stderr)
        # Download to a temp file in the *same directory* as the final path
        # (not tempfile's default dir, which may be on a different
        # filesystem and would make the rename below fail or silently fall
        # back to a non-atomic copy), then rename into place only once the
        # download has fully succeeded. Otherwise an interrupted download
        # (network drop, Ctrl-C, disk full) leaves a partial/corrupt file at
        # _MODEL_PATH that the os.path.exists() check above would then
        # silently reuse forever, permanently breaking
        # HandLandmarker.create_from_options().
        tmp_path = _MODEL_PATH + f".tmp-{os.getpid()}"
        try:
            urllib.request.urlretrieve(_MODEL_URL, tmp_path)
            os.replace(tmp_path, _MODEL_PATH)
        except BaseException:
            try:
                os.remove(tmp_path)
            except OSError:
                pass
            raise
    return _MODEL_PATH


def connect(socket_path, retries=20, delay=0.25):
    for _ in range(retries):
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.connect(socket_path)
            return sock
        except (FileNotFoundError, ConnectionRefusedError):
            time.sleep(delay)
    raise RuntimeError(f"could not connect to {socket_path} after {retries} retries")


def make_reader(sock):
    def read_exact(n):
        chunks = []
        remaining = n
        while remaining > 0:
            chunk = sock.recv(remaining)
            if not chunk:
                return None
            chunks.append(chunk)
            remaining -= len(chunk)
        return b"".join(chunks)
    return read_exact


def _no_hand_event(timestamp):
    return encode_event(timestamp, None, None)


def match_right_hand(left_lm, right_hands, max_row_delta=0.15):
    """Given the authoritative LEFT-image hand landmarks and the RIGHT-image
    detections [(handedness, landmarks), ...], return the right-image landmarks
    of the SAME physical hand, or None.

    Under a nominally-rectified horizontal stereo pair, corresponding landmarks
    share ~the same image row (y) and the right-image x is < the left-image x
    (positive disparity). Pick the right hand with the smallest mean |dy| that
    also has positive mean disparity, within max_row_delta."""
    best, best_dy = None, max_row_delta
    for _label, rlm in right_hands:
        dy = statistics.mean(abs(l[1] - r[1]) for l, r in zip(left_lm, rlm))
        disp = statistics.mean(l[0] - r[0] for l, r in zip(left_lm, rlm))
        if disp <= 0:
            continue
        if dy < best_dy:
            best, best_dy = rlm, dy
    return best


def fuse_depths(user_hands, right_hands):
    """user_hands: {"left": lm|None, "right": lm|None} — authoritative, from the
    LEFT image. right_hands: [(handedness, lm), ...] from the RIGHT image.
    Returns {"left": depth|None, "right": depth|None} (meters)."""
    out = {}
    for side, llm in user_hands.items():
        if llm is None or not right_hands:
            out[side] = None
            continue
        rlm = match_right_hand(llm, right_hands)
        out[side] = robust_depth(llm, rlm) if rlm is not None else None
    return out


def drain_to_latest(read_frame_fn, readable_fn):
    """Read frames until the socket would block, returning the newest (or None
    on EOF). readable_fn() -> bool says whether another frame is buffered.

    Prevents backlog: a slow 2x inference can't accumulate a queue that fills
    the socket buffer and trips the C++ sender's 200 ms send deadline (which
    would permanently disable gesture control). Stale frames are silently
    dropped — benign for a depth channel."""
    frame = read_frame_fn()
    if frame is None:
        return None
    while readable_fn():
        nxt = read_frame_fn()
        if nxt is None:
            break
        frame = nxt
    return frame


def run_echo(sock, read_exact):
    frame_count = 0
    while True:
        frame = read_frame(read_exact)
        if frame is None:
            break
        timestamp, width, height, fmt, planes = frame
        frame_count += 1
        print(f"echo: frame {frame_count} {width}x{height} fmt={fmt} "
              f"planes={len(planes)}", file=sys.stderr)
        sock.sendall(_no_hand_event(timestamp))


def _landmarks_to_pairs(hand_landmarks):
    # hand_landmarks is already a plain list of NormalizedLandmark under the
    # Tasks API (unlike the legacy solutions API, there's no wrapping
    # `.landmark` field to unwrap).
    return [(lm.x, lm.y) for lm in hand_landmarks]


def build_landmarker():
    """Import mediapipe and construct the HandLandmarker (up to two hands in one
    frame — that single frame is what makes the left/right split consistent).

    Deliberately called *before* connect() (see main()): on real hardware,
    `import mediapipe` plus model load takes ~0.5s (mostly import time), and
    spatial-screens starts pushing frames the instant it accepts our socket
    connection, at a raw rate the sidecar doesn't control. A 640x480 GRAY8
    frame (~307KB) is bigger than the default ~208KB Unix-socket send
    buffer, and gesture_client.cpp's send() has no retry/backpressure
    handling — a couple of frames piling up unread during our import/model
    warm-up is enough to fill the buffer and make it hit EAGAIN, which
    permanently disables gesture control for the rest of the process's
    life. Confirmed on hardware: without this ordering, gesture control was
    disabled before the sidecar ever read a single frame. Doing the slow
    setup before connect() means we're already draining the socket in
    run_inference's loop by the time spatial-screens can send anything.
    """
    from mediapipe.tasks.python import vision
    from mediapipe.tasks.python.core.base_options import BaseOptions

    return vision.HandLandmarker.create_from_options(
        vision.HandLandmarkerOptions(
            base_options=BaseOptions(model_asset_path=_ensure_model()),
            running_mode=vision.RunningMode.VIDEO,
            num_hands=2,
            min_hand_detection_confidence=0.5,
            min_hand_presence_confidence=0.5,
            min_tracking_confidence=0.5,
        )
    )


def run_inference(sock, read_exact, landmarker):
    import cv2
    import mediapipe as mp
    import numpy as np
    from classify import classify_pose, pinch_norm, pinch_pos, select_hand

    def hand_dict(lm, handedness):
        return {
            "present": True,
            "handedness": handedness,
            "pinch_norm": pinch_norm(lm),
            "pinch_pos": pinch_pos(lm),
            "pose": classify_pose(lm),
            "landmarks": lm,
        }

    last_ts = -1
    while True:
        frame = read_frame(read_exact)
        if frame is None:
            break
        timestamp, width, height, fmt, planes = frame

        if fmt != FORMAT_GRAY8 or len(planes) < 1:
            print(f"hand_tracker: unexpected format {fmt}/{len(planes)} planes, "
                  f"skipping", file=sys.stderr)
            continue

        # Single frame, both hands: run ONE landmarker (num_hands=2) on ONE camera
        # image (the left plane) and split left/right by spatial x-position within
        # that single frame (see classify.select_hand). Distinguishing hands
        # across the two stereo cameras was tried and failed: parallax (~6 cm
        # baseline) makes the cameras disagree on which side a near-center hand is
        # on, so it appeared in both panels. One frame = one consistent x-axis =
        # a hand is left OR right, never both. planes[1], if the C++ side still
        # sends it, is unused here — reserved for future stereo fusion (which
        # would add true depth; see docs/specs/2026-07-06-two-hand-gestures-design.md).
        gray = np.frombuffer(planes[0], dtype=np.uint8).reshape(height, width)
        rgb = cv2.cvtColor(gray, cv2.COLOR_GRAY2RGB)
        mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)

        ts = max(int(timestamp * 1000), last_ts + 1)  # must strictly increase
        last_ts = ts
        result = landmarker.detect_for_video(mp_image, ts)
        hands = [
            (result.handedness[i][0].category_name, _landmarks_to_pairs(lm))
            for i, lm in enumerate(result.hand_landmarks)
        ]

        left_lm = select_hand(hands, "left")
        right_lm = select_hand(hands, "right")
        left = hand_dict(left_lm, "left") if left_lm is not None else None
        right = hand_dict(right_lm, "right") if right_lm is not None else None
        sock.sendall(encode_event(timestamp, left, right))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", required=True)
    parser.add_argument("--echo", action="store_true",
                         help="skip MediaPipe; just acknowledge frames (IPC smoke test)")
    args = parser.parse_args()

    # Build the (slow-to-import/load) landmarker before connecting — see
    # build_landmarker()'s docstring for why ordering matters.
    landmarker = None if args.echo else build_landmarker()

    sock = connect(args.socket)
    read_exact = make_reader(sock)

    if args.echo:
        run_echo(sock, read_exact)
    else:
        run_inference(sock, read_exact, landmarker)


if __name__ == "__main__":
    main()
