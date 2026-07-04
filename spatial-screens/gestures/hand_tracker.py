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
import sys
import time
import urllib.request

from classify import classify_pose, pinch_norm, pinch_pos
from protocol import encode_event, read_frame

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
        urllib.request.urlretrieve(_MODEL_URL, _MODEL_PATH)
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
    return encode_event(timestamp, False, "", [(0.0, 0.0)] * 21, 999.0, (0.0, 0.0), "none")


def run_echo(sock, read_exact):
    frame_count = 0
    while True:
        frame = read_frame(read_exact)
        if frame is None:
            break
        timestamp, width, height, fmt, _data = frame
        frame_count += 1
        print(f"echo: frame {frame_count} {width}x{height} fmt={fmt}", file=sys.stderr)
        sock.sendall(_no_hand_event(timestamp))


def _landmarks_to_pairs(hand_landmarks):
    # hand_landmarks is already a plain list of NormalizedLandmark under the
    # Tasks API (unlike the legacy solutions API, there's no wrapping
    # `.landmark` field to unwrap).
    return [(lm.x, lm.y) for lm in hand_landmarks]


def build_landmarker():
    """Import mediapipe and construct the HandLandmarker.

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
            num_hands=1,
            min_hand_detection_confidence=0.5,
            min_hand_presence_confidence=0.5,
            min_tracking_confidence=0.5,
        )
    )


def run_inference(sock, read_exact, landmarker):
    import cv2
    import mediapipe as mp
    import numpy as np

    last_ts_ms = -1
    while True:
        frame = read_frame(read_exact)
        if frame is None:
            break
        timestamp, width, height, fmt, data = frame

        if fmt != FORMAT_GRAY8:
            print(f"hand_tracker: unexpected format {fmt}, skipping frame", file=sys.stderr)
            continue

        gray = np.frombuffer(data, dtype=np.uint8).reshape(height, width)
        rgb = cv2.cvtColor(gray, cv2.COLOR_GRAY2RGB)
        mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)

        # detect_for_video requires strictly increasing timestamps; guard
        # against the source timestamp repeating or going backwards.
        ts_ms = max(int(timestamp * 1000), last_ts_ms + 1)
        last_ts_ms = ts_ms
        result = landmarker.detect_for_video(mp_image, ts_ms)

        if result.hand_landmarks:
            lm = _landmarks_to_pairs(result.hand_landmarks[0])
            handedness = result.handedness[0][0].category_name.lower()
            event = encode_event(
                timestamp, True, handedness, lm,
                pinch_norm(lm), pinch_pos(lm), classify_pose(lm),
            )
        else:
            event = _no_hand_event(timestamp)
        sock.sendall(event)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", required=True)
    parser.add_argument("--echo", action="store_true",
                         help="skip MediaPipe; just acknowledge frames (IPC smoke test)")
    args = parser.parse_args()

    # Build the (slow-to-import/load) landmarker before connecting — see
    # build_landmarker()'s docstring for why ordering matters here.
    landmarker = None if args.echo else build_landmarker()

    sock = connect(args.socket)
    read_exact = make_reader(sock)

    if args.echo:
        run_echo(sock, read_exact)
    else:
        run_inference(sock, read_exact, landmarker)


if __name__ == "__main__":
    main()
