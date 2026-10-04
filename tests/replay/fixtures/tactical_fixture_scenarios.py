"""Independent tactical fixture scenarios helpers; no production simulation imports."""
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
from tactical_fixture_encode import (
    player_bytes,
    unit_bytes,
    order_bytes,
    command_body,
    command_bytes,
    header_bytes,
    replay_bytes,
    event_bytes,
    state_bytes,
    visibility_masks,
    snapshot_bytes,
    fog_grid_bytes,
    fog_cells,
    command_offsets,
    mutations,
)
from tactical_fixture_simulate import (
    execute,
    simulate,
    Durability,
    simulate_durability,
)




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
