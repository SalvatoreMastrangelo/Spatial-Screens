import io
import json

from protocol import encode_event, encode_frame, read_frame


def _decode(evt_bytes):
    assert evt_bytes.endswith(b"\n")
    return json.loads(evt_bytes[:-1].decode("utf-8"))


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
    lm = [(0.1, 0.2)] * 21
    left = {"present": True, "handedness": "left", "pinch_norm": 0.3,
            "pinch_pos": (0.15, 0.2), "pose": "fist", "landmarks": lm}
    msg = encode_event(t=1.0, left=left, right=None)

    assert msg.endswith(b"\n")
    obj = json.loads(msg.decode("utf-8").strip())
    assert obj == {
        "type": "hand", "t": 1.0,
        "left": {"present": True, "handedness": "left",
                 "landmarks": [[0.1, 0.2]] * 21,
                 "pinch_norm": 0.3, "pinch_pos": [0.15, 0.2], "pose": "fist"},
        "right": {"present": False},
    }


def test_encode_event_uses_compact_separators_the_cpp_parser_requires():
    """gesture_client.cpp's hand-rolled scanners (json_find_bool,
    json_find_string, json_find_pair) look for these exact substrings with
    no space after ':' or ','. json.dumps()'s default separators put a
    space after both, which silently breaks every field but pinch_norm
    (strtof tolerates leading whitespace). This test pins the byte layout
    so a regression to the default separators fails loudly here instead of
    only at runtime against real hardware."""
    lm = [(0.1, 0.2)] * 21
    left = {"present": True, "handedness": "left", "pinch_norm": 0.3,
            "pinch_pos": (0.15, 0.2), "pose": "fist", "landmarks": lm}
    msg = encode_event(t=1.0, left=left, right=None)

    assert b'"present":true' in msg
    assert b'"pose":"fist"' in msg
    assert b'"pinch_pos":[0.15,0.2]' in msg
    assert msg.endswith(b"\n")


def test_event_two_hands():
    lm = [(i / 100.0, i / 50.0) for i in range(21)]
    left = {"present": True, "handedness": "left", "pinch_norm": 0.3,
            "pinch_pos": (0.4, 0.5), "pose": "open_palm", "landmarks": lm}
    obj = _decode(encode_event(1.5, left, None))
    assert obj["type"] == "hand"
    assert obj["t"] == 1.5
    assert obj["left"]["present"] is True
    assert obj["left"]["pose"] == "open_palm"
    assert obj["left"]["pinch_pos"] == [0.4, 0.5]
    assert len(obj["left"]["landmarks"]) == 21
    assert obj["left"]["landmarks"][8] == [0.08, 0.16]
    assert obj["right"] == {"present": False}

def test_event_no_hands():
    obj = _decode(encode_event(2.0, None, None))
    assert obj["left"] == {"present": False}
    assert obj["right"] == {"present": False}
