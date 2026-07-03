"""Wire protocol between spatial-screens (C++) and the gesture sidecar.

Frame forwarding (spatial-screens -> sidecar), length-prefixed binary:
    [u32 len][f64 timestamp][i32 width][i32 height][u8 format][raw bytes]

Gesture events (sidecar -> spatial-screens), newline-delimited JSON, one
object per processed frame. See
docs/specs/2026-07-03-hand-gesture-control-design.md for the design
rationale.
"""
import json
import struct

_LENGTH_PREFIX = struct.Struct("<I")
_FRAME_HEADER = struct.Struct("<dii B")  # timestamp, width, height, format


def read_frame(read_exact):
    """Read one frame message via read_exact(n) -> bytes|None.

    Returns (timestamp, width, height, format, data), or None on clean EOF
    (read_exact returned None while reading the length prefix).
    """
    length_bytes = read_exact(_LENGTH_PREFIX.size)
    if length_bytes is None:
        return None
    (length,) = _LENGTH_PREFIX.unpack(length_bytes)
    payload = read_exact(length)
    timestamp, width, height, fmt = _FRAME_HEADER.unpack(payload[: _FRAME_HEADER.size])
    data = payload[_FRAME_HEADER.size :]
    return timestamp, width, height, fmt, data


def encode_frame(timestamp, width, height, fmt, data):
    """Inverse of read_frame — used by spatial-screens (conceptually; the
    C++ side has its own byte-for-byte equivalent) and by tests here to
    build synthetic input."""
    payload = _FRAME_HEADER.pack(timestamp, width, height, fmt) + data
    return _LENGTH_PREFIX.pack(len(payload)) + payload


def encode_event(t, present, handedness, landmarks, pinch_norm, pinch_pos, pose):
    """One gesture event as newline-delimited JSON.

    pinch_pos is the normalized [0,1] midpoint of the thumb-tip/index-tip
    landmarks — a convenience field (not derivable cheaply on the C++ side
    without a full JSON array parser) so the C++ consumer can track
    pinch-drag deltas without parsing the full 21-point landmarks array.
    """
    obj = {
        "type": "hand",
        "t": t,
        "present": present,
        "handedness": handedness,
        "landmarks": [list(p) for p in landmarks],
        "pinch_norm": pinch_norm,
        "pinch_pos": list(pinch_pos),
        "pose": pose,
    }
    return (json.dumps(obj) + "\n").encode("utf-8")
