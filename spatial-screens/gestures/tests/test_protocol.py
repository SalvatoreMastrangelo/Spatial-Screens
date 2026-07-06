import io
import json

from protocol import encode_event, encode_frame, read_frame


def _reader_from_bytes(buf):
    state = {"pos": 0}
    def read_exact(n):
        if state["pos"] + n > len(buf):
            return None
        chunk = buf[state["pos"]:state["pos"] + n]
        state["pos"] += n
        return chunk
    return read_exact


def test_frame_two_planes_roundtrip():
    left = bytes(range(10)) * 4      # 40 bytes
    right = bytes(range(40, 50)) * 4 # 40 bytes, distinct
    buf = encode_frame(1.25, 8, 5, 0, [left, right])
    ts, w, h, fmt, planes = read_frame(_reader_from_bytes(buf))
    assert (ts, w, h, fmt) == (1.25, 8, 5, 0)
    assert len(planes) == 2
    assert planes[0] == left
    assert planes[1] == right

def test_frame_single_plane_roundtrip():
    data = bytes(range(20))
    buf = encode_frame(2.0, 4, 5, 0, [data])
    ts, w, h, fmt, planes = read_frame(_reader_from_bytes(buf))
    assert planes == [data]


def test_read_frame_returns_none_on_clean_eof():
    assert read_frame(_reader_from_bytes(b"")) is None


def test_encode_event_produces_newline_delimited_json():
    msg = encode_event(
        t=1.0, present=True, handedness="left",
        landmarks=[(0.1, 0.2)] * 21,
        pinch_norm=0.3, pinch_pos=(0.15, 0.2), pose="fist",
    )

    assert msg.endswith(b"\n")
    obj = json.loads(msg.decode("utf-8").strip())
    assert obj == {
        "type": "hand", "t": 1.0, "present": True, "handedness": "left",
        "landmarks": [[0.1, 0.2]] * 21,
        "pinch_norm": 0.3, "pinch_pos": [0.15, 0.2], "pose": "fist",
    }


def test_encode_event_uses_compact_separators_the_cpp_parser_requires():
    """gesture_client.cpp's hand-rolled scanners (json_find_bool,
    json_find_string, json_find_pair) look for these exact substrings with
    no space after ':' or ','. json.dumps()'s default separators put a
    space after both, which silently breaks every field but pinch_norm
    (strtof tolerates leading whitespace). This test pins the byte layout
    so a regression to the default separators fails loudly here instead of
    only at runtime against real hardware."""
    msg = encode_event(
        t=1.0, present=True, handedness="left",
        landmarks=[(0.1, 0.2)] * 21,
        pinch_norm=0.3, pinch_pos=(0.15, 0.2), pose="fist",
    )

    assert b'"present":true' in msg
    assert b'"pose":"fist"' in msg
    assert b'"pinch_pos":[0.15,0.2]' in msg
    assert msg.endswith(b"\n")
