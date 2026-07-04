import io
import json

from protocol import encode_event, encode_frame, read_frame


def _reader_from_bytes(data):
    buf = io.BytesIO(data)

    def read_exact(n):
        chunk = buf.read(n)
        return chunk if chunk else None

    return read_exact


def test_encode_then_read_frame_round_trips():
    raw = b"\x01\x02\x03\x04\x05\x06"
    msg = encode_frame(1.5, 3, 2, 0, raw)

    t, w, h, fmt, data = read_frame(_reader_from_bytes(msg))

    assert (t, w, h, fmt, data) == (1.5, 3, 2, 0, raw)


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
