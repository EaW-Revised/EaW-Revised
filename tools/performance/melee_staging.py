"""Export a native melee replay to private retail Lua staging inputs (#1262).

This is a deliberately narrow reader of the engine's unextended squadron replay
format, not a replay validator or a runtime loader. Generate the input with
path_bench --melee s --seed 601 --ticks 1 --replay-out <file>. Type and faction
names come from a supplied catalog; the default catalog covers the benchmark's
own source roster. No retail data or native implementation is copied.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import zlib

ROOT = Path(__file__).resolve().parents[2]
SCALE = 1 << 24


def type_id(name: str) -> int:
    return zlib.crc32(name.upper().encode("ascii"))


def benchmark_catalog() -> dict:
    source = (ROOT / "src/skirmish/melee.cpp").read_text(encoding="utf-8")
    names = re.findall(r'\{\s*"([A-Za-z0-9_-]+)"\s*,\s*\d+\s*\}', source)
    if not names:
        raise ValueError("native benchmark roster was not found; supply --catalog")
    return {"types": names, "factions": ["Rebel", "Empire"]}


def read_fixture(blob: bytes, catalog: dict) -> dict:
    """Retain native ordering, positions, squadron members and attack payloads."""
    offset = 0

    def take(fmt: str):
        nonlocal offset
        size = struct.calcsize("<" + fmt)
        if offset + size > len(blob):
            raise ValueError("truncated melee replay")
        values = struct.unpack_from("<" + fmt, blob, offset)
        offset += size
        return values

    magic, version, header, _rules, math, bits, numerator, denominator = take("8sHH5I")
    if (magic, version, header, math, bits, numerator, denominator) != (
            b"EAWRPLY\0", 3, 104, 1, 24, 1, 30):
        raise ValueError("expected an unextended Q24, 30 Hz squadron replay")
    seed, ticks, nplayers, nsquadrons, nunits, ncommands, identity = take("QQIIQQ32s")
    if not ticks or not nsquadrons or not nunits or not ncommands:
        raise ValueError("melee replay must contain units, squadrons and orders")
    if nplayers * 24 + nunits * 80 + nsquadrons * 16 + ncommands * 36 > len(blob) - offset:
        raise ValueError("declared tables exceed replay bytes")
    factions = {type_id(n): n for n in catalog["factions"]}
    types = {type_id(n): n for n in catalog["types"]}
    if len(types) != len(catalog["types"]) or len(factions) != len(catalog["factions"]):
        raise ValueError("duplicate or colliding catalog names")
    players = {}
    for _ in range(nplayers):
        player, _team, faction, flags, reserved = take("IIQII")
        if reserved or player in players:
            raise ValueError("invalid player table")
        players[player] = {"faction": factions.get(faction), "commandable": bool(flags & 1)}
    units = {}
    for _ in range(nunits):
        entity, kind, owner, reserved, *pose = take("QQII7q")
        if reserved or entity in units or owner not in players:
            raise ValueError("invalid unit table")
        units[entity] = {"id": entity, "type_id": kind, "type": types.get(kind),
                         "owner": owner, "position": [v / SCALE for v in pose[:3]],
                         "rotation": [v / SCALE for v in pose[3:]], "members": []}
    members = set()
    for _ in range(nsquadrons):
        container, count, reserved = take("QII")
        if reserved or container not in units or not count or units[container]["members"]:
            raise ValueError("invalid squadron table")
        ids = list(take("Q" * count)) if count <= (len(blob) - offset) // 8 else []
        if len(ids) != count or len(set(ids)) != count or any(
                i not in units or i == container or i in members
                or units[i]["owner"] != units[container]["owner"] for i in ids):
            raise ValueError("invalid squadron members")
        units[container]["members"] = ids
        members.update(ids)
    if any(units[i]["members"] for i in members):
        raise ValueError("nested squadron containers")
    top = [unit for entity, unit in units.items() if entity not in members]
    if any(not unit["type"] or not players[unit["owner"]]["faction"] for unit in top):
        raise ValueError("catalog does not resolve every staged type and owner")
    commands = []
    issued = set()
    for _ in range(ncommands):
        size, = take("I")
        end = offset + size
        tick, owner, sequence, opcode, reserved8, reserved16 = take("QIQBBH")
        if tick or reserved8 or reserved16 or owner not in players or not players[owner]["commandable"]:
            raise ValueError("expected tick-zero orders from a commandable owner")
        if opcode == 3:
            target, = take("Q")
            payload = {"target": target}
            if target not in units or target in members or units[target]["owner"] == owner:
                raise ValueError("invalid melee attack target")
        elif opcode == 6:
            x, y, z, target = take("3qQ")
            if target:
                raise ValueError("melee attack-move must target a position")
            payload = {"position": [v / SCALE for v in (x, y, z)]}
        else:
            raise ValueError("melee staging admits only attack and attack-move")
        count, reserved = take("II")
        ids = list(take("Q" * count)) if count <= (len(blob) - offset) // 8 else []
        if offset != end or reserved or not ids or len(ids) != count or len(set(ids)) != count:
            raise ValueError("invalid melee command length or unit list")
        if any(i not in units or i in members or i in issued or units[i]["owner"] != owner for i in ids):
            raise ValueError("invalid or repeated melee command issuer")
        issued.update(ids)
        commands.append({"owner": owner, "sequence": sequence,
                         "kind": "attack" if opcode == 3 else "attack_move", "units": ids, **payload})
    if offset != len(blob) or issued != {u["id"] for u in top}:
        raise ValueError("trailing bytes or staged units without exactly one order")
    return {"seed": seed, "replay_sha256": hashlib.sha256(blob).hexdigest(),
            "content_identity": identity.hex(), "players": players,
            "units": top, "craft": [units[i] for i in units if i in members], "commands": commands}


def lua(value) -> str:
    if isinstance(value, dict):
        return "{" + ",".join("[" + lua(k) + "]=" + lua(v) for k, v in value.items()) + "}"
    if isinstance(value, list):
        return "{" + ",".join(map(lua, value)) + "}"
    if value is None:
        return "nil"
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return str(value)
    return json.dumps(value, ensure_ascii=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--replay", type=Path, required=True)
    parser.add_argument("--catalog", type=Path, help="JSON with types and factions name lists")
    parser.add_argument("--out", type=Path, required=True,
                        help="fresh ignored output directory; pass <out>/private (not <out>) to "
                             "Invoke-FocMapCapture.ps1 -GcInputRoot")
    parser.add_argument("--start-seconds", type=float, default=60.0, help="logical seconds before one staged fight")
    args = parser.parse_args()
    if not 0 <= args.start_seconds <= 900:
        parser.error("--start-seconds must be in [0, 900]")
    fixture = read_fixture(args.replay.read_bytes(), json.loads(args.catalog.read_text())
                           if args.catalog else benchmark_catalog())
    args.out.mkdir(parents=True, exist_ok=False)
    (args.out / "fixture.json").write_text(json.dumps(fixture, indent=2) + "\n", encoding="utf-8")
    private = args.out / "private"
    private.mkdir()
    template = Path(__file__).with_suffix(".lua").read_text(encoding="ascii")
    (private / "probe.lua").write_text("EaWR_Melee_Input = " + lua(fixture)
                                      + "\nEaWR_Melee_Start_Seconds = " + str(args.start_seconds)
                                      + "\n" + template, encoding="ascii")


if __name__ == "__main__":
    main()
