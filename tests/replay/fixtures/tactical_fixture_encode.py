"""Independent tactical fixture encode helpers; no production simulation imports."""
from __future__ import annotations
from dataclasses import dataclass, field, replace
from decimal import ROUND_HALF_EVEN, Decimal, getcontext
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

from tactical_fixture_types import (
    OUT,
    NAME,
    MAGIC,
    STATE_MAGIC,
    SNAPSHOT_MAGIC,
    FORMAT_VERSION,
    HEADER_SIZE,
    RULES_VERSION,
    STATE_ENCODING_VERSION,
    SNAPSHOT_ENCODING_VERSION,
    MATH_VERSION,
    FRACTIONAL_BITS,
    ONE,
    TICK_NUMERATOR,
    TICK_DENOMINATOR,
    FINAL_TICK_COUNT,
    SEED,
    CONTENT_IDENTITY,
    COMMANDABLE,
    OPCODES,
    ORDER_KINDS,
    EVENT_KINDS,
    REASONS,
    HULL_TARGET,
    Player,
    Order,
    Unit,
    Command,
    q24,
    vec,
    rne_shift,
    decimal_pi,
    decimal_sin_cos,
    yaw_quat,
    to_matrix,
    VISIBILITY,
    VISIBILITY_SEED,
    VISIBILITY_CONTENT,
    VISIBILITY_SENSORS,
    FOG_LAYOUT,
    TIE,
    FRIGATE,
    DURABILITY,
    DURABILITY_SEED,
    DURABILITY_CONTENT,
    DURABILITY_TICKS,
    ROLES,
    STATES,
    rne_div,
    HardpointSpec,
    ProfileSpec,
    scaled,
)




# --- Encoding ----------------------------------------------------------------------------


def player_bytes(player: Player) -> bytes:
    return struct.pack("<IIQII", player.player_id, player.team_id, player.faction_id, player.flags, 0)





def unit_bytes(unit: Unit) -> bytes:
    return struct.pack("<QQII3q4q", unit.entity_id, unit.type_id, unit.owner, 0, *unit.position, *unit.rotation)





def order_bytes(order: Order) -> bytes:
    return struct.pack("<QII3qQ", order.issued_tick, OPCODES.get(order.kind, 0), 0, *order.destination, order.target)





def command_body(command: Command) -> bytes:
    body = struct.pack("<QIQBBH", command.tick, command.player_id, command.sequence, OPCODES[command.kind], 0, 0)
    if command.kind == "move":
        body += struct.pack("<3q", *command.destination)
    elif command.kind == "attack":
        body += struct.pack("<Q", command.target)
    elif command.kind == "damage":
        body += struct.pack("<qII", command.amount, command.hardpoint, 0)
    body += struct.pack("<II", len(command.units), 0)
    body += b"".join(struct.pack("<Q", unit) for unit in command.units)
    return body





def command_bytes(command: Command) -> bytes:
    body = command_body(command)
    return struct.pack("<I", len(body)) + body





def header_bytes(players: int, units: int, commands: int, final_tick_count: int,
                 seed: int = SEED, content: bytes = CONTENT_IDENTITY) -> bytes:
    header = MAGIC + struct.pack(
        "<HHIIIIIQQIIQQ",
        FORMAT_VERSION,
        HEADER_SIZE,
        RULES_VERSION,
        MATH_VERSION,
        FRACTIONAL_BITS,
        TICK_NUMERATOR,
        TICK_DENOMINATOR,
        seed,
        final_tick_count,
        players,
        0,
        units,
        commands,
    ) + content
    assert len(header) == HEADER_SIZE
    return header





def replay_bytes(players, units, commands, final_tick_count: int,
                 seed: int = SEED, content: bytes = CONTENT_IDENTITY) -> bytes:
    return (
        header_bytes(len(players), len(units), len(commands), final_tick_count, seed, content)
        + b"".join(player_bytes(player) for player in players)
        + b"".join(unit_bytes(unit) for unit in units)
        + b"".join(command_bytes(command) for command in commands)
    )





def event_bytes(event: dict[str, object]) -> bytes:
    return struct.pack(
        "<QIBBBBQQ",
        event["tick"],
        event["player"],
        EVENT_KINDS[event["event"]],
        ORDER_KINDS[event["order"]],
        REASONS[event["reason"]],
        event.get("hardpoint", 0),
        event["sequence"],
        event["unit"],
    )





def state_bytes(players, units: dict[int, Unit], completed_tick: int, next_id: int, rng_state: int,
                content: bytes = CONTENT_IDENTITY, durability: "Durability | None" = None) -> bytes:
    data = STATE_MAGIC + struct.pack(
        "<IIIIII", STATE_ENCODING_VERSION, RULES_VERSION, MATH_VERSION, FRACTIONAL_BITS,
        TICK_NUMERATOR, TICK_DENOMINATOR,
    )
    data += struct.pack("<QQQ", completed_tick, next_id, rng_state) + content
    data += struct.pack("<Q", len(players)) + b"".join(player_bytes(player) for player in players)
    data += struct.pack("<Q", len(units))
    for unit_id in sorted(units):
        data += unit_bytes(units[unit_id]) + order_bytes(units[unit_id].order)
        if durability is not None:
            data += durability.state_bytes(units[unit_id])
    return data





# --- P2-05 sensor visibility -----------------------------------------------------------------


def visibility_masks(players, units: dict[int, Unit], sensors: dict[int, int]) -> dict[int, int]:
    """Bit k = k-th player by ascending ID. A unit is seen by its own team, and by every team
    with a unit whose type has a sensor profile at planar (XY) distance <= its reveal range."""
    ordered = sorted(players, key=lambda player: player.player_id)
    team_of = {player.player_id: player.team_id for player in ordered}
    team_bits: dict[int, int] = {}
    for index, player in enumerate(ordered):
        team_bits[player.team_id] = team_bits.get(player.team_id, 0) | (1 << index)
    masks = {}
    for unit in units.values():
        mask = team_bits[team_of[unit.owner]]
        for observer in units.values():
            reach = sensors.get(observer.type_id)
            if reach is None:
                continue
            dx = unit.position[0] - observer.position[0]
            dy = unit.position[1] - observer.position[1]
            if dx * dx + dy * dy <= reach * reach:
                mask |= team_bits[team_of[observer.owner]]
        masks[unit.entity_id] = mask
    return masks





def snapshot_bytes(players, units: dict[int, Unit], completed_tick: int, events, sensors: dict[int, int],
                   durability: "Durability | None" = None) -> bytes:
    ordered = sorted(players, key=lambda player: player.player_id)
    teams = {player.player_id: player.team_id for player in ordered}
    masks = visibility_masks(ordered, units, sensors)
    data = SNAPSHOT_MAGIC + struct.pack("<IQQ", SNAPSHOT_ENCODING_VERSION, completed_tick, len(ordered))
    data += b"".join(struct.pack("<II", player.player_id, player.team_id) for player in ordered)
    data += struct.pack("<Q", len(units))
    for unit_id in sorted(units):
        unit = units[unit_id]
        matrix = to_matrix(unit.rotation, unit.position)
        data += struct.pack("<QQII", unit.entity_id, unit.type_id, unit.owner, teams[unit.owner])
        data += struct.pack("<12q", *(value for row in matrix for value in row))
        reach = sensors.get(unit.type_id)
        data += struct.pack("<QIIq", masks[unit_id], 0 if reach is None else 1, 0, reach or 0)
        data += durability.instance_bytes(unit) if durability is not None else struct.pack("<II", 0, 0)
    data += struct.pack("<Q", len(events)) + b"".join(event_bytes(event) for event in events)
    return data





def fog_grid_bytes(team: int, layout: dict[str, int], revision: int, cells: bytes) -> bytes:
    return b"EAWRFOG\x00" + struct.pack(
        "<IIIIqqqqIQQ", 1, team, layout["width"], layout["height"], layout["origin_x_raw"],
        layout["origin_y_raw"], layout["cell_x_raw"], layout["cell_y_raw"], 1, revision, len(cells),
    ) + cells





def fog_cells(players, units: dict[int, Unit], sensors: dict[int, int], team: int, layout: dict[str, int]) -> bytes:
    """255 where the cell centre (lower corner + floor(cell / 2) raw) lies within the planar
    reveal range of a unit of `team`, else 0; row-major y*width+x."""
    team_of = {player.player_id: player.team_id for player in players}
    observers = [
        (unit.position[0], unit.position[1], sensors[unit.type_id])
        for unit in units.values()
        if team_of[unit.owner] == team and unit.type_id in sensors
    ]
    cells = bytearray()
    for y in range(layout["height"]):
        centre_y = layout["origin_y_raw"] + y * layout["cell_y_raw"] + layout["cell_y_raw"] // 2
        for x in range(layout["width"]):
            centre_x = layout["origin_x_raw"] + x * layout["cell_x_raw"] + layout["cell_x_raw"] // 2
            lit = any((centre_x - ox) ** 2 + (centre_y - oy) ** 2 <= reach * reach for ox, oy, reach in observers)
            cells.append(255 if lit else 0)
    return bytes(cells)





# --- Mutations ---------------------------------------------------------------------------


def command_offsets(players, units, commands) -> list[int]:
    offset = HEADER_SIZE + 24 * len(players) + 80 * len(units)
    offsets = []
    for command in commands:
        offsets.append(offset)
        offset += len(command_bytes(command))
    return offsets





def mutations(data: bytes, players, units, commands) -> list[tuple[str, bytes, str, str]]:
    offsets = command_offsets(players, units, commands)
    unit_base = HEADER_SIZE + 24 * len(players)
    result = []

    def put(buffer: bytearray, offset: int, fmt: str, value: int) -> None:
        struct.pack_into(fmt, buffer, offset, value)

    rules = bytearray(data)
    put(rules, 12, "<I", 2)
    result.append(("rules", bytes(rules), "EAWR-SIM-0302", "tactical rules version at offset 12 changed from 1 to 2"))

    rate = bytearray(data)
    put(rate, 28, "<I", 60)
    result.append(("tick-rate", bytes(rate), "EAWR-SIM-0302", "tick denominator at offset 28 changed from 30 to 60"))

    flags = bytearray(data)
    put(flags, HEADER_SIZE + 16, "<I", 3)
    result.append(("player-flags", bytes(flags), "EAWR-SIM-0302", "player 1 flags changed from 1 to 3 (unknown bit)"))

    owner = bytearray(data)
    put(owner, unit_base + 8 * 80 + 16, "<I", 4)
    result.append(("owner", bytes(owner), "EAWR-SIM-0305", "unit 9 owner changed from 3 to undeclared player 4"))

    rotation = bytearray(data)
    put(rotation, unit_base + 8 * 80 + 72, "<q", 0)
    result.append(("rotation", bytes(rotation), "EAWR-SIM-0305", "unit 9 rotation w changed from 1 to 0 (zero quaternion)"))

    first, second = command_bytes(commands[0]), command_bytes(commands[1])
    order = data[: offsets[0]] + second + first + data[offsets[0] + len(first) + len(second):]
    result.append(("order", order, "EAWR-SIM-0304", "first two command records swapped; (tick, player, sequence) decreases"))

    issuer = bytearray(data)
    put(issuer, offsets[-1] + 4 + 8, "<I", 3)
    result.append(("issuer", bytes(issuer), "EAWR-SIM-0306", "last command issuer changed from player 2 to non-commandable player 3"))

    unit_order = bytearray(data)
    list_start = offsets[2] + 4 + 24 + 8 + 8
    put(unit_order, list_start, "<Q", 3)
    put(unit_order, list_start + 8, "<Q", 2)
    result.append(("unit-order", bytes(unit_order), "EAWR-SIM-0308", "command 2 unit list changed from 2,3,4 to 3,2,4"))

    length = bytearray(data)
    put(length, offsets[0], "<I", len(command_body(commands[0])) + 8)
    result.append(("length", bytes(length), "EAWR-SIM-0301", "command 0 body length increased by 8"))

    count = bytearray(data)
    put(count, 64, "<Q", len(commands) + 1)
    result.append(("count", bytes(count), "EAWR-SIM-0301", "command_count at offset 64 incremented by one"))

    result.append(("trailing", data + b"\x00", "EAWR-SIM-0301", "one zero byte appended after the final command"))
    return result
