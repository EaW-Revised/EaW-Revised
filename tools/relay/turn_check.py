#!/usr/bin/env python3
"""Check that a TURN server answers over UDP from this network (docs/deploy/coturn-fallback.md).

    python tools/relay/turn_check.py eaw.ernst.work [--port 3478] [--count 20]

Sends unauthenticated TURN Allocate requests (a conforming server answers each with `401 Unauthorized` and a nonce; nothing
is allocated and no credential is needed) and counts the answers. Cloudflare's TURN addresses ignore unauthenticated
requests, so this is for the operator's own coturn. Standard library only.
"""
import argparse
import os
import socket
import struct
import sys
import time
import zlib

MAGIC = 0x2112A442
ALLOCATE_REQUEST = 0x0003
ALLOCATE_ERROR = 0x0113
ALLOCATE_SUCCESS = 0x0103
BINDING_SUCCESS = 0x0101


def allocate_request(transaction: bytes) -> bytes:
    body = struct.pack("!HH", 0x0019, 4) + bytes([17, 0, 0, 0])  # REQUESTED-TRANSPORT: UDP
    message = struct.pack("!HHI", ALLOCATE_REQUEST, len(body) + 8, MAGIC) + transaction + body
    fingerprint = (zlib.crc32(message) & 0xFFFFFFFF) ^ 0x5354554E
    return message + struct.pack("!HHI", 0x8028, 4, fingerprint)


def check(host: str, port: int, count: int, gap: float = 0.1, wait: float = 1.0):
    address = socket.getaddrinfo(host, port, socket.AF_INET, socket.SOCK_DGRAM)[0][4]
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(0.02)
    sent, rtts = {}, []
    next_send, last_send = time.monotonic(), None
    while True:
        now = time.monotonic()
        if len(sent) + len(rtts) < count and now >= next_send:
            transaction = os.urandom(12)
            sock.sendto(allocate_request(transaction), address)
            sent[transaction] = now
            next_send, last_send = now + gap, now
        if last_send is not None and now - last_send > wait and len(sent) + len(rtts) >= count:
            break
        try:
            data, _ = sock.recvfrom(2048)
        except socket.timeout:
            continue
        if len(data) < 20 or data[8:20] not in sent:
            continue
        kind = struct.unpack("!H", data[:2])[0]
        if kind in (ALLOCATE_ERROR, ALLOCATE_SUCCESS, BINDING_SUCCESS):
            rtts.append((time.monotonic() - sent.pop(data[8:20])) * 1000)
    return address[0], sorted(rtts)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("host")
    parser.add_argument("--port", type=int, default=3478)
    parser.add_argument("--count", type=int, default=20)
    args = parser.parse_args()
    ip, rtts = check(args.host, args.port, args.count)
    median = f", median {rtts[len(rtts) // 2]:.0f} ms" if rtts else ""
    print(f"coturn answered {len(rtts)}/{args.count} from {ip}:{args.port}{median}")
    return 0 if len(rtts) == args.count else 1


if __name__ == "__main__":
    sys.exit(main())
