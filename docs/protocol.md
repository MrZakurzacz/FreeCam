# FreeCam Protocol v0

This document defines the first development protocol. It is intentionally small and can change before the first stable release.

## Network layout

FreeCam uses two logical channels:

- control channel: TCP
- video channel: UDP

## Default ports

Development defaults:
- TCP control: 47820
- UDP video: 47821

These values are not stable API commitments.

## Control channel

The control connection uses UTF-8 JSON messages. Each message ends with a newline.

Example client hello:

```json
{"type":"hello","protocol":0,"deviceName":"Jakub's iPhone"}
```

Example receiver response:

```json
{"type":"hello_ack","protocol":0,"videoPort":47821}
```

Planned control messages:
- hello
- hello_ack
- start_stream
- stop_stream
- set_video
- request_keyframe
- ping
- pong

## Video format

The first implementation sends H.264 Annex B access units over UDP.

Each UDP datagram starts with a small FreeCam packet header:

```text
magic        4 bytes   "FCAM"
version      1 byte
flags        1 byte
sequence     4 bytes
frame_id     4 bytes
fragment_id  2 bytes
fragments    2 bytes
timestamp_us 8 bytes
payload      remaining bytes
```

All integer fields use network byte order.

### Flags

Bit 0:
- 1 = keyframe
- 0 = non-keyframe

Other bits are reserved.

## Fragmentation

An encoded H.264 frame can be larger than one safe UDP datagram.

The sender splits each encoded frame into fragments. Each fragment has the same frame_id and fragments count. fragment_id starts at 0.

The receiver:
1. groups fragments by frame_id
2. rejects incomplete frames after a short timeout
3. reconstructs complete frames
4. passes complete H.264 access units to the decoder

A lost frame should not block later frames.

## Reliability

The video channel does not retransmit normal frames.

The receiver can request a new H.264 keyframe over the TCP control channel after:
- decoder loss
- excessive packet loss
- reconnect

## Planned discovery

Automatic discovery is not part of protocol v0.

The first test build uses the PC IP address entered on the iPhone. Automatic LAN discovery will be added after the basic stream works.
