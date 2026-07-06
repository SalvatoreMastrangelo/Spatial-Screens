"""Wire protocol between spatial-screens (C++) and the gesture sidecar.

Frame forwarding (spatial-screens -> sidecar), length-prefixed binary:
    [u32 len][f64 timestamp][i32 width][i32 height][u8 format][u8 n_planes][raw bytes]

Gesture events (sidecar -> spatial-screens), newline-delimited JSON, one
object per processed frame. See
docs/specs/2026-07-03-hand-gesture-control-design.md for the design
rationale.
"""
import json
import struct

_LENGTH_PREFIX = struct.Struct("<I")
_FRAME_HEADER = struct.Struct("<dii B B")  # timestamp, width, height, format, n_planes


def read_frame(read_exact):
    """Read one frame message via read_exact(n) -> bytes|None.

    Returns (timestamp, width, height, format, planes), where planes is a list
    of n_planes equal-size byte buffers (GRAY8: one byte per pixel), or None on
    clean EOF / short read.
    """
    length_bytes = read_exact(_LENGTH_PREFIX.size)
    if length_bytes is None:
        return None
    (length,) = _LENGTH_PREFIX.unpack(length_bytes)
    payload = read_exact(length)
    if payload is None or len(payload) < length:
        return None
    timestamp, width, height, fmt, n_planes = _FRAME_HEADER.unpack(
        payload[: _FRAME_HEADER.size])
    body = payload[_FRAME_HEADER.size :]
    plane_size = len(body) // n_planes if n_planes else 0
    planes = [body[i * plane_size : (i + 1) * plane_size] for i in range(n_planes)]
    return timestamp, width, height, fmt, planes


def encode_frame(timestamp, width, height, fmt, planes):
    """Inverse of read_frame. planes is a list of equal-size byte buffers."""
    body = b"".join(planes)
    payload = _FRAME_HEADER.pack(timestamp, width, height, fmt, len(planes)) + body
    return _LENGTH_PREFIX.pack(len(payload)) + payload


def _hand_obj(h):
    """One hand's sub-object. h is a dict (see encode_event) or None."""
    if h is None or not h.get("present", False):
        return {"present": False}
    obj = {
        "present": True,
        "handedness": h.get("handedness", ""),
        "pinch_norm": h["pinch_norm"],
        "pinch_pos": list(h["pinch_pos"]),
        "pose": h["pose"],
        "landmarks": [list(p) for p in h["landmarks"]],
    }
    # Fused stereo depth (meters), when available. Omitted (not null) when the
    # sidecar couldn't triangulate, so the C++ scanner's key-presence check
    # (has_depth) stays simple. Emitted AFTER landmarks — still no nested
    # braces, so hand_object()'s first-'}' termination holds.
    depth = h.get("depth")
    if depth is not None:
        obj["depth"] = depth
    return obj


def encode_event(t, left, right):
    """One gesture event as newline-delimited JSON carrying up to two hands.

    left/right are per-hand dicts (keys: present, handedness, pinch_norm,
    pinch_pos, pose, landmarks) or None. pinch_pos is the normalized [0,1]
    midpoint of the thumb-tip/index-tip landmarks — a convenience field so the
    C++ consumer can track pinch-drag deltas without parsing the full landmarks
    array.
    """
    obj = {
        "type": "hand",
        "t": t,
        "left": _hand_obj(left),
        "right": _hand_obj(right),
    }
    return (json.dumps(obj, separators=(",", ":")) + "\n").encode("utf-8")
