#!/usr/bin/env python3
"""Generate the original P0-04 replay-v1 fixture and its independent oracle.

This file is deliberately standalone: it uses only Python's standard library,
does not import project code, and implements the small Q24/replay reference
model independently of any future C++ reader or simulation.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct
from typing import Iterable


OUT = Path(__file__).resolve().parent
MAGIC = b"EAWRPLY\x00"
STATE_MAGIC = b"EAWRSTA\x00"
FORMAT_VERSION = 1
HEADER_SIZE = 96
SIM_RULES_VERSION = 1
MATH_VERSION = 1
FRACTIONAL_BITS = 24
TICK_NUMERATOR = 1
TICK_DENOMINATOR = 30
FINAL_TICK_COUNT = 5
SEED = 0x0123456789ABCDEF
CONTENT_IDENTITY_TEXT = b"EAWR-P0-04-original-v1-content\n"
CONTENT_IDENTITY = hashlib.sha256(CONTENT_IDENTITY_TEXT).digest()
MASK64 = (1 << 64) - 1
INT64_MIN = -(1 << 63)
INT64_MAX = (1 << 63) - 1
SPLITMIX_INCREMENT = 0x9E3779B97F4A7C15
SPLITMIX_MUL1 = 0xBF58476D1CE4E5B9
SPLITMIX_MUL2 = 0x94D049BB133111EB
ENTITY_SIZE = 64
COMMAND_COMMON_SIZE = 24


@dataclass(frozen=True)
class Entity:
    entity_id: int
    asset_id: int
    position: tuple[int, int, int]
    velocity: tuple[int, int, int]


@dataclass(frozen=True)
class Command:
    tick: int
    player_id: int
    sequence: int
    opcode: int
    payload: object


def hex_u64(value: int) -> str:
    return f"0x{value:016x}"


def entity_bytes(entity: Entity) -> bytes:
    return struct.pack(
        "<QQ6q",
        entity.entity_id,
        entity.asset_id,
        *entity.position,
        *entity.velocity,
    )


def command_payload(command: Command) -> bytes:
    if command.opcode == 1:
        assert isinstance(command.payload, Entity)
        return entity_bytes(command.payload)
    if command.opcode == 2:
        assert isinstance(command.payload, int)
        return struct.pack("<Q", command.payload)
    if command.opcode in (3, 4):
        entity_id, values = command.payload
        return struct.pack("<Q3q", entity_id, *values)
    raise AssertionError(f"unknown fixture opcode {command.opcode}")


def command_body(command: Command) -> bytes:
    common = struct.pack(
        "<QIQBBH",
        command.tick,
        command.player_id,
        command.sequence,
        command.opcode,
        0,
        0,
    )
    return common + command_payload(command)


def command_bytes(command: Command) -> bytes:
    body = command_body(command)
    return struct.pack("<I", len(body)) + body


def header_bytes(final_tick_count: int, initial_count: int, command_count: int) -> bytes:
    header = struct.pack(
        "<8sHHIIIIIQQQQ32s",
        MAGIC,
        FORMAT_VERSION,
        HEADER_SIZE,
        SIM_RULES_VERSION,
        MATH_VERSION,
        FRACTIONAL_BITS,
        TICK_NUMERATOR,
        TICK_DENOMINATOR,
        SEED,
        final_tick_count,
        initial_count,
        command_count,
        CONTENT_IDENTITY,
    )
    assert len(header) == HEADER_SIZE
    return header


def replay_bytes(
    initial_entities: tuple[Entity, ...],
    commands: tuple[Command, ...],
    final_tick_count: int = FINAL_TICK_COUNT,
) -> bytes:
    assert all(command_bytes(left) and (left.tick, left.player_id, left.sequence) < (right.tick, right.player_id, right.sequence)
               for left, right in zip(commands, commands[1:]))
    return (
        header_bytes(final_tick_count, len(initial_entities), len(commands))
        + b"".join(entity_bytes(entity) for entity in initial_entities)
        + b"".join(command_bytes(command) for command in commands)
    )


def nearest_even_ratio(numerator: int, denominator: int) -> int:
    """Round numerator/denominator to nearest integer, ties to even."""

    assert denominator > 0
    sign = -1 if numerator < 0 else 1
    quotient, remainder = divmod(abs(numerator), denominator)
    twice = remainder * 2
    if twice > denominator or (twice == denominator and quotient & 1):
        quotient += 1
    return sign * quotient


def checked_add(left: int, right: int) -> int:
    value = left + right
    if not INT64_MIN <= value <= INT64_MAX:
        raise OverflowError("checked int64 position add")
    return value


def splitmix64(state: int) -> tuple[int, int]:
    state = (state + SPLITMIX_INCREMENT) & MASK64
    z = state
    z = ((z ^ (z >> 30)) * SPLITMIX_MUL1) & MASK64
    z = ((z ^ (z >> 27)) * SPLITMIX_MUL2) & MASK64
    return state, (z ^ (z >> 31)) & MASK64


def apply_command(world: dict[int, Entity], next_id: int, command: Command) -> int:
    if command.opcode == 1:
        assert isinstance(command.payload, Entity)
        entity = command.payload
        assert entity.entity_id == next_id
        assert entity.entity_id not in world
        world[entity.entity_id] = entity
        return next_id + 1
    if command.opcode == 2:
        assert isinstance(command.payload, int)
        assert command.payload in world
        del world[command.payload]
        return next_id
    entity_id, values = command.payload
    assert entity_id in world
    current = world[entity_id]
    if command.opcode == 3:
        world[entity_id] = Entity(current.entity_id, current.asset_id, tuple(values), current.velocity)
    elif command.opcode == 4:
        world[entity_id] = Entity(current.entity_id, current.asset_id, current.position, tuple(values))
    else:
        raise AssertionError(f"unknown fixture opcode {command.opcode}")
    return next_id


def pending_bytes(commands: tuple[Command, ...], completed_tick: int) -> bytes:
    return b"".join(command_bytes(command) for command in commands if command.tick >= completed_tick)


def canonical_state(
    completed_tick: int,
    final_tick_count: int,
    next_id: int,
    rng_state: int,
    tick_nonce: int,
    world: dict[int, Entity],
    commands: tuple[Command, ...],
) -> bytes:
    pending = pending_bytes(commands, completed_tick)
    encoded = bytearray()
    encoded += STATE_MAGIC
    encoded += struct.pack(
        "<IIIIIIQQQQQ32sQ",
        1,
        SIM_RULES_VERSION,
        MATH_VERSION,
        FRACTIONAL_BITS,
        TICK_NUMERATOR,
        TICK_DENOMINATOR,
        completed_tick,
        final_tick_count,
        next_id,
        rng_state,
        tick_nonce,
        CONTENT_IDENTITY,
        len(world),
    )
    for entity in (world[entity_id] for entity_id in sorted(world)):
        encoded += entity_bytes(entity)
    encoded += struct.pack("<Q", sum(1 for command in commands if command.tick >= completed_tick))
    encoded += pending
    return bytes(encoded)


def entity_json(entity: Entity) -> dict[str, object]:
    return {
        "entity_id": entity.entity_id,
        "asset_id": entity.asset_id,
        "position_raw": list(entity.position),
        "velocity_raw": list(entity.velocity),
    }


def command_json(index: int, command: Command, offset: int) -> dict[str, object]:
    encoded = command_bytes(command)
    payload_offset = offset + 4 + COMMAND_COMMON_SIZE
    item: dict[str, object] = {
        "index": index,
        "offset": offset,
        "body_offset": offset + 4,
        "body_length": len(encoded) - 4,
        "payload_offset": payload_offset,
        "payload_length": len(encoded) - 4 - COMMAND_COMMON_SIZE,
        "tick": command.tick,
        "player_id": command.player_id,
        "sequence": command.sequence,
        "opcode": command.opcode,
    }
    if command.opcode == 1:
        item["entity"] = entity_json(command.payload)
    elif command.opcode == 2:
        item["entity_id"] = command.payload
    else:
        item["entity_id"] = command.payload[0]
        item["values_raw"] = list(command.payload[1])
    return item


def fixture_commands() -> tuple[Command, ...]:
    # Same-sequence commands from different players are intentionally ordered
    # by player ID. Tick 0's p2/p3 position writes make that order observable.
    return (
        Command(0, 1, 1, 3, (1, (5, 0, 0))),
        Command(0, 2, 1, 3, (1, (7, 0, 0))),
        Command(0, 2, 2, 4, (1, (45, 0, 0))),
        Command(0, 2, 3, 4, (2, (45, 0, 0))),
        Command(0, 3, 1, 3, (1, (9, 0, 0))),
        Command(0, 3, 2, 4, (1, (-45, 0, 0))),
        Command(0, 3, 3, 1, Entity(3, 3003, (100, 0, 0), (75, 0, 0))),
        Command(1, 1, 1, 2, 2),
        Command(1, 2, 1, 1, Entity(4, 4004, (-20, 0, 0), (-75, 0, 0))),
        Command(1, 3, 1, 4, (3, (-75, 0, 0))),
        Command(1, 3, 2, 4, (4, (45, 0, 0))),
        Command(2, 1, 1, 3, (3, (500, 0, 0))),
        Command(2, 2, 1, 4, (3, (45, 0, 0))),
        Command(2, 3, 1, 1, Entity(5, 5005, (-100, 0, 0), (-45, 0, 0))),
        Command(3, 1, 1, 2, 1),
        Command(3, 2, 1, 2, 3),
        Command(3, 3, 1, 3, (4, (-50, 0, 0))),
        Command(4, 1, 1, 4, (4, (45, 0, 0))),
        Command(4, 2, 1, 1, Entity(6, 6006, (42, 0, 0), (0, 0, 0))),
        Command(4, 3, 1, 4, (6, (-45, 0, 0))),
    )


def simulate(initial: tuple[Entity, ...], commands: tuple[Command, ...]) -> list[dict[str, object]]:
    world = {entity.entity_id: entity for entity in initial}
    next_id = max(world, default=0) + 1
    rng_state = SEED
    rows: list[dict[str, object]] = []
    for tick in range(FINAL_TICK_COUNT):
        for command in commands:
            if command.tick == tick:
                next_id = apply_command(world, next_id, command)
        rng_state, tick_nonce = splitmix64(rng_state)
        moved: dict[int, Entity] = {}
        for entity_id in sorted(world):
            entity = world[entity_id]
            displacement = tuple(
                nearest_even_ratio(value * TICK_NUMERATOR, TICK_DENOMINATOR)
                for value in entity.velocity
            )
            position = tuple(
                checked_add(value, delta)
                for value, delta in zip(entity.position, displacement)
            )
            moved[entity_id] = Entity(entity.entity_id, entity.asset_id, position, entity.velocity)
        world = moved
        completed_tick = tick + 1
        canonical = canonical_state(
            completed_tick,
            FINAL_TICK_COUNT,
            next_id,
            rng_state,
            tick_nonce,
            world,
            commands,
        )
        rows.append({
            "completed_tick": completed_tick,
            "next_entity_id": next_id,
            "rng_state": hex_u64(rng_state),
            "tick_nonce": hex_u64(tick_nonce),
            "entities": [entity_json(world[entity_id]) for entity_id in sorted(world)],
            "pending_command_indexes": [
                index for index, command in enumerate(commands) if command.tick >= completed_tick
            ],
            "canonical_state_bytes": len(canonical),
            "state_sha256": hashlib.sha256(canonical).hexdigest(),
        })
    return rows


def mutation_bytes(data: bytes, command_info: list[dict[str, object]]) -> dict[str, bytes]:
    mutations: dict[str, bytes] = {}

    length = bytearray(data)
    struct.pack_into("<I", length, int(command_info[0]["offset"]), 0)
    mutations["mutated-length-zero.eawr-replay"] = bytes(length)

    version = bytearray(data)
    struct.pack_into("<H", version, 8, 2)
    mutations["mutated-version.eawr-replay"] = bytes(version)

    order = bytearray(data)
    first = int(command_info[0]["offset"])
    second = int(command_info[1]["offset"])
    end = second + 4 + int(command_info[1]["body_length"])
    first_bytes = bytes(order[first:second])
    second_bytes = bytes(order[second:end])
    order[first:end] = second_bytes + first_bytes
    mutations["mutated-order.eawr-replay"] = bytes(order)

    count = bytearray(data)
    original_count = struct.unpack_from("<Q", count, 56)[0]
    struct.pack_into("<Q", count, 56, original_count + 1)
    mutations["mutated-count.eawr-replay"] = bytes(count)

    mutations["mutated-trailing.eawr-replay"] = data + b"\x00"

    overflow = bytearray(data)
    for info in command_info:
        if info["tick"] == 0 and info["player_id"] == 3 and info["sequence"] == 1:
            struct.pack_into("<q", overflow, int(info["payload_offset"]) + 8, INT64_MAX)
        if info["tick"] == 0 and info["player_id"] == 3 and info["sequence"] == 2:
            struct.pack_into("<q", overflow, int(info["payload_offset"]) + 8, 45)
    mutations["mutated-overflow.eawr-replay"] = bytes(overflow)
    return mutations


def main() -> None:
    assert hashlib.sha256(b"").hexdigest() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
    assert hashlib.sha256(b"abc").hexdigest() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"

    initial = (
        Entity(1, 1001, (0, 0, 0), (0, 0, 0)),
        Entity(2, 2002, (20, 0, 0), (0, 0, 0)),
    )
    commands = fixture_commands()
    data = replay_bytes(initial, commands)
    fixture_path = OUT / "original-v1.eawr-replay"
    fixture_path.write_bytes(data)

    command_start = HEADER_SIZE + len(initial) * ENTITY_SIZE
    command_info: list[dict[str, object]] = []
    offset = command_start
    for index, command in enumerate(commands):
        info = command_json(index, command, offset)
        command_info.append(info)
        offset += 4 + int(info["body_length"])
    assert offset == len(data)

    rows = simulate(initial, commands)
    audit = {
        "fixture": {
            "file": fixture_path.name,
            "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "header_offsets": {
                "magic": [0, 8], "format_version": [8, 2], "header_size": [10, 2],
                "sim_rules_version": [12, 4], "math_version": [16, 4],
                "fractional_bits": [20, 4], "tick_numerator": [24, 4],
                "tick_denominator": [28, 4], "seed": [32, 8],
                "final_tick_count": [40, 8], "initial_entity_count": [48, 8],
                "command_count": [56, 8], "content_identity": [64, 32],
            },
            "hex_prefix_128_bytes": data[:128].hex(),
        },
        "header": {
            "format_version": FORMAT_VERSION,
            "header_size": HEADER_SIZE,
            "sim_rules_version": SIM_RULES_VERSION,
            "math_version": MATH_VERSION,
            "fractional_bits": FRACTIONAL_BITS,
            "tick_numerator": TICK_NUMERATOR,
            "tick_denominator": TICK_DENOMINATOR,
            "seed": hex_u64(SEED),
            "final_tick_count": FINAL_TICK_COUNT,
            "initial_entity_count": len(initial),
            "command_count": len(commands),
            "content_identity": CONTENT_IDENTITY.hex(),
            "content_identity_input_sha256": hashlib.sha256(CONTENT_IDENTITY_TEXT).hexdigest(),
        },
        "initial_entities": [entity_json(entity) for entity in initial],
        "commands": command_info,
        "rounding_examples": [
            {"velocity_raw": 45, "ratio": "45/30", "rounded_displacement_raw": 2},
            {"velocity_raw": -45, "ratio": "-45/30", "rounded_displacement_raw": -2},
            {"velocity_raw": 75, "ratio": "75/30", "rounded_displacement_raw": 2},
            {"velocity_raw": -75, "ratio": "-75/30", "rounded_displacement_raw": -2},
        ],
        "ticks": rows,
        "sha256_known_vectors": {
            "empty": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            "abc": "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        },
        "zero_tick_fixture": {
            "file": "zero-tick-v1.eawr-replay",
            "bytes": len(replay_bytes((), (), 0)),
            "sha256": hashlib.sha256(replay_bytes((), (), 0)).hexdigest(),
            "expected_output": "header-only UTF-8 hash output (no post-tick rows)",
        },
    }
    with (OUT / "original-v1.audit.json").open(
        "w", encoding="utf-8", newline="\n"
    ) as output:
        output.write(json.dumps(audit, indent=2, ensure_ascii=False) + "\n")

    zero_tick = replay_bytes((), (), 0)
    (OUT / "zero-tick-v1.eawr-replay").write_bytes(zero_tick)

    mutation_meta = [
        {
            "file": "mutated-length-zero.eawr-replay",
            "kind": "length",
            "change": "first command body length at offset 224 changed to zero",
            "expected": "parser rejects before command decode; no runtime behavior inferred",
        },
        {
            "file": "mutated-version.eawr-replay",
            "kind": "version",
            "change": "format_version at offset 8 changed from 1 to 2",
            "expected": "parser rejects unsupported format version",
        },
        {
            "file": "mutated-order.eawr-replay",
            "kind": "order",
            "change": "first two command records swapped; (tick,player,sequence) decreases",
            "expected": "parser rejects strict command ordering violation",
        },
        {
            "file": "mutated-count.eawr-replay",
            "kind": "count",
            "change": "command_count at offset 56 incremented from 20 to 21",
            "expected": "parser rejects missing command bytes after bounded count validation",
        },
        {
            "file": "mutated-trailing.eawr-replay",
            "kind": "trailing",
            "change": "one zero byte appended after the final command",
            "expected": "parser rejects trailing bytes after the declared command table",
        },
        {
            "file": "mutated-overflow.eawr-replay",
            "kind": "overflow",
            "change": "tick-0 player-3 position becomes INT64_MAX and velocity becomes +45 raw",
            "expected": "structural parse remains valid; checked movement add must fail atomically at tick 0",
        },
    ]
    mutations = mutation_bytes(data, command_info)
    for filename, mutated in mutations.items():
        (OUT / filename).write_bytes(mutated)
    with (OUT / "original-v1.mutations.json").open(
        "w", encoding="utf-8", newline="\n"
    ) as output:
        output.write(json.dumps({"mutations": mutation_meta}, indent=2) + "\n")

    # Freeze generated values in the audit itself so reruns detect accidental
    # changes to the schedule, encoder, or reference arithmetic.
    assert hashlib.sha256(fixture_path.read_bytes()).hexdigest() == audit["fixture"]["sha256"]
    assert len(data) == 224 + sum(4 + int(item["body_length"]) for item in command_info)
    print(json.dumps({
        "fixture": fixture_path.name,
        "bytes": len(data),
        "sha256": audit["fixture"]["sha256"],
        "ticks": len(rows),
        "zero_tick_sha256": audit["zero_tick_fixture"]["sha256"],
    }, sort_keys=True))


if __name__ == "__main__":
    main()
