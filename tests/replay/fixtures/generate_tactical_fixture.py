#!/usr/bin/env python3
"""Generate the replay-v2 tactical fixtures and their independent oracle.

Standalone by design: it uses only Python's standard library, never imports or runs
project code, and implements the replay-v2 encoding, the tactical rules-v1 command
semantics, the canonical state and snapshot bytes, the Q24 quaternion-to-matrix rule,
the P2-05 sensor visibility, the derived FogGridV1 cells and the P2-09 hull and hardpoint
rules from docs/replay-format.md, docs/fixed-point.md, docs/behaviour/space-visibility.md
and docs/behaviour/space-hardpoints.md. Visibility is computed by brute force over every
observer, not through a spatial index; the durability rules use exact Python integers. Production output is
never blessed as expected data; the C++ reader, the session and sim_headless are
compared against the files written here.
"""

from __future__ import annotations

from dataclasses import dataclass, field, replace
from decimal import ROUND_HALF_EVEN, Decimal, getcontext
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys


OUT = Path(__file__).resolve().parent
NAME = "tactical-v2"
MAGIC = b"EAWRPLY\x00"
STATE_MAGIC = b"EAWRTST\x00"
SNAPSHOT_MAGIC = b"EAWRTSN\x00"
FORMAT_VERSION = 2
HEADER_SIZE = 104
RULES_VERSION = 1
STATE_ENCODING_VERSION = 1
SNAPSHOT_ENCODING_VERSION = 3
MATH_VERSION = 1
FRACTIONAL_BITS = 24
ONE = 1 << FRACTIONAL_BITS
TICK_NUMERATOR = 1
TICK_DENOMINATOR = 30
FINAL_TICK_COUNT = 6
SEED = 0x5EED0000_00000066
CONTENT_IDENTITY = hashlib.sha256(b"EAWR-P2-03-tactical-v2-synthetic-content\n").digest()
COMMANDABLE = 1

OPCODES = {"stop": 1, "move": 2, "attack": 3, "damage": 4}
ORDER_KINDS = {"none": 0, **OPCODES}
EVENT_KINDS = {"order_accepted": 1, "order_rejected": 2, "hardpoint_destroyed": 3, "unit_destroyed": 4}
REASONS = {
    "none": 0,
    "unit_not_live": 1,
    "unit_not_owned": 2,
    "target_not_live": 3,
    "target_not_hostile": 4,
    "not_damageable": 5,
    "hardpoint_invalid": 6,
}
HULL_TARGET = 0xFFFFFFFF


@dataclass(frozen=True)
class Player:
    player_id: int
    team_id: int
    faction_id: int
    flags: int


@dataclass
class Order:
    kind: str = "none"
    issued_tick: int = 0
    destination: tuple[int, int, int] = (0, 0, 0)
    target: int = 0


@dataclass
class Unit:
    entity_id: int
    type_id: int
    owner: int
    position: tuple[int, int, int]
    rotation: tuple[int, int, int, int]
    order: Order = field(default_factory=Order)


@dataclass(frozen=True)
class Command:
    tick: int
    player_id: int
    sequence: int
    kind: str
    units: tuple[int, ...]
    destination: tuple[int, int, int] = (0, 0, 0)
    target: int = 0
    amount: int = 0             # damage: Q24 raw
    hardpoint: int = HULL_TARGET  # damage: HardPoints index, or the hull


# --- Q24 helpers -------------------------------------------------------------------------


def q24(text: str) -> int:
    """Exact decimal text to Q24 raw, nearest-even."""
    return int((Decimal(text) * ONE).quantize(Decimal(1), rounding=ROUND_HALF_EVEN))


def vec(x: str, y: str, z: str) -> tuple[int, int, int]:
    return (q24(x), q24(y), q24(z))


def rne_shift(value: int) -> int:
    """value / 2^24 rounded to nearest, ties to even (symmetric for negatives)."""
    quotient, remainder = divmod(value, ONE)
    if remainder * 2 > ONE or (remainder * 2 == ONE and quotient % 2 == 1):
        quotient += 1
    return quotient


def decimal_pi() -> Decimal:
    return Decimal("3.14159265358979323846264338327950288419716939937510582097494459")


def decimal_sin_cos(angle: Decimal) -> tuple[Decimal, Decimal]:
    sine = Decimal(0)
    cosine = Decimal(0)
    term = Decimal(1)
    for n in range(0, 80):
        if n % 2 == 0:
            cosine += term if n % 4 == 0 else -term
        else:
            sine += term if n % 4 == 1 else -term
        term = term * angle / (n + 1)
    return sine, cosine


def yaw_quat(degrees: int) -> tuple[int, int, int, int]:
    """Right-handed rotation about +Z (turns-free: exact decimal authoring input only)."""
    getcontext().prec = 60
    half = Decimal(degrees) * decimal_pi() / Decimal(360)
    sine, cosine = decimal_sin_cos(half)
    raw = [
        0,
        0,
        int((sine * ONE).quantize(Decimal(1), rounding=ROUND_HALF_EVEN)),
        int((cosine * ONE).quantize(Decimal(1), rounding=ROUND_HALF_EVEN)),
    ]
    norm = sum(component * component for component in raw)
    assert abs(norm - ONE * ONE) <= 8 * ONE, (degrees, norm)
    return (raw[0], raw[1], raw[2], raw[3])


def to_matrix(rotation: tuple[int, int, int, int], position: tuple[int, int, int]) -> list[list[int]]:
    """docs/fixed-point.md: exact widened sums, one nearest-even rounding per component."""
    x, y, z, w = rotation
    norm = x * x + y * y + z * z + w * w
    if abs(norm - ONE * ONE) > 8 * ONE:
        raise ValueError("non-unit quaternion")

    def diagonal(a: int, b: int) -> int:
        return rne_shift(ONE * ONE - 2 * a * a - 2 * b * b)

    def pair(a: int, b: int, c: int, d: int, subtract: bool) -> int:
        second = 2 * c * d
        return rne_shift(2 * a * b + (-second if subtract else second))

    return [
        [diagonal(y, z), pair(x, y, z, w, True), pair(x, z, y, w, False), position[0]],
        [pair(x, y, z, w, False), diagonal(x, z), pair(y, z, x, w, True), position[1]],
        [pair(x, z, y, w, True), pair(y, z, x, w, False), diagonal(x, y), position[2]],
    ]


# --- Fixture content -----------------------------------------------------------------------


def fixture_players() -> tuple[Player, ...]:
    return (
        Player(1, 0, 0x1001, COMMANDABLE),  # Rebel slot, team 0
        Player(2, 1, 0x1002, COMMANDABLE),  # Empire slot, team 1
        Player(3, 3, 0x1003, 0),            # Neutral owner, not commandable
    )


def fixture_units() -> tuple[Unit, ...]:
    return (
        Unit(1, 101, 1, vec("-3811", "4460", "0"), yaw_quat(227)),
        Unit(2, 102, 1, vec("-4661", "5112", "0"), yaw_quat(336)),
        Unit(3, 102, 1, vec("-4600.5", "5050.25", "0"), yaw_quat(336)),
        Unit(4, 103, 1, vec("-4700", "5150", "-12.125"), yaw_quat(336)),
        Unit(5, 201, 2, vec("3685", "-4171", "0"), yaw_quat(45)),
        Unit(6, 202, 2, vec("4118", "-4976", "0"), yaw_quat(149)),
        Unit(7, 203, 2, vec("4180", "-4900", "0"), (0, 0, ONE, 0)),
        Unit(8, 204, 2, vec("4050", "-5040", "30.5"), yaw_quat(149)),
        Unit(9, 301, 3, vec("0", "0", "0"), (0, 0, 0, ONE)),
    )


def fixture_commands() -> tuple[Command, ...]:
    return (
        Command(0, 1, 1, "move", (2, 3), destination=vec("-1000", "1000", "0")),
        Command(0, 2, 1, "attack", (6, 7), target=2),
        Command(1, 1, 2, "attack", (2, 3, 4), target=5),
        Command(1, 1, 3, "move", (5, 6), destination=vec("0", "0", "0")),
        Command(1, 2, 2, "attack", (8,), target=7),
        Command(3, 1, 4, "stop", (2, 42)),
        Command(3, 2, 3, "attack", (6,), target=9),
        Command(3, 2, 4, "attack", (7,), target=77),
        Command(5, 1, 5, "move", (4,), destination=vec("-123.25", "456.5", "7.125")),
        Command(5, 2, 5, "stop", (6, 7, 8)),
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


# --- Tactical rules v1 -------------------------------------------------------------------


def execute(units: dict[int, Unit], teams: dict[int, int], command: Command) -> list[dict[str, object]]:
    issuer_team = teams[command.player_id]
    command_reason = "none"
    if command.kind == "attack":
        target = units.get(command.target)
        if target is None:
            command_reason = "target_not_live"
        elif teams[target.owner] == issuer_team:
            command_reason = "target_not_hostile"
    events = []
    for unit_id in command.units:
        reason = command_reason
        unit = units.get(unit_id)
        if reason == "none":
            if unit is None:
                reason = "unit_not_live"
            elif unit.owner != command.player_id:
                reason = "unit_not_owned"
        if reason == "none":
            assert unit is not None
            unit.order = Order(command.kind, command.tick, command.destination, command.target)
        events.append({
            "tick": command.tick,
            "player": command.player_id,
            "sequence": command.sequence,
            "unit": unit_id,
            "event": "order_accepted" if reason == "none" else "order_rejected",
            "order": command.kind,
            "reason": reason,
        })
    return events


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


def simulate(players, initial, commands) -> dict[str, object]:
    teams = {player.player_id: player.team_id for player in players}
    units = {unit.entity_id: replace(unit, order=Order()) for unit in initial}
    next_id = max(units) + 1 if units else 1
    rng_state = SEED
    # sim_headless binds no sensor table, so each unit is seen by its own team only.
    sensors: dict[int, int] = {}
    snapshots = [{"tick": 0, "sha256": hashlib.sha256(snapshot_bytes(players, units, 0, [], sensors)).hexdigest()}]
    hashes = []
    all_events = []
    ticks = []
    for tick in range(FINAL_TICK_COUNT):
        events = []
        for command in commands:
            if command.tick == tick:
                events.extend(execute(units, teams, command))
        completed = tick + 1
        state = state_bytes(players, units, completed, next_id, rng_state)
        state_hash = hashlib.sha256(state).hexdigest()
        snapshot = snapshot_bytes(players, units, completed, events, sensors)
        snapshot_hash = hashlib.sha256(snapshot).hexdigest()
        hashes.append({"tick": completed, "sha256": state_hash})
        snapshots.append({"tick": completed, "sha256": snapshot_hash})
        all_events.extend(events)
        ticks.append({
            "completed_tick": completed,
            "state_bytes": len(state),
            "state_sha256": state_hash,
            "snapshot_bytes": len(snapshot),
            "snapshot_sha256": snapshot_hash,
            "event_count": len(events),
            "orders": {
                str(unit_id): {
                    "kind": units[unit_id].order.kind,
                    "issued_tick": units[unit_id].order.issued_tick,
                    "destination_raw": list(units[unit_id].order.destination),
                    "target": units[unit_id].order.target,
                }
                for unit_id in sorted(units)
                if units[unit_id].order.kind != "none"
            },
        })
    return {"hashes": hashes, "snapshots": snapshots, "events": all_events, "ticks": ticks}


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



# --- P2-05 visibility fixture ------------------------------------------------------------
#
# Rules v1 moves nothing, so the approach and retreat of one enemy fighter is a sequence of
# one-tick replays ("frames"), each a setup with the fighter at the next position. Sensor
# ranges are the FoC Space_FOW_Reveal_Range values of the named types; the type IDs are
# synthetic.

VISIBILITY = "tactical-visibility"
VISIBILITY_SEED = 0x5EED0000_00000068
VISIBILITY_CONTENT = hashlib.sha256(b"EAWR-P2-05-tactical-visibility-synthetic-content\n").digest()
VISIBILITY_SENSORS = (
    (102, "Nebulon_B_Frigate", "1200"),
    (104, "X-Wing", "500"),
    (203, "TIE_Fighter", "500"),
    (301, "Orbital_Resource_Container", "100"),
)
FOG_LAYOUT = {
    "origin_x_raw": q24("-4096"), "origin_y_raw": q24("-4096"),
    "cell_x_raw": q24("256"), "cell_y_raw": q24("256"), "width": 32, "height": 32,
}
TIE = 5
FRIGATE = 1


def visibility_frames() -> tuple[tuple[str, tuple[int, int, int]], ...]:
    return (
        ("approaching, 2000 from the frigate", vec("2000", "0", "0")),
        ("one raw unit beyond the frigate's 1200", (q24("1200") + 1, 0, 0)),
        ("exactly 1200: seen, the boundary is inclusive", vec("1200", "0", "0")),
        ("exactly 500: the fighter's own 500 reveals the frigate", vec("500", "0", "0")),
        ("1100 in the plane and 700 above; 3D distance 1304 but Z is ignored", vec("1100", "0", "700")),
        ("diagonal (720, 960): exactly 1200", vec("720", "960", "0")),
        ("diagonal one raw unit beyond 1200: hidden again", (q24("720"), q24("960") + 1, 0)),
        ("retreating, 1513 from the frigate", vec("1500", "-200", "0")),
    )


def visibility_players() -> tuple[Player, ...]:
    return fixture_players()


def visibility_units(fighter: tuple[int, int, int]) -> tuple[Unit, ...]:
    return (
        Unit(1, 102, 1, vec("0", "0", "0"), (0, 0, 0, ONE)),        # Rebel frigate, 1200
        Unit(2, 104, 1, vec("-2500", "0", "0"), (0, 0, 0, ONE)),    # Rebel X-wing, 500
        Unit(TIE, 203, 2, fighter, (0, 0, ONE, 0)),                 # Empire TIE fighter, 500
        Unit(6, 299, 2, vec("-300", "0", "0"), (0, 0, ONE, 0)),     # Empire type without a sensor
        Unit(9, 301, 3, vec("0", "3000", "0"), (0, 0, 0, ONE)),     # neutral container, 100
    )


def build_visibility(outputs: dict[str, bytes]) -> None:
    players = visibility_players()
    sensors = {type_id: q24(reach) for type_id, _, reach in VISIBILITY_SENSORS}
    teams = sorted({player.team_id for player in players})
    bit = {player.player_id: index for index, player in enumerate(sorted(players, key=lambda p: p.player_id))}

    def text(name: str, content: str) -> None:
        outputs[name] = content.encode("utf-8")

    text(f"{VISIBILITY}.sensors.csv", "type_id,reveal_range_raw,foc_type\n" + "".join(
        f"{type_id},{q24(reach)},{name}\n" for type_id, name, reach in VISIBILITY_SENSORS))
    text(f"{VISIBILITY}.fog-layout.csv", "origin_x_raw,origin_y_raw,cell_x_raw,cell_y_raw,width,height\n"
         + ",".join(str(FOG_LAYOUT[key]) for key in (
             "origin_x_raw", "origin_y_raw", "cell_x_raw", "cell_y_raw", "width", "height")) + "\n")

    rows = []
    audit_frames = []
    for index, (label, fighter) in enumerate(visibility_frames()):
        initial = visibility_units(fighter)
        units = {unit.entity_id: replace(unit) for unit in initial}
        replay_name = f"{VISIBILITY}-frame-{index}.eawr-replay"
        outputs[replay_name] = replay_bytes(players, initial, (), 1, VISIBILITY_SEED, VISIBILITY_CONTENT)
        masks = visibility_masks(players, units, sensors)
        snapshot0 = hashlib.sha256(snapshot_bytes(players, units, 0, [], sensors)).hexdigest()
        next_id = max(units) + 1
        state1 = hashlib.sha256(
            state_bytes(players, units, 1, next_id, VISIBILITY_SEED, VISIBILITY_CONTENT)).hexdigest()
        snapshot1 = hashlib.sha256(snapshot_bytes(players, units, 1, [], sensors)).hexdigest()
        fog = {}
        lit = {}
        for team in teams:
            cells = fog_cells(players, units, sensors, team, FOG_LAYOUT)
            fog[team] = hashlib.sha256(fog_grid_bytes(team, FOG_LAYOUT, 1, cells)).hexdigest()
            lit[team] = sum(1 for cell in cells if cell == 255)
        rebel_sees_tie = (masks[TIE] >> bit[1]) & 1 == 1
        empire_sees_frigate = (masks[FRIGATE] >> bit[2]) & 1 == 1
        rows.append(",".join([
            str(index), replay_name, ";".join(str(value) for value in fighter),
            "yes" if rebel_sees_tie else "no", "yes" if empire_sees_frigate else "no",
            ";".join(f"{unit_id}:{masks[unit_id]}" for unit_id in sorted(masks)),
            snapshot0, state1, snapshot1, *(fog[team] for team in teams),
        ]) + "\n")
        audit_frames.append({
            "frame": index,
            "label": label,
            "replay": replay_name,
            "replay_sha256": hashlib.sha256(outputs[replay_name]).hexdigest(),
            "fighter_position_raw": list(fighter),
            "fighter_planar_distance_squared_raw": fighter[0] ** 2 + fighter[1] ** 2,
            "frigate_range_squared_raw": sensors[102] ** 2,
            "fighter_range_squared_raw": sensors[203] ** 2,
            "rebel_sees_fighter": rebel_sees_tie,
            "empire_sees_frigate": empire_sees_frigate,
            "visible_to_masks": {str(unit_id): masks[unit_id] for unit_id in sorted(masks)},
            "fog_lit_cells": {str(team): lit[team] for team in teams},
        })
    text(f"{VISIBILITY}.frames.csv",
         "frame,replay,fighter_position_raw,rebel_sees_fighter,empire_sees_frigate,masks,"
         "snapshot0_sha256,state1_sha256,snapshot1_sha256,"
         + ",".join(f"fog_team_{team}_sha256" for team in teams) + "\n" + "".join(rows))
    audit = {
        "contract": "docs/behaviour/space-visibility.md",
        "players": [player.__dict__ for player in players],
        "mask_bits": {str(player_id): position for player_id, position in bit.items()},
        "seed": f"0x{VISIBILITY_SEED:016x}",
        "content_identity": VISIBILITY_CONTENT.hex(),
        "sensors": [
            {"type_id": type_id, "foc_type": name, "Space_FOW_Reveal_Range": reach, "reveal_range_raw": q24(reach)}
            for type_id, name, reach in VISIBILITY_SENSORS
        ],
        "units_without_sensor": [299],
        "fog_layout": FOG_LAYOUT,
        "fog_revision": 1,
        "frames": audit_frames,
    }
    text(f"{VISIBILITY}.audit.json", json.dumps(audit, indent=2) + "\n")


# --- P2-09 hull and hardpoint durability ---------------------------------------------------

DURABILITY = "tactical-durability"
DURABILITY_SEED = 0x5EED0000_00000072
DURABILITY_CONTENT = hashlib.sha256(b"EAWR-P2-09-tactical-durability-foc-shaped-content\n").digest()
DURABILITY_TICKS = 6
ROLES = {"other": 0, "weapon": 1, "engine": 2, "shield_generator": 3, "fighter_bay": 4, "special_ability": 5}
STATES = {"intact": 0, "damaged": 1, "destroyed": 2}


def rne_div(numerator: int, denominator: int) -> int:
    """numerator / denominator (both >= 0) rounded to nearest, ties to even."""
    quotient, remainder = divmod(numerator, denominator)
    if 2 * remainder > denominator or (2 * remainder == denominator and quotient % 2 == 1):
        quotient += 1
    return quotient


@dataclass(frozen=True)
class HardpointSpec:
    role: str
    health: int = 0          # Q24 raw maximum; zero when not destroyable
    repair_amount: int = 0
    repair_cost: int = 0

    @property
    def destroyable(self) -> bool:
        return self.health > 0


@dataclass(frozen=True)
class ProfileSpec:
    type_id: int
    name: str
    hull: int
    max_speed: int | None
    hardpoints: tuple[HardpointSpec, ...]
    dies_with_hardpoints: bool = False


def scaled(value: str) -> int:
    """FoC Health or Tactical_Health x Object_Max_Health_Multiplier_Space 1.5, as Q24 raw."""
    return rne_div(q24(value) * q24("1.5"), ONE)


def durability_profiles() -> tuple[ProfileSpec, ...]:
    """FoC data values (hardpoints.xml, spaceunitsfrigates.xml, spaceunitscorvettes.xml,
    starbases.xml); type IDs are fixture-local."""
    def hp(role: str, health: str = "0", repair: str = "0", cost: str = "0") -> HardpointSpec:
        return HardpointSpec(role, scaled(health) if health != "0" else 0, q24(repair), q24(cost))
    return (
        ProfileSpec(10, "Nebulon_B_Frigate", scaled("3600"), q24("2.2"),
                    (hp("weapon", "260"),) * 4 + (hp("engine", "260"),)),
        ProfileSpec(20, "Acclamator_Assault_Ship", scaled("2000"), q24("2.2"),
                    (hp("weapon", "140"), hp("weapon", "140"), hp("weapon", "160"), hp("weapon", "140"),
                     hp("weapon", "140"), hp("weapon", "160"), hp("engine", "170"), hp("fighter_bay", "100"))),
        ProfileSpec(30, "Corellian_Corvette", scaled("750"), q24("3.1"), (hp("weapon"),) * 8),
        ProfileSpec(40, "Skirmish_Empire_Star_Base_1", scaled("1600"), None,
                    (hp("special_ability", "450", ".50", "1.3"), hp("special_ability", "450", ".50", "1.3"),
                     hp("weapon", "650", ".50", "1.3"), hp("weapon", "450", ".50", "1.3"),
                     hp("weapon", "450", ".50", "1.3"), hp("shield_generator", "650", ".50", "1.5"),
                     hp("fighter_bay", "1000", ".50", "1.5"))),
    )


class Durability:
    """Hull and hardpoint health of the durable units, with rules HD and HS of the note."""

    def __init__(self, profiles: tuple[ProfileSpec, ...], units: dict[int, Unit]) -> None:
        self.constraint = q24("0.2")      # Hull_Vs_Hard_Points_Health_Constraint
        self.engines_off = q24("0.4")     # Engines_Disabled_Speed_Modifier
        self.damaged = q24("0.33")        # Health_Low_Percent_Threshold
        self.profiles = {profile.type_id: profile for profile in profiles}
        self.health: dict[int, list[int]] = {}   # entity -> [hull, hardpoint...]
        for unit in units.values():
            profile = self.profiles.get(unit.type_id)
            if profile is not None:
                self.health[unit.entity_id] = [profile.hull] + [spec.health for spec in profile.hardpoints]

    def profile(self, unit: Unit) -> ProfileSpec | None:
        return self.profiles.get(unit.type_id) if unit.entity_id in self.health else None

    def destroyed(self, unit_id: int, profile: ProfileSpec, index: int) -> bool:
        return profile.hardpoints[index].destroyable and self.health[unit_id][1 + index] == 0

    def role_alive(self, unit_id: int, profile: ProfileSpec, role: str, when_absent: bool) -> bool:
        indices = [i for i, spec in enumerate(profile.hardpoints) if spec.role == role]
        if not indices:
            return when_absent
        return any(not self.destroyed(unit_id, profile, i) for i in indices)

    def damage(self, unit_id: int, profile: ProfileSpec, target: int, amount: int) -> tuple[int | None, bool]:
        """HD-02, HD-03, HD-20, HD-21: returns (destroyed hardpoint, unit destroyed)."""
        health = self.health[unit_id]
        if amount <= 0:
            return None, False
        if target == HULL_TARGET:
            if health[0] <= 0:
                return None, False
            health[0] = max(0, health[0] - amount)
            return None, health[0] == 0
        if health[1 + target] <= 0:
            return None, False
        health[1 + target] = max(0, health[1 + target] - amount)
        if health[1 + target] > 0:
            return None, False
        dead = False
        if profile.dies_with_hardpoints and not any(
                spec.destroyable and health[1 + i] > 0 for i, spec in enumerate(profile.hardpoints)):
            health[0] = 0
            dead = True
        return target, dead

    def service(self, unit_id: int, profile: ProfileSpec) -> tuple[list[int], bool]:
        """HS-01 to HS-04 over exact integers: returns (destroyed hardpoints, unit destroyed)."""
        health = self.health[unit_id]
        maxima = [spec.health for spec in profile.hardpoints]
        t = sum(value for spec, value in zip(profile.hardpoints, maxima) if spec.destroyable)
        s = sum(health[1 + i] for i, spec in enumerate(profile.hardpoints) if spec.destroyable)
        if t == 0 or health[0] <= 0:
            return [], False
        m, c, k = profile.hull, self.constraint, ONE
        if profile.dies_with_hardpoints:
            cap = min(m, rne_div(s * m * k + c * m * t, t * k))
            if health[0] > cap:
                health[0] = cap
                if cap == 0:
                    return [], True
        h = health[0]
        current = s * m * k
        limit = min(t * m * k, t * h * k + t * c * m)
        if current <= limit:
            return [], False
        excess = current - limit
        before = list(health)
        destroyed = []
        for i, spec in enumerate(profile.hardpoints):
            if not spec.destroyable or before[1 + i] <= 0:
                continue
            loss = rne_div(excess * before[1 + i], m * k * t)
            if loss <= 0:
                continue
            health[1 + i] = max(0, before[1 + i] - loss)
            if health[1 + i] == 0:
                destroyed.append(i)
        return destroyed, False

    def state_bytes(self, unit: Unit) -> bytes:
        if unit.entity_id not in self.health:
            return b""
        health = self.health[unit.entity_id]
        return struct.pack("<qII", health[0], len(health) - 1, 0) + b"".join(struct.pack("<q", v) for v in health[1:])

    def instance_bytes(self, unit: Unit) -> bytes:
        profile = self.profile(unit)
        if profile is None:
            return struct.pack("<II", 0, 0)
        health = self.health[unit.entity_id]
        engines = self.role_alive(unit.entity_id, profile, "engine", True)
        shields = self.role_alive(unit.entity_id, profile, "shield_generator", True)
        launch = self.role_alive(unit.entity_id, profile, "fighter_bay", False)
        factor = ONE if engines else self.engines_off
        speed = rne_div(profile.max_speed * factor, ONE) if profile.max_speed is not None else 0
        flags = (1 if engines else 0) | (2 if shields else 0) | (4 if launch else 0) | (8 if profile.max_speed is not None else 0)
        data = struct.pack("<IIqqqqII", 1, len(profile.hardpoints), health[0], profile.hull, factor, speed, flags, 0)
        for i, spec in enumerate(profile.hardpoints):
            value = health[1 + i]
            if not spec.destroyable:
                state = "intact"
            elif value == 0:
                state = "destroyed"
            elif value * ONE < spec.health * self.damaged:
                state = "damaged"
            else:
                state = "intact"
            enabled = not self.destroyed(unit.entity_id, profile, i)
            data += struct.pack("<qBBBBI", value, ROLES[spec.role], STATES[state], 1 if enabled else 0, 0, 0)
        return data


def durability_players() -> tuple[Player, ...]:
    return (Player(1, 0, 0x1001, COMMANDABLE), Player(2, 1, 0x1002, COMMANDABLE))


def durability_units() -> tuple[Unit, ...]:
    return (
        Unit(1, 10, 1, vec("-3000", "4000", "0"), yaw_quat(315)),    # Rebel Nebulon-B
        Unit(2, 20, 2, vec("3000", "-3500", "0"), yaw_quat(135)),    # Empire Acclamator
        Unit(3, 30, 1, vec("-3200", "4100", "0"), yaw_quat(315)),    # Rebel Corellian corvette
        Unit(4, 40, 2, vec("3685", "-4171", "0"), yaw_quat(45)),     # Empire station
        Unit(5, 50, 1, vec("-3100", "4300", "0"), yaw_quat(315)),    # Rebel squadron: no profile
    )


def durability_commands() -> tuple[Command, ...]:
    """Player 2 stands in for the script host (HD-30); every hit names its hardpoint index."""
    def hit(tick: int, sequence: int, units: tuple[int, ...], amount: str, hardpoint: int = HULL_TARGET) -> Command:
        return Command(tick, 2, sequence, "damage", units, amount=q24(amount), hardpoint=hardpoint)
    return (
        hit(0, 1, (1,), "400", 4),          # Nebulon engines (390): destroyed
        hit(0, 2, (1,), "200", 0),          # Nebulon weapon FL: 190 left, intact
        hit(1, 3, (1,), "100", 0),          # 90 left: damaged
        hit(1, 4, (2,), "150", 7),          # Acclamator fighter bay (150): destroyed
        hit(1, 5, (4,), "975", 5),          # station shield generator (975): destroyed
        hit(2, 6, (1,), "90", 0),           # weapon FL: destroyed
        hit(2, 7, (3, 5), "10", 0),         # corvette weapon: not destroyable; squadron: no profile
        hit(2, 8, (1,), "10", 9),           # no hardpoint 9
        hit(3, 9, (2,), "2000"),            # Acclamator hull to a third: hardpoints pulled down
        hit(4, 10, (2,), "1000"),           # hull to zero: the Acclamator dies
        Command(5, 1, 1, "attack", (1,), target=2),   # target_not_live
        Command(5, 2, 11, "stop", (2,)),              # unit_not_live
    )


def simulate_durability(players, initial, commands, profiles) -> dict[str, object]:
    teams = {player.player_id: player.team_id for player in players}
    units = {unit.entity_id: replace(unit, order=Order()) for unit in initial}
    durability = Durability(profiles, units)
    next_id = max(units) + 1
    sensors: dict[int, int] = {}
    snapshots = [{"tick": 0, "sha256": hashlib.sha256(
        snapshot_bytes(players, units, 0, [], sensors, durability)).hexdigest()}]
    hashes, all_events, ticks = [], [], []

    def destruction(tick: int, kind: str, unit: Unit, hardpoint: int = 0) -> dict[str, object]:
        return {"tick": tick, "player": unit.owner, "sequence": 0, "unit": unit.entity_id, "event": kind,
                "order": "none", "reason": "none", "hardpoint": hardpoint}

    for tick in range(DURABILITY_TICKS):
        events = []
        for command in commands:
            if command.tick != tick:
                continue
            if command.kind != "damage":
                events.extend(execute(units, teams, command))
                continue
            for unit_id in command.units:
                unit = units.get(unit_id)
                profile = durability.profile(unit) if unit is not None else None
                if unit is None:
                    reason = "unit_not_live"
                elif profile is None:
                    reason = "not_damageable"
                elif command.hardpoint != HULL_TARGET and (
                        command.hardpoint >= len(profile.hardpoints)
                        or not profile.hardpoints[command.hardpoint].destroyable):
                    reason = "hardpoint_invalid"
                else:
                    reason = "none"
                events.append({"tick": tick, "player": command.player_id, "sequence": command.sequence,
                               "unit": unit_id, "event": "order_accepted" if reason == "none" else "order_rejected",
                               "order": "damage", "reason": reason})
                if reason != "none":
                    continue
                destroyed, dead = durability.damage(unit_id, profile, command.hardpoint, command.amount)
                if destroyed is not None:
                    events.append(destruction(tick, "hardpoint_destroyed", unit, destroyed))
                if dead:
                    events.append(destruction(tick, "unit_destroyed", unit))
                    del units[unit_id]
                    del durability.health[unit_id]
        for unit_id in sorted(units):
            profile = durability.profile(units[unit_id])
            if profile is None:
                continue
            destroyed, dead = durability.service(unit_id, profile)
            events.extend(destruction(tick, "hardpoint_destroyed", units[unit_id], i) for i in destroyed)
            if dead:
                events.append(destruction(tick, "unit_destroyed", units[unit_id]))
                del units[unit_id]
                del durability.health[unit_id]
        completed = tick + 1
        state = state_bytes(players, units, completed, next_id, DURABILITY_SEED, DURABILITY_CONTENT, durability)
        snapshot = snapshot_bytes(players, units, completed, events, sensors, durability)
        hashes.append({"tick": completed, "sha256": hashlib.sha256(state).hexdigest()})
        snapshots.append({"tick": completed, "sha256": hashlib.sha256(snapshot).hexdigest()})
        all_events.extend(events)
        ticks.append({
            "completed_tick": completed,
            "state_sha256": hashes[-1]["sha256"],
            "snapshot_sha256": snapshots[-1]["sha256"],
            "health_raw": {str(unit_id): list(values) for unit_id, values in sorted(durability.health.items())},
            "live_units": sorted(units),
        })
    return {"hashes": hashes, "snapshots": snapshots, "events": all_events, "ticks": ticks}


def build_durability(outputs: dict[str, bytes]) -> None:
    players = durability_players()
    units = durability_units()
    commands = durability_commands()
    profiles = durability_profiles()
    outputs[f"{DURABILITY}.eawr-replay"] = replay_bytes(
        players, units, commands, DURABILITY_TICKS, DURABILITY_SEED, DURABILITY_CONTENT)
    result = simulate_durability(players, units, commands, profiles)

    def text(name: str, content: str) -> None:
        outputs[name] = content.encode("utf-8")

    rows = [f"rules,0,0,0,{q24('0.2')},{q24('0.4')},{q24('0.33')}\n"]
    for profile in profiles:
        speed = "none" if profile.max_speed is None else str(profile.max_speed)
        rows.append(f"hull,{profile.type_id},0,0,{profile.hull},{speed},{1 if profile.dies_with_hardpoints else 0}\n")
        for spec in profile.hardpoints:
            rows.append(f"hardpoint,{profile.type_id},{ROLES[spec.role]},{1 if spec.destroyable else 0},"
                        f"{spec.health},{spec.repair_amount},{spec.repair_cost}\n")
    text(f"{DURABILITY}.table.csv",
         "kind,type_id,role,destroyable,max_raw,repair_amount_raw,repair_cost_raw\n" + "".join(rows))
    text(f"{DURABILITY}.hashes.csv",
         "tick,sha256\n" + "".join(f"{row['tick']},{row['sha256']}\n" for row in result["hashes"]))
    text(f"{DURABILITY}.snapshots.csv",
         "tick,sha256\n" + "".join(f"{row['tick']},{row['sha256']}\n" for row in result["snapshots"]))
    text(f"{DURABILITY}.events.csv",
         "tick,player,sequence,unit,event,order,reason,hardpoint\n" + "".join(
             f"{e['tick']},{e['player']},{e['sequence']},{e['unit']},{e['event']},{e['order']},{e['reason']},"
             f"{e.get('hardpoint', 0)}\n" for e in result["events"]))
    audit = {
        "contract": "docs/behaviour/space-hardpoints.md",
        "seed": f"0x{DURABILITY_SEED:016x}",
        "content_identity": DURABILITY_CONTENT.hex(),
        "replay_sha256": hashlib.sha256(outputs[f"{DURABILITY}.eawr-replay"]).hexdigest(),
        "rules_raw": {"hull_vs_hardpoints": q24("0.2"), "engines_disabled_speed": q24("0.4"),
                      "damaged_fraction": q24("0.33")},
        "profiles": [
            {"type_id": profile.type_id, "foc_type": profile.name, "hull_raw": profile.hull,
             "max_speed_raw": profile.max_speed,
             "hardpoints": [{"role": spec.role, "health_raw": spec.health, "repair_amount_raw": spec.repair_amount,
                             "repair_cost_raw": spec.repair_cost} for spec in profile.hardpoints]}
            for profile in profiles
        ],
        "nebulon_max_speed_without_engines_raw": rne_div(q24("2.2") * q24("0.4"), ONE),
        "ticks": result["ticks"],
    }
    text(f"{DURABILITY}.audit.json", json.dumps(audit, indent=2) + "\n")


def build() -> dict[str, bytes]:
    """Every generated file, by name. Text files are UTF-8 with LF lines."""
    players = fixture_players()
    units = fixture_units()
    commands = fixture_commands()
    data = replay_bytes(players, units, commands, FINAL_TICK_COUNT)
    result = simulate(players, units, commands)
    outputs: dict[str, bytes] = {f"{NAME}.eawr-replay": data}

    def text(name: str, content: str) -> None:
        outputs[name] = content.encode("utf-8")

    text(f"{NAME}.hashes.csv",
        "tick,sha256\n" + "".join(f"{row['tick']},{row['sha256']}\n" for row in result["hashes"]))
    text(f"{NAME}.snapshots.csv",
        "tick,sha256\n" + "".join(f"{row['tick']},{row['sha256']}\n" for row in result["snapshots"]))
    text(f"{NAME}.events.csv",
        "tick,player,sequence,unit,event,order,reason\n" + "".join(
            f"{e['tick']},{e['player']},{e['sequence']},{e['unit']},{e['event']},{e['order']},{e['reason']}\n"
            for e in result["events"]))

    mutation_meta = []
    for name, mutated, code, change in mutations(data, players, units, commands):
        filename = f"{NAME}-mutated-{name}.eawr-replay"
        outputs[filename] = mutated
        mutation_meta.append({"file": filename, "code": code, "change": change})
    text(f"{NAME}.mutations.json", json.dumps({"mutations": mutation_meta}, indent=2) + "\n")

    offsets = command_offsets(players, units, commands)
    audit = {
        "fixture": {
            "file": f"{NAME}.eawr-replay",
            "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "header_offsets": {
                "magic": [0, 8], "format_version": [8, 2], "header_size": [10, 2],
                "tactical_rules_version": [12, 4], "math_version": [16, 4], "fractional_bits": [20, 4],
                "tick_numerator": [24, 4], "tick_denominator": [28, 4], "seed": [32, 8],
                "final_tick_count": [40, 8], "player_count": [48, 4], "reserved": [52, 4],
                "unit_count": [56, 8], "command_count": [64, 8], "content_identity": [72, 32],
            },
            "player_table": [HEADER_SIZE, 24 * len(players)],
            "unit_table": [HEADER_SIZE + 24 * len(players), 80 * len(units)],
            "command_offsets": offsets,
        },
        "header": {
            "format_version": FORMAT_VERSION, "tactical_rules_version": RULES_VERSION,
            "tick": f"{TICK_NUMERATOR}/{TICK_DENOMINATOR}", "seed": f"0x{SEED:016x}",
            "final_tick_count": FINAL_TICK_COUNT, "content_identity": CONTENT_IDENTITY.hex(),
        },
        "players": [player.__dict__ for player in players],
        "units": [
            {"entity_id": unit.entity_id, "type_id": unit.type_id, "owner": unit.owner,
             "position_raw": list(unit.position), "rotation_raw": list(unit.rotation),
             "matrix_raw": to_matrix(unit.rotation, unit.position)}
            for unit in units
        ],
        "commands": [
            {"offset": offset, "body_length": len(command_body(command)), "tick": command.tick,
             "player": command.player_id, "sequence": command.sequence, "kind": command.kind,
             "units": list(command.units), "destination_raw": list(command.destination),
             "target": command.target}
            for offset, command in zip(offsets, commands)
        ],
        "next_entity_id": max(unit.entity_id for unit in units) + 1,
        "rng_state": f"0x{SEED:016x}",
        "ticks": result["ticks"],
    }
    text(f"{NAME}.audit.json", json.dumps(audit, indent=2) + "\n")
    build_visibility(outputs)
    build_durability(outputs)
    return outputs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true",
                        help="regenerate into memory and fail if any checked-in fixture differs")
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
            if name.endswith((".csv", ".json")):
                # Text goldens may be checked out with CRLF under core.autocrlf.
                actual = actual.replace(b"\r\n", b"\n")
            if actual != data:
                stale.append(name)
        present = {path.name for path in OUT.glob(f"{NAME}-mutated-*.eawr-replay")}
        present |= {path.name for path in OUT.glob(f"{VISIBILITY}-frame-*.eawr-replay")}
        stale.extend(sorted(present - set(outputs)))
        if stale:
            print("stale tactical fixtures: " + ", ".join(stale), file=sys.stderr)
            return 1
        print(f"tactical fixtures current ({len(outputs)} files)")
        return 0
    for name, data in outputs.items():
        (OUT / name).write_bytes(data)
    data = outputs[f"{NAME}.eawr-replay"]
    print(json.dumps({"fixture": f"{NAME}.eawr-replay", "bytes": len(data),
                      "sha256": hashlib.sha256(data).hexdigest()}, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
