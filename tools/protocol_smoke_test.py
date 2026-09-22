#!/usr/bin/env python3
import json
import math
import socket
import struct
import time

CONTROL_PORT = 47820
VIDEO_PORT = 47821
MAX_DATAGRAM = 1200
HEADER_SIZE = 26
MAX_PAYLOAD = MAX_DATAGRAM - HEADER_SIZE


def recv_line(sock: socket.socket) -> str:
    data = bytearray()
    while True:
        chunk = sock.recv(1)
        if not chunk:
            raise RuntimeError("control connection closed before newline")
        if chunk == b"\n":
            return data.decode("utf-8")
        data.extend(chunk)


def make_packet(
    *,
    sequence: int,
    frame_id: int,
    fragment_id: int,
    fragment_count: int,
    timestamp_us: int,
    payload: bytes,
    keyframe: bool,
) -> bytes:
    flags = 1 if keyframe else 0
    header = struct.pack(
        "!4sBBIIHHQ",
        b"FCAM",
        0,
        flags,
        sequence,
        frame_id,
        fragment_id,
        fragment_count,
        timestamp_us,
    )
    assert len(header) == HEADER_SIZE
    return header + payload


def main() -> None:
    with socket.create_connection(("127.0.0.1", CONTROL_PORT), timeout=5) as control:
        hello = {
            "type": "hello",
            "protocol": 0,
            "deviceName": "CI smoke test",
        }
        control.sendall((json.dumps(hello, separators=(",", ":")) + "\n").encode())
        ack = json.loads(recv_line(control))
        assert ack["type"] == "hello_ack", ack
        assert ack["protocol"] == 0, ack
        assert ack["videoPort"] == VIDEO_PORT, ack

        access_unit = b"\x00\x00\x00\x01\x65" + bytes(
            index % 251 for index in range(5000)
        )

        fragment_count = math.ceil(len(access_unit) / MAX_PAYLOAD)
        timestamp_us = int(time.monotonic() * 1_000_000)

        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as video:
            sequence = 100

            for fragment_id in range(fragment_count):
                start = fragment_id * MAX_PAYLOAD
                end = min(start + MAX_PAYLOAD, len(access_unit))
                packet = make_packet(
                    sequence=sequence,
                    frame_id=42,
                    fragment_id=fragment_id,
                    fragment_count=fragment_count,
                    timestamp_us=timestamp_us,
                    payload=access_unit[start:end],
                    keyframe=True,
                )
                video.sendto(packet, ("127.0.0.1", VIDEO_PORT))
                sequence += 1

        time.sleep(0.25)


if __name__ == "__main__":
    main()
