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

from protocol import encode_event, read_frame
from depth_fusion import robust_depth
from enhance import make_enhancer
from fuse_hands import nominal_disparity, fuse_and_assign, match_right_hand

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


def run_inference(sock, read_exact, landmarker, landmarker_r=None, fusion=False, both_cam=True, make_enh=None):
    import select as _select
    from concurrent.futures import ThreadPoolExecutor

    import cv2
    import mediapipe as mp
    import numpy as np
    from classify import MIRROR_HANDEDNESS, classify_pose, pinch_norm, pinch_pos, select_hand

    # One enhancer PER inference thread: cv2.CLAHE is stateful and not reentrant,
    # and the fusion path runs the left/right infer() concurrently. make_enh() is
    # a factory that returns a fresh enhancer (fresh CLAHE) on each call.
    make_enh = make_enh or (lambda: (lambda g: g))
    enh_l = make_enh()
    d0 = nominal_disparity()

    def hand_dict(lm, handedness, depth=None):
        return {
            "present": True,
            "handedness": handedness,
            "pinch_norm": pinch_norm(lm),
            "pinch_pos": pinch_pos(lm),
            "pose": classify_pose(lm),
            "landmarks": lm,
            "depth": depth,
        }

    def infer(landmarker_obj, plane, width, height, ts, enh):
        gray = np.frombuffer(plane, dtype=np.uint8).reshape(height, width)
        gray = enh(gray)                           # brightening pre-pass (per-thread enhancer)
        rgb = cv2.cvtColor(gray, cv2.COLOR_GRAY2RGB)
        mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
        result = landmarker_obj.detect_for_video(mp_image, ts)
        return [(result.handedness[i][0].category_name, _landmarks_to_pairs(lm))
                for i, lm in enumerate(result.hand_landmarks)]

    def readable():
        return bool(_select.select([sock], [], [], 0)[0])

    do_fusion = fusion and landmarker_r is not None
    pool = ThreadPoolExecutor(max_workers=2) if do_fusion else None
    enh_r = make_enh() if do_fusion else None   # separate CLAHE for the right-plane thread
    last_ts_l = last_ts_r = -1
    while True:
        frame = drain_to_latest(lambda: read_frame(read_exact), readable)
        if frame is None:
            break
        timestamp, width, height, fmt, planes = frame
        if fmt != FORMAT_GRAY8 or len(planes) < 1:
            print(f"hand_tracker: unexpected format {fmt}/{len(planes)} planes, "
                  f"skipping", file=sys.stderr)
            continue

        # detect_for_video needs a strictly-increasing timestamp PER landmarker.
        ts_l = max(int(timestamp * 1000), last_ts_l + 1)
        last_ts_l = ts_l
        stereo = do_fusion and len(planes) >= 2
        if stereo:
            ts_r = max(int(timestamp * 1000), last_ts_r + 1)
            last_ts_r = ts_r
            fut_r = pool.submit(infer, landmarker_r, planes[1], width, height, ts_r, enh_r)
            hands = infer(landmarker, planes[0], width, height, ts_l, enh_l)
            try:
                right_hands = fut_r.result()
            except Exception as e:  # right inference is best-effort; never kill gestures
                print(f"hand_tracker: right-eye inference failed ({e}); depth off "
                      f"this frame", file=sys.stderr)
                right_hands, stereo = None, False
        else:
            hands = infer(landmarker, planes[0], width, height, ts_l, enh_l)
            right_hands = None

        if stereo and both_cam:
            left_f, right_f = fuse_and_assign(hands, right_hands, d0, MIRROR_HANDEDNESS)
            left = (hand_dict(left_f["landmarks"], "left", left_f["depth"])
                    if left_f is not None else None)
            right = (hand_dict(right_f["landmarks"], "right", right_f["depth"])
                     if right_f is not None else None)
        else:
            left_lm = select_hand(hands, "left")
            right_lm = select_hand(hands, "right")
            depths = (fuse_depths({"left": left_lm, "right": right_lm}, right_hands)
                      if stereo else {"left": None, "right": None})
            left = (hand_dict(left_lm, "left", depths["left"])
                    if left_lm is not None else None)
            right = (hand_dict(right_lm, "right", depths["right"])
                     if right_lm is not None else None)
        sock.sendall(encode_event(timestamp, left, right))


def build_argparser():
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", required=True)
    parser.add_argument("--echo", action="store_true",
                        help="skip MediaPipe; just acknowledge frames (IPC smoke test)")
    parser.add_argument("--fusion", action="store_true",
                        help="enable stereo depth fusion (2nd inference on the right plane)")
    parser.add_argument("--no-both-cam", dest="both_cam", action="store_false",
                        help="disable both-camera tracking union (fusion only feeds depth)")
    parser.add_argument("--enhance", default="gamma_clahe",
                        choices=["none", "gamma", "clahe", "gamma_clahe"])
    parser.add_argument("--enhance-gamma", type=float, default=0.45)
    parser.add_argument("--enhance-clahe-clip", type=float, default=3.0)
    parser.set_defaults(both_cam=True)
    return parser


def main():
    args = build_argparser().parse_args()

    # Build the (slow-to-import/load) landmarker(s) before connecting — see
    # build_landmarker()'s docstring for why ordering matters.
    landmarker = None if args.echo else build_landmarker()
    landmarker_r = build_landmarker() if (args.fusion and not args.echo) else None

    sock = connect(args.socket)
    read_exact = make_reader(sock)

    if args.echo:
        run_echo(sock, read_exact)
    else:
        run_inference(sock, read_exact, landmarker, landmarker_r, args.fusion, args.both_cam,
                      lambda: make_enhancer(args.enhance, args.enhance_gamma, args.enhance_clahe_clip))


if __name__ == "__main__":
    main()
