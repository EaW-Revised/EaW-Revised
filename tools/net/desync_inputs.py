#!/usr/bin/env python3
"""Read the canonical input batches of a desync diagnostic bundle (docs/replay-format.md).

    python tools/net/desync_inputs.py show <inputs.bin>
    python tools/net/desync_inputs.py diff <inputs.bin> <remote-peer-N-inputs.bin>

`show` lists, for every retained tick, the sealed control inputs and the tactical commands the batch carries
(tick, player, sequence, opcode and the payload bytes). `diff` compares the two files tick by tick and names the
first command that differs; the remote file holds only the ticks whose input hash disagreed.

A batch is the match-control domain line, a u32 count of sealed control inputs (each: u64 tick, u32 seat, u64
sequence, u32 size, bytes) and, when the world stepped, the tactical replay record of the commands executed that
tick. This reader needs no game data and stays on the standard library.
"""
from __future__ import annotations

import struct
import sys

MAGIC = b"eawr-desync-input-v1\n"
PAUSE_DOMAIN = b"eawr-match-pause-v1"
REPLAY_MAGIC = b"EAWRPLY\x00"


def frames(data: bytes):
    if not data.startswith(MAGIC):
        raise ValueError("not a desync input file")
    offset = len(MAGIC)
    while offset < len(data):
        tick, size = struct.unpack_from("<QQ", data, offset)
        offset += 16
        yield tick, data[offset:offset + size]
        offset += size


def commands(record: bytes):
    """The commands of an embedded tactical replay record, as (tick, player, sequence, opcode, payload hex)."""
    header_size = struct.unpack_from("<H", record, 10)[0]
    _seed, _final, players, squadrons, units, count = struct.unpack_from("<QQIIQQ", record, 32)
    offset = header_size + players * 24 + units * 80
    for _ in range(squadrons):
        _container, members, _zero = struct.unpack_from("<QII", record, offset)
        offset += 16 + 8 * members
    result = []
    for _ in range(count):
        size = struct.unpack_from("<I", record, offset)[0]
        body = record[offset + 4:offset + 4 + size]
        offset += 4 + size
        tick, player, sequence, opcode = struct.unpack_from("<QIQB", body, 0)
        result.append((tick, player, sequence, opcode, body[24:].hex()))
    return result


def describe(batch: bytes):
    """(sealed control inputs, commands) of one batch, or (None, None) for a batch that is not a match-control one."""
    if not batch.startswith(PAUSE_DOMAIN):
        return None, None
    offset = len(PAUSE_DOMAIN)
    sealed_count = struct.unpack_from("<I", batch, offset)[0]
    offset += 4
    sealed = []
    for _ in range(sealed_count):
        tick, seat, sequence, size = struct.unpack_from("<QIQI", batch, offset)
        offset += 24
        sealed.append((tick, seat, sequence, batch[offset:offset + size].hex()))
        offset += size
    record = batch[offset:]
    return sealed, (commands(record) if record.startswith(REPLAY_MAGIC) else [])


def show(path: str) -> int:
    for tick, batch in frames(open(path, "rb").read()):
        sealed, found = describe(batch)
        if sealed is None:
            print(f"tick {tick}: {len(batch)} bytes, unknown framing")
            continue
        print(f"tick {tick}: {len(batch)} bytes, {len(sealed)} sealed input(s), {len(found)} command(s)")
        for item in sealed:
            print("   sealed", item)
        for item in found:
            print("   command", item)
    return 0


def diff(local_path: str, remote_path: str) -> int:
    local = dict(frames(open(local_path, "rb").read()))
    differing = 0
    for tick, remote in frames(open(remote_path, "rb").read()):
        mine = local.get(tick)
        if mine is None:
            print(f"tick {tick}: local batch not retained")
            continue
        if mine == remote:
            continue
        differing += 1
        sealed_a, commands_a = describe(mine)
        sealed_b, commands_b = describe(remote)
        print(f"tick {tick}: local {len(mine)} bytes, remote {len(remote)} bytes")
        if sealed_a != sealed_b:
            print("   sealed inputs differ:", sealed_a, "vs", sealed_b)
        if commands_a is None or commands_b is None:
            continue
        for index in range(max(len(commands_a), len(commands_b))):
            a = commands_a[index] if index < len(commands_a) else None
            b = commands_b[index] if index < len(commands_b) else None
            if a != b:
                print(f"   command #{index}: local {a} | remote {b}")
                break
        else:
            print("   commands equal; the record header or setup bytes differ")
    return 1 if differing else 0


def main(argv: list[str]) -> int:
    if len(argv) == 3 and argv[1] == "show":
        return show(argv[2])
    if len(argv) == 4 and argv[1] == "diff":
        return diff(argv[2], argv[3])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
