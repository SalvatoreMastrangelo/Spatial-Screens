#!/usr/bin/env python3
"""Hand-gesture sidecar for spatial-screens.

Connects to the Unix domain socket spatial-screens listens on, receives raw
camera frames, and sends back one JSON gesture event per frame. See
docs/specs/2026-07-03-hand-gesture-control-design.md for the wire protocol.

Standalone testing: python3 hand_tracker.py --socket /tmp/test.sock --echo
"""
import argparse
import socket
import sys
import time

from protocol import encode_event, read_frame

FORMAT_GRAY8 = 0


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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", required=True)
    parser.add_argument("--echo", action="store_true",
                         help="skip MediaPipe; just acknowledge frames (IPC smoke test)")
    args = parser.parse_args()

    sock = connect(args.socket)
    read_exact = make_reader(sock)

    if args.echo:
        run_echo(sock, read_exact)
        return

    print("hand_tracker: real inference not wired up yet (see Task 7)", file=sys.stderr)
    run_echo(sock, read_exact)


if __name__ == "__main__":
    main()
