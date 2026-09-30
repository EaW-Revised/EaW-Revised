#!/usr/bin/env python3
"""Generate the P1-07 fog-stub-v1 sidecar fixtures and their independent oracle.

This file is deliberately standalone: it uses only Python's standard library and
does not import project code or the replay fixture generator. It encodes the
canonical grid, sidecar and evidence bytes of docs/replay-format.md directly, and
computes the tick-0 world-state hash itself from the frozen original-v1 replay
bytes. Ticks 1..5 come from the replay's own independent audit. Production C++
output is never used as expected data.

Usage: generate_fog_fixture.py [--check]
  --check  regenerate into memory and fail if any checked-in fixture differs.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, replace
import hashlib
import json
from pathlib import Path
import struct
import sys


OUT = Path(__file__).resolve().parent
REPLAY_FIXTURES = OUT.parent.parent / "replay" / "fixtures"
ORIGINAL_REPLAY = REPLAY_FIXTURES / "original-v1.eawr-replay"
ORIGINAL_AUDIT = REPLAY_FIXTURES / "original-v1.audit.json"
ZERO_TICK_REPLAY = REPLAY_FIXTURES / "zero-tick-v1.eawr-replay"
ORIGINAL_SHA256 = "fcf7f050a4ae8be540def3e4609fc7ff68a21e4ed4fce59e46c1fc62e59a4f90"
ORIGINAL_BYTES = 1480

GRID_MAGIC = b"EAWRFOG\x00"
SIDECAR_MAGIC = b"EAWRFGF\x00"
EVIDENCE_MAGIC = b"EAWRFGE\x00"
STATE_MAGIC = b"EAWRSTA\x00"
GRID_HEADER_SIZE = 76
SIDECAR_HEADER_SIZE = 60
Q24 = 1 << 24
INT64_MAX = (1 << 63) - 1

CODE_MALFORMED = "EAWR-SIM-0201"
CODE_VERSION = "EAWR-SIM-0202"
CODE_LIMIT = "EAWR-SIM-0203"
CODE_ORDER = "EAWR-SIM-0204"
CODE_REVISION = "EAWR-SIM-0205"
CODE_OVERFLOW = "EAWR-SIM-0206"
CODE_VALUE = "EAWR-SIM-0207"
CODE_IDENTITY = "EAWR-SIM-0208"


@dataclass(frozen=True)
class Grid:
    team_id: int
    width: int
    height: int
    origin_x_raw: int
    origin_y_raw: int
    cell_x_raw: int
    cell_y_raw: int
    revision: int
    cells: bytes
    schema_version: int = 1
    encoding: int = 1
    byte_count: int | None = None


@dataclass(frozen=True)
class Event:
    tick: int
    grid: Grid
    length_override: int | None = None


def grid_bytes(grid: Grid) -> bytes:
    """Canonical FogGridV1 bytes, field by field (76-byte header + cells)."""
    byte_count = len(grid.cells) if grid.byte_count is None else grid.byte_count
    header = bytearray()
    header += GRID_MAGIC                                   # 0
    header += struct.pack("<I", grid.schema_version)       # 8
    header += struct.pack("<I", grid.team_id)              # 12
    header += struct.pack("<I", grid.width)                # 16
    header += struct.pack("<I", grid.height)               # 20
    header += struct.pack("<q", grid.origin_x_raw)         # 24
    header += struct.pack("<q", grid.origin_y_raw)         # 32
    header += struct.pack("<q", grid.cell_x_raw)           # 40
    header += struct.pack("<q", grid.cell_y_raw)           # 48
    header += struct.pack("<I", grid.encoding)             # 56
    header += struct.pack("<Q", grid.revision)             # 60
    header += struct.pack("<Q", byte_count)                # 68
    assert len(header) == GRID_HEADER_SIZE
    return bytes(header) + grid.cells


def sidecar_bytes(
    replay_sha: bytes,
    final_tick: int,
    events: list[Event],
    *,
    magic: bytes = SIDECAR_MAGIC,
    version: int = 1,
    event_count: int | None = None,
) -> bytes:
    out = bytearray()
    out += magic
    out += struct.pack("<I", version)
    out += replay_sha
    out += struct.pack("<Q", final_tick)
    out += struct.pack("<Q", len(events) if event_count is None else event_count)
    assert len(out) == SIDECAR_HEADER_SIZE
    for event in events:
        encoded = grid_bytes(event.grid)
        length = len(encoded) if event.length_override is None else event.length_override
        out += struct.pack("<QQ", event.tick, length)
        out += encoded
    return bytes(out)


def tick0_state_hash(replay: bytes) -> str:
    """Independent EAWRSTA encoding of the initial world (completed tick 0)."""
    numerator, denominator = struct.unpack_from("<II", replay, 24)
    seed, final_tick, entity_count, command_count = struct.unpack_from("<QQQQ", replay, 32)
    content_identity = replay[64:96]
    entity_end = 96 + 64 * entity_count
    entities = replay[96:entity_end]
    ids = [struct.unpack_from("<Q", replay, 96 + 64 * index)[0] for index in range(entity_count)]
    next_id = (max(ids) + 1) if ids else 1
    # Every command tick is >= 0, so all commands are pending at tick 0 and the
    # pending bytes are the command section verbatim, length prefixes included.
    pending = replay[entity_end:]
    offset = 0
    for _ in range(command_count):
        (length,) = struct.unpack_from("<I", pending, offset)
        offset += 4 + length
    assert offset == len(pending)
    state = bytearray()
    state += STATE_MAGIC
    state += struct.pack("<IIIIII", 1, 1, 1, 24, numerator, denominator)
    state += struct.pack("<QQQQQ", 0, final_tick, next_id, seed, 0)
    state += content_identity
    state += struct.pack("<Q", entity_count)
    state += entities
    state += struct.pack("<Q", command_count)
    state += pending
    return hashlib.sha256(bytes(state)).hexdigest()


def active_grids(events: list[Event], tick: int) -> list[Grid]:
    latest: dict[int, Grid] = {}
    for event in events:
        if event.tick <= tick:
            latest[event.grid.team_id] = event.grid
    return [latest[team] for team in sorted(latest)]


def evidence_rows(
    replay: bytes,
    sidecar: bytes,
    events: list[Event],
    world_hashes: list[str],
) -> list[tuple[int, str, int]]:
    replay_sha = hashlib.sha256(replay).digest()
    sidecar_sha = hashlib.sha256(sidecar).digest()
    rows = []
    for tick, world_hash in enumerate(world_hashes):
        grids = active_grids(events, tick)
        preimage = bytearray()
        preimage += EVIDENCE_MAGIC
        preimage += struct.pack("<I", 1)
        preimage += replay_sha
        preimage += sidecar_sha
        preimage += struct.pack("<Q", tick)
        preimage += bytes.fromhex(world_hash)
        preimage += struct.pack("<I", len(grids))
        for grid in grids:
            encoded = grid_bytes(grid)
            preimage += struct.pack("<Q", len(encoded))
            preimage += encoded
        rows.append((tick, hashlib.sha256(bytes(preimage)).hexdigest(), len(preimage)))
    return rows


def csv_text(rows: list[tuple[int, str, int]]) -> bytes:
    return ("tick,sha256\n" + "".join(f"{tick},{digest}\n" for tick, digest, _ in rows)).encode("utf-8")


def q24(numerator: int, denominator: int = 1) -> int:
    assert (numerator * Q24) % denominator == 0
    return numerator * Q24 // denominator


# Team 0: asymmetric 3x2, negative X origin, rectangular cells (2.0 x 0.75).
TEAM0_R1 = Grid(0, 3, 2, q24(-7, 2), q24(41, 4), q24(2), q24(3, 4), 1,
                bytes([0, 64, 128, 192, 255, 17]))
# Team 7: 2x3 (transposed shape), different origin and cells (1.5 x 3.0).
TEAM7_R1 = Grid(7, 2, 3, q24(100), q24(-40), q24(3, 2), q24(3), 1,
                bytes([255, 0, 9, 200, 33, 1]))
# One-cell change at (x=2, y=1): 17 -> 18, revision advances.
TEAM0_R2 = replace(TEAM0_R1, revision=2, cells=bytes([0, 64, 128, 192, 255, 18]))
# Transform-only change: origin moves, cells identical, revision advances.
TEAM0_R3 = replace(TEAM0_R2, revision=3, origin_x_raw=q24(-3), origin_y_raw=q24(12))
# Revision-only change: identical content, revision bump is permitted.
TEAM7_R2 = replace(TEAM7_R1, revision=2)

BASE_EVENTS = [
    Event(0, TEAM0_R1),
    Event(0, TEAM7_R1),
    Event(2, TEAM0_R2),
    Event(3, TEAM7_R1),   # repeated identical grid retains revision 1
    Event(4, TEAM0_R3),
    Event(5, TEAM7_R2),
]

# Sensitivity variant: identical except one tick-0 team-0 cell (x=0, y=0).
ONE_CELL_EVENTS = [Event(0, replace(TEAM0_R1, cells=bytes([1, 64, 128, 192, 255, 17])))] + BASE_EVENTS[1:]
ZERO_TICK_EVENTS = [Event(0, TEAM0_R1), Event(0, TEAM7_R1)]


def event_audit(sidecar: bytes, events: list[Event]) -> list[dict[str, object]]:
    items = []
    offset = SIDECAR_HEADER_SIZE
    for index, event in enumerate(events):
        encoded = grid_bytes(event.grid)
        grid_offset = offset + 16
        assert sidecar[grid_offset:grid_offset + len(encoded)] == encoded
        items.append({
            "index": index,
            "offset": offset,
            "tick_offset": offset,
            "length_offset": offset + 8,
            "grid_offset": grid_offset,
            "grid_length": len(encoded),
            "cells_offset": grid_offset + GRID_HEADER_SIZE,
            "completed_tick": event.tick,
            "team_id": event.grid.team_id,
            "width": event.grid.width,
            "height": event.grid.height,
            "origin_raw": [event.grid.origin_x_raw, event.grid.origin_y_raw],
            "cell_raw": [event.grid.cell_x_raw, event.grid.cell_y_raw],
            "revision": event.grid.revision,
            "cells": list(event.grid.cells),
            "grid_sha256": hashlib.sha256(encoded).hexdigest(),
        })
        offset = grid_offset + len(encoded)
    assert offset == len(sidecar)
    return items


def mutations(replay_sha: bytes, final_tick: int) -> list[tuple[str, str, str, bytes]]:
    """(name, stage, expected diagnostic code, bytes). Stage parse or bind."""
    base = sidecar_bytes(replay_sha, final_tick, BASE_EVENTS)
    tiny = [Grid(team, 1, 1, 0, 0, Q24, Q24, 1, bytes([team & 0xFF])) for team in range(65)]
    items: list[tuple[str, str, str, bytes]] = [
        ("bad-magic", "parse", CODE_MALFORMED,
         sidecar_bytes(replay_sha, final_tick, BASE_EVENTS, magic=b"EAWRFGX\x00")),
        ("sidecar-version", "parse", CODE_VERSION,
         sidecar_bytes(replay_sha, final_tick, BASE_EVENTS, version=2)),
        ("truncated-header", "parse", CODE_MALFORMED, base[:SIDECAR_HEADER_SIZE - 1]),
        ("truncated-cells", "parse", CODE_MALFORMED, base[:-1]),
        ("trailing-byte", "parse", CODE_MALFORMED, base + b"\x00"),
        ("event-count-exceeds-bytes", "parse", CODE_MALFORMED,
         sidecar_bytes(replay_sha, final_tick, BASE_EVENTS, event_count=len(BASE_EVENTS) + 1)),
        ("event-count-zero", "parse", CODE_VALUE,
         sidecar_bytes(replay_sha, final_tick, [])),
        ("event-count-limit", "parse", CODE_LIMIT,
         sidecar_bytes(replay_sha, final_tick, BASE_EVENTS, event_count=1_000_001)),
        ("final-tick-limit", "parse", CODE_LIMIT,
         sidecar_bytes(replay_sha, 1_000_001, BASE_EVENTS)),
        ("grid-length-mismatch", "parse", CODE_MALFORMED,
         sidecar_bytes(replay_sha, final_tick,
                       [replace(BASE_EVENTS[0], length_override=GRID_HEADER_SIZE + 5)] + BASE_EVENTS[1:])),
        ("grid-bad-magic", "parse", CODE_MALFORMED, _patch(base, SIDECAR_HEADER_SIZE + 16, b"EAWRFOX\x00")),
        ("grid-schema-version", "parse", CODE_VERSION,
         _first(replay_sha, final_tick, replace(TEAM0_R1, schema_version=2))),
        ("grid-encoding-zero", "parse", CODE_VERSION,
         _first(replay_sha, final_tick, replace(TEAM0_R1, encoding=0))),
        ("grid-width-zero", "parse", CODE_VALUE,
         _first(replay_sha, final_tick, replace(TEAM0_R1, width=0, byte_count=6))),
        ("grid-width-limit", "parse", CODE_LIMIT,
         _first(replay_sha, final_tick, replace(TEAM0_R1, width=4097, byte_count=6))),
        ("grid-cell-zero", "parse", CODE_VALUE,
         _first(replay_sha, final_tick, replace(TEAM0_R1, cell_x_raw=0))),
        ("grid-cell-negative", "parse", CODE_VALUE,
         _first(replay_sha, final_tick, replace(TEAM0_R1, cell_y_raw=-Q24))),
        ("grid-origin-overflow", "parse", CODE_OVERFLOW,
         _first(replay_sha, final_tick, replace(TEAM0_R1, origin_x_raw=INT64_MAX - 5 * Q24))),
        ("grid-extent-product-overflow", "parse", CODE_OVERFLOW,
         _first(replay_sha, final_tick, replace(TEAM0_R1, cell_y_raw=INT64_MAX // 2 + 1))),
        ("grid-revision-zero", "parse", CODE_VALUE,
         _first(replay_sha, final_tick, replace(TEAM0_R1, revision=0))),
        ("grid-byte-count-mismatch", "parse", CODE_MALFORMED,
         _first(replay_sha, final_tick, replace(TEAM0_R1, byte_count=7))),
        ("team-order", "parse", CODE_ORDER,
         sidecar_bytes(replay_sha, final_tick, [BASE_EVENTS[1], BASE_EVENTS[0]] + BASE_EVENTS[2:])),
        ("duplicate-tick-team", "parse", CODE_ORDER,
         sidecar_bytes(replay_sha, final_tick, BASE_EVENTS[:2] + [Event(2, TEAM0_R2), Event(2, TEAM0_R2)])),
        ("tick-order", "parse", CODE_ORDER,
         sidecar_bytes(replay_sha, final_tick, BASE_EVENTS[:2] + [BASE_EVENTS[3], BASE_EVENTS[2]])),
        ("team-missing-at-tick-zero", "parse", CODE_ORDER,
         sidecar_bytes(replay_sha, final_tick, BASE_EVENTS + [Event(5, replace(TEAM7_R1, team_id=9))])),
        ("no-tick-zero", "parse", CODE_ORDER,
         sidecar_bytes(replay_sha, final_tick, [Event(1, TEAM0_R1)])),
        ("tick-beyond-final", "parse", CODE_ORDER,
         sidecar_bytes(replay_sha, final_tick, BASE_EVENTS[:2] + [Event(final_tick + 1, TEAM0_R2)])),
        ("first-revision-not-one", "parse", CODE_REVISION,
         sidecar_bytes(replay_sha, final_tick, [Event(0, replace(TEAM0_R1, revision=2))])),
        ("same-revision-different-cells", "parse", CODE_REVISION,
         sidecar_bytes(replay_sha, final_tick, BASE_EVENTS[:2] + [Event(2, replace(TEAM0_R2, revision=1))])),
        ("same-revision-different-origin", "parse", CODE_REVISION,
         sidecar_bytes(replay_sha, final_tick,
                       BASE_EVENTS[:2] + [Event(2, replace(TEAM0_R1, origin_x_raw=0))])),
        ("revision-decrease", "parse", CODE_REVISION,
         sidecar_bytes(replay_sha, final_tick,
                       BASE_EVENTS[:3] + [Event(3, replace(TEAM0_R1, revision=1))])),
        ("too-many-teams", "parse", CODE_LIMIT,
         sidecar_bytes(replay_sha, final_tick, [Event(0, grid) for grid in tiny])),
        ("replay-sha-mismatch", "bind", CODE_IDENTITY,
         sidecar_bytes(bytes(32), final_tick, BASE_EVENTS)),
        ("final-tick-mismatch", "bind", CODE_IDENTITY,
         sidecar_bytes(replay_sha, final_tick + 1, BASE_EVENTS)),
    ]
    names = [item[0] for item in items]
    assert len(names) == len(set(names))
    return items


def _patch(data: bytes, offset: int, value: bytes) -> bytes:
    return data[:offset] + value + data[offset + len(value):]


def _first(replay_sha: bytes, final_tick: int, grid: Grid) -> bytes:
    return sidecar_bytes(replay_sha, final_tick, [Event(0, grid)] + BASE_EVENTS[1:2])


def build() -> dict[str, bytes]:
    assert hashlib.sha256(b"abc").hexdigest() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    replay = ORIGINAL_REPLAY.read_bytes()
    assert len(replay) == ORIGINAL_BYTES
    assert hashlib.sha256(replay).hexdigest() == ORIGINAL_SHA256
    audit = json.loads(ORIGINAL_AUDIT.read_text(encoding="utf-8"))
    assert audit["fixture"]["sha256"] == ORIGINAL_SHA256
    final_tick = struct.unpack_from("<Q", replay, 40)[0]
    assert final_tick == 5
    audit_ticks = audit["ticks"]
    assert [row["completed_tick"] for row in audit_ticks] == list(range(1, final_tick + 1))
    world = [tick0_state_hash(replay)] + [row["state_sha256"] for row in audit_ticks]

    zero_replay = ZERO_TICK_REPLAY.read_bytes()
    zero_world = [tick0_state_hash(zero_replay)]
    replay_sha = hashlib.sha256(replay).digest()
    zero_sha = hashlib.sha256(zero_replay).digest()

    outputs: dict[str, bytes] = {}
    fixtures = {
        "fog-stub-v1": (replay, world, sidecar_bytes(replay_sha, final_tick, BASE_EVENTS), BASE_EVENTS,
                        "original-v1.eawr-replay"),
        "fog-stub-v1-one-cell": (replay, world, sidecar_bytes(replay_sha, final_tick, ONE_CELL_EVENTS),
                                 ONE_CELL_EVENTS, "original-v1.eawr-replay"),
        "fog-stub-v1-zero-tick": (zero_replay, zero_world, sidecar_bytes(zero_sha, 0, ZERO_TICK_EVENTS),
                                  ZERO_TICK_EVENTS, "zero-tick-v1.eawr-replay"),
    }
    fixture_audit = {}
    grid_lines = ["fixture\tevent_index\tcompleted_tick\tteam_id\trevision\tgrid_offset\tgrid_length\tsha256\n"]
    for name, (replay_data, hashes, data, events, replay_name) in fixtures.items():
        rows = evidence_rows(replay_data, data, events, hashes)
        outputs[f"{name}.eawr-fog"] = data
        outputs[f"{name}.evidence.csv"] = csv_text(rows)
        events_audit = event_audit(data, events)
        for item in events_audit:
            grid_lines.append(
                f"{name}\t{item['index']}\t{item['completed_tick']}\t{item['team_id']}\t{item['revision']}"
                f"\t{item['grid_offset']}\t{item['grid_length']}\t{item['grid_sha256']}\n")
        fixture_audit[name] = {
            "file": f"{name}.eawr-fog",
            "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "replay": replay_name,
            "replay_sha256": hashlib.sha256(replay_data).hexdigest(),
            "header": {
                "magic": [0, 8], "sidecar_version": [8, 4], "replay_sha256": [12, 32],
                "final_completed_tick": [44, 8], "event_count": [52, 8],
            },
            "events": events_audit,
            "world_state_sha256": hashes,
            "evidence": [
                {"completed_tick": tick, "preimage_bytes": size, "sha256": digest}
                for tick, digest, size in rows
            ],
        }
    base_rows = [row["sha256"] for row in fixture_audit["fog-stub-v1"]["evidence"]]
    cell_rows = [row["sha256"] for row in fixture_audit["fog-stub-v1-one-cell"]["evidence"]]
    assert all(left != right for left, right in zip(base_rows, cell_rows))
    base_events = fixture_audit["fog-stub-v1"]["events"]
    cell_events = fixture_audit["fog-stub-v1-one-cell"]["events"]
    assert base_events[0]["grid_sha256"] != cell_events[0]["grid_sha256"]
    # Repeated identical grid at tick 3 leaves the active grid set unchanged.
    assert active_grids(BASE_EVENTS, 2) == active_grids(BASE_EVENTS, 3)
    outputs["fog-stub-v1.grids.tsv"] = "".join(grid_lines).encode("utf-8")
    world_lines = ["replay\tcompleted_tick\tstate_sha256\n"]
    world_lines += [f"original-v1.eawr-replay\t{tick}\t{digest}\n" for tick, digest in enumerate(world)]
    world_lines += [f"zero-tick-v1.eawr-replay\t{tick}\t{digest}\n" for tick, digest in enumerate(zero_world)]
    outputs["fog-stub-v1.world.tsv"] = "".join(world_lines).encode("utf-8")

    mutation_lines = ["file\tstage\tcode\n"]
    mutation_audit = []
    for name, stage, code, data in mutations(replay_sha, final_tick):
        file_name = f"mutations/{name}.eawr-fog"
        outputs[file_name] = data
        mutation_lines.append(f"{file_name}\t{stage}\t{code}\n")
        mutation_audit.append({"file": file_name, "stage": stage, "code": code, "bytes": len(data)})
    outputs["fog-stub-v1.mutations.tsv"] = "".join(mutation_lines).encode("utf-8")

    document = {
        "contract": "docs/replay-format.md#synthetic-fog-stub-v1-fixture-sidecar-p1-07",
        "policy": "synthetic Phase 1 fog stub; not retail fog semantics",
        "grid_header": {
            "magic": [0, 8], "schema_version": [8, 4], "team_id": [12, 4], "width": [16, 4],
            "height": [20, 4], "origin_x_raw": [24, 8], "origin_y_raw": [32, 8],
            "cell_x_raw": [40, 8], "cell_y_raw": [48, 8], "encoding": [56, 4],
            "revision": [60, 8], "byte_count": [68, 8], "cells": [76, "byte_count"],
        },
        "original_v1": {"bytes": ORIGINAL_BYTES, "sha256": ORIGINAL_SHA256,
                        "tick0_state_sha256": world[0],
                        "ticks_1_to_5_source": "tests/replay/fixtures/original-v1.audit.json"},
        "zero_tick_v1": {"tick0_state_sha256": zero_world[0]},
        "fixtures": fixture_audit,
        "mutations": mutation_audit,
    }
    outputs["fog-stub-v1.audit.json"] = (json.dumps(document, indent=2, sort_keys=True) + "\n").encode("utf-8")
    return outputs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    outputs = build()
    if args.check:
        stale = []
        for name, data in sorted(outputs.items()):
            path = OUT / name
            if not path.is_file():
                stale.append(name)
                continue
            actual = path.read_bytes()
            if name.endswith((".csv", ".tsv", ".json")):
                # Text goldens may be checked out with CRLF under core.autocrlf.
                actual = actual.replace(b"\r\n", b"\n")
            if actual != data:
                stale.append(name)
        expected = {name for name in outputs if name.startswith("mutations/")}
        present = {f"mutations/{path.name}" for path in (OUT / "mutations").glob("*.eawr-fog")}
        stale.extend(sorted(present - expected))
        if stale:
            print("stale fog fixtures: " + ", ".join(stale), file=sys.stderr)
            return 1
        print(f"fog fixtures current ({len(outputs)} files)")
        return 0
    (OUT / "mutations").mkdir(exist_ok=True)
    for name, data in outputs.items():
        (OUT / name).write_bytes(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
