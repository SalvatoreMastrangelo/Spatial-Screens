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
import threading
import time
import urllib.request

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
    """Import mediapipe and construct one HandLandmarker (up to two hands).

    main() builds two independent instances of this — one per camera plane
    (left/right) — since VIDEO running-mode is stateful per stream and each
    needs its own monotonic timestamp counter.

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


def run_inference(sock, read_exact, landmarker_left, landmarker_right):
    import cv2
    import mediapipe as mp
    import numpy as np
    from classify import classify_pose, pinch_norm, pinch_pos, select_hand

    def detect(landmarker, plane, width, height, ts_ms):
        gray = np.frombuffer(plane, dtype=np.uint8).reshape(height, width)
        rgb = cv2.cvtColor(gray, cv2.COLOR_GRAY2RGB)
        mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
        result = landmarker.detect_for_video(mp_image, ts_ms)
        return [
            (result.handedness[i][0].category_name, _landmarks_to_pairs(lm))
            for i, lm in enumerate(result.hand_landmarks)
        ]

    def hand_dict(lm, handedness):
        return {
            "present": True,
            "handedness": handedness,
            "pinch_norm": pinch_norm(lm),
            "pinch_pos": pinch_pos(lm),
            "pose": classify_pose(lm),
            "landmarks": lm,
        }

    last_ts_l, last_ts_r = -1, -1
    while True:
        frame = read_frame(read_exact)
        if frame is None:
            break
        timestamp, width, height, fmt, planes = frame

        if fmt != FORMAT_GRAY8 or len(planes) < 1:
            print(f"hand_tracker: unexpected format {fmt}/{len(planes)} planes, "
                  f"skipping", file=sys.stderr)
            continue

        # detect_for_video needs strictly-increasing per-stream timestamps.
        ts_l = max(int(timestamp * 1000), last_ts_l + 1)
        last_ts_l = ts_l
        # Right camera plane if present, else fall back to the left plane so a
        # single-plane sender still yields both hands (num_hands=2 sees both).
        right_plane = planes[1] if len(planes) > 1 else planes[0]
        ts_r = max(int(timestamp * 1000), last_ts_r + 1)
        last_ts_r = ts_r

        # Run the two independent landmarker inferences CONCURRENTLY. They use
        # separate landmarker instances on separate planes and share no state,
        # and MediaPipe releases the GIL during its C++ graph, so two threads
        # nearly halve the per-frame cost (~1.7x measured on hardware) — letting
        # dual-camera tracking hold the 15 Hz target. Sequential is ~52 ms/cycle
        # (~19 Hz ceiling, lower with hands in frame); threaded ~30 ms.
        hands = [None, None]

        def _run(idx, landmarker, plane, ts):
            hands[idx] = detect(landmarker, plane, width, height, ts)

        tL = threading.Thread(target=_run, args=(0, landmarker_left, planes[0], ts_l))
        tR = threading.Thread(target=_run, args=(1, landmarker_right, right_plane, ts_r))
        tL.start()
        tR.start()
        tL.join()
        tR.join()
        left_hands, right_hands = hands

        left_lm = select_hand(left_hands, "left")
        right_lm = select_hand(right_hands, "right")
        left = hand_dict(left_lm, "left") if left_lm is not None else None
        right = hand_dict(right_lm, "right") if right_lm is not None else None
        sock.sendall(encode_event(timestamp, left, right))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", required=True)
    parser.add_argument("--echo", action="store_true",
                         help="skip MediaPipe; just acknowledge frames (IPC smoke test)")
    args = parser.parse_args()

    # Build the (slow-to-import/load) landmarkers before connecting — see
    # build_landmarker()'s docstring for why ordering matters. Two independent
    # VIDEO streams (left + right camera) each need their own stateful instance.
    landmarker_left = None if args.echo else build_landmarker()
    landmarker_right = None if args.echo else build_landmarker()

    sock = connect(args.socket)
    read_exact = make_reader(sock)

    if args.echo:
        run_echo(sock, read_exact)
    else:
        run_inference(sock, read_exact, landmarker_left, landmarker_right)


if __name__ == "__main__":
    main()
