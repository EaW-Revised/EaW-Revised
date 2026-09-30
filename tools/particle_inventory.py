#!/usr/bin/env python3
"""Inventory particle plug-ins in loose files and MEG archives.

This helper reads only ALO chunk metadata.  It never extracts or writes source assets.
The output is suitable for checking into plan/inventories/ after adding implementation
support annotations there.
"""

from __future__ import annotations

import argparse
import json
import struct
from collections import Counter
from pathlib import Path


PLUGIN_NAMES = {
    0: ("creator", "Point"), 1: ("creator", "Trail"),
    2: ("creator", "Sphere"), 3: ("creator", "Box"),
    4: ("creator", "Torus"), 5: ("creator", "Mesh"),
    6: ("creator", "Terrain"), 7: ("modifier", "LinearSize"),
    8: ("modifier", "LinearColor"), 9: ("modifier", "LinearRotation"),
    10: ("modifier", "Acceleration"), 11: ("modifier", "AttractAcceleration"),
    12: ("modifier", "Turbulence"), 13: ("modifier", "Vortex"),
    14: ("modifier", "ConstantUV"), 15: ("modifier", "KeyedSize"),
    16: ("modifier", "KeyedRotation"), 17: ("modifier", "KeyedColor"),
    18: ("modifier", "KeyedUV"), 19: ("killer", "Age"),
    20: ("killer", "Radius"), 21: ("killer", "Terrain"),
    22: ("renderer", "Billboard"), 23: ("renderer", "Line"),
    24: ("renderer", "Chain"), 25: ("renderer", "Volumetric"),
    26: ("translater", "Emitter"), 27: ("translater", "World"),
    28: ("renderer", "XYAligned"), 29: ("renderer", "VelocityAligned"),
    30: ("modifier", "RandomUV"), 31: ("modifier", "KeyedAcceleration"),
    32: ("modifier", "KeyedFriction"), 33: ("modifier", "SlottedRandomUV"),
    34: ("creator", "Shape"), 35: ("creator", "EnhancedMesh"),
    36: ("renderer", "StretchedTextureChain"), 37: ("renderer", "BumpMap"),
    38: ("renderer", "HeatSaturation"), 39: ("creator", "EnhancedTrail"),
    40: ("creator", "Death"), 41: ("modifier", "Attractor"),
    42: ("modifier", "SpeedLimit"), 43: ("creator", "AlignedShape"),
    44: ("killer", "Target"), 45: ("modifier", "DistanceKeyTime"),
    46: ("modifier", "LinearSpeed"), 47: ("renderer", "XYAlignedChain"),
    48: ("modifier", "LinearRotationRate"),
    49: ("modifier", "KeyedRotationRate"),
    50: ("creator", "OutwardVelocityShape"),
    51: ("modifier", "TerrainBounce"), 52: ("renderer", "Kites"),
    53: ("modifier", "ColorVariance"),
    54: ("modifier", "WindAcceleration"), 55: ("modifier", "Torque"),
    56: ("modifier", "AxisAttractor"), 57: ("modifier", "ConstantSize"),
    58: ("modifier", "ConstantRotation"), 59: ("modifier", "ConstantColor"),
    60: ("creator", "HardwareSpawner"),
    61: ("renderer", "HardwareBillboards"),
    62: ("killer", "HardwareStomper"),
}

CPU_SUPPORTED = {7, 10, 11, 14, 15, 18, 19, 21, 26, 27, 34, 39, 40, 49, 53, 54, 57, 58}
CPU_REASONS = {
    39: "implemented for V1 parent birth lifecycle in the portable presentation CPU runtime",
    40: "implemented for V1 parent death burst in the portable presentation CPU runtime",
}
RENDER_METADATA = {22, 23, 24, 25, 28, 29, 36, 37, 38, 47, 52, 61}
UNSUPPORTED_REASONS = {
    1: "requires parent-particle attachment",
    5: "requires renderer mesh binding",
    6: "unsupported by the MIT reference and needs terrain sampling",
    12: "noise contract is not yet ported",
    32: "MIT reference has no behavior implementation",
    35: "requires renderer mesh binding",
    41: "target/bone attachment is deferred",
    43: "bone alignment is deferred",
    44: "target/bone attachment is deferred",
    45: "MIT reference has no behavior implementation",
    50: "MIT reference only partially implements this family",
    51: "MIT reference has no behavior implementation",
    55: "MIT reference has no behavior implementation",
    56: "MIT reference has no behavior implementation",
    60: "hardware/GPU family is outside the CPU slice",
    62: "hardware/GPU family is outside the CPU slice",
}


def chunks(data: bytes, begin: int = 0, end: int | None = None):
    end = len(data) if end is None else end
    pos = begin
    while pos < end:
        if end - pos < 8:
            raise ValueError("truncated chunk header")
        chunk_type, size_word = struct.unpack_from("<II", data, pos)
        size = size_word & 0x7FFFFFFF
        payload = pos + 8
        limit = payload + size
        if limit > end:
            raise ValueError("chunk crosses enclosing bound")
        yield chunk_type, bool(size_word & 0x80000000), payload, limit
        pos = limit


def minis(data: bytes, begin: int, end: int):
    pos = begin
    while pos < end:
        if end - pos < 2:
            raise ValueError("truncated mini-chunk header")
        item_type, size = struct.unpack_from("BB", data, pos)
        payload = pos + 2
        limit = payload + size
        if limit > end:
            raise ValueError("mini-chunk crosses enclosing bound")
        yield item_type, payload, limit
        pos = limit


def scalar(data: bytes, item, default=0):
    begin, end = item[1], item[2]
    if end - begin == 1:
        return data[begin]
    if end - begin == 4:
        return struct.unpack_from("<I", data, begin)[0]
    return default


def real(data: bytes, item, default=0.0):
    return struct.unpack_from("<f", data, item[1])[0] if item[2] - item[1] == 4 else default


def infer_v1_plugins(data: bytes, begin: int, end: int) -> tuple[Counter[int], int, Counter[str]]:
    emitters = []
    for chunk_type, is_group, child_begin, child_end in chunks(data, begin, end):
        if chunk_type != 0x800 or not is_group:
            continue
        emitters.extend(item for item in chunks(data, child_begin, child_end)
                        if item[0] == 0x700 and item[1])
    dependencies: dict[int, int] = {}
    records = []
    for index, emitter in enumerate(emitters):
        props = {}
        track_summary = []
        for child_type, child_group, child_begin, child_end in chunks(data, emitter[2], emitter[3]):
            if child_type == 2 and not child_group:
                props = {item[0]: item for item in minis(data, child_begin, child_end)}
            elif child_type == 0x36 and not child_group:
                dependency = {item[0]: item for item in minis(data, child_begin, child_end)}
                death = scalar(data, dependency.get(0x37, (0, 0, 0)), 0xFFFFFFFF)
                life = scalar(data, dependency.get(0x39, (0, 0, 0)), 0xFFFFFFFF)
                if death != 0xFFFFFFFF:
                    dependencies[death] = 40
                if life != 0xFFFFFFFF:
                    dependencies[life] = 39
            elif child_type == 1 and child_group:
                children = list(chunks(data, child_begin, child_end))
                for offset in range(0, len(children), 2):
                    if offset + 1 >= len(children):
                        break
                    header, keys = children[offset], children[offset + 1]
                    header_minis = {item[0]: item for item in minis(data, header[2], header[3])}
                    key_count = sum(1 for item in minis(data, keys[2], keys[3]) if item[0] == 5)
                    first = real(data, header_minis.get(2, (0, 0, 0)))
                    last = real(data, header_minis.get(3, (0, 0, 0)))
                    track_summary.append((key_count, first, last))
        records.append((props, track_summary))

    result: Counter[int] = Counter()
    conversion_only: Counter[str] = Counter()
    for index, (props, tracks) in enumerate(records):
        def integer(key, default=0):
            return scalar(data, props.get(key, (0, 0, 0)), default)

        def floating(key, default=0.0):
            return real(data, props.get(key, (0, 0, 0)), default)

        emit_mesh = integer(0x34)
        result[35 if emit_mesh else dependencies.get(index, 34)] += 1
        result[26 if integer(0x08) else 27] += 1
        ground = integer(0x2F)
        result[21 if ground == 1 else 19] += 1
        if integer(0x3B):
            result[38] += 1
        elif integer(0x2E):
            result[28] += 1
        elif integer(0x41):
            result[52] += 1
        else:
            result[22] += 1
        conversion_only["KeyedChannelColor"] += 1
        if floating(0x09) != 0.0:
            conversion_only["InwardSpeed"] += 1
        accel_item = props.get(0x0A)
        has_accel = accel_item is not None and any(
            value != 0.0 for value in struct.unpack_from("<fff", data, accel_item[1]))
        if has_accel or floating(0x0C) != 0.0:
            result[10] += 1
        if floating(0x0B) != 0.0:
            result[11] += 1
        if integer(0x31):
            result[54] += 1
        if ground in (2, 3):
            result[51] += 1
        color_item = props.get(0x2C)
        if color_item is not None and any(
            value != 0.0 for value in struct.unpack_from("<ffff", data, color_item[1])):
            result[53] += 1
        if len(tracks) >= 6:
            size_keys, size_first, size_last = tracks[4]
            result[57 if size_keys == 0 and size_first == size_last else
                   7 if size_keys == 0 else 15] += 1
            uv_keys, uv_first, uv_last = tracks[5]
            result[14 if uv_keys == 0 and uv_first == uv_last else 18] += 1
        result[58 if integer(0x48) else 49] += 1
    return result, len(emitters), conversion_only


def inspect_alo(data: bytes) -> tuple[str, Counter[int], int, Counter[str]]:
    roots = list(chunks(data))
    if len(roots) != 1:
        return "not-particle", Counter(), 0, Counter()
    root_type, grouped, begin, end = roots[0]
    if not grouped or root_type not in (0x900, 0x1500):
        return "not-particle", Counter(), 0, Counter()
    if root_type == 0x900:
        plugins, emitter_count, conversion_only = infer_v1_plugins(data, begin, end)
        return "v1", plugins, emitter_count, conversion_only

    counts: Counter[int] = Counter()
    emitter_count = 0
    for chunk_type, is_group, child_begin, child_end in chunks(data, begin, end):
        if chunk_type != 0x1520 or not is_group:
            continue
        for emitter_type, emitter_group, emitter_begin, emitter_end in chunks(
                data, child_begin, child_end):
            if emitter_type != 0x1540 or not emitter_group:
                continue
            emitter_count += 1
            for family, family_group, family_begin, family_end in chunks(
                    data, emitter_begin, emitter_end):
                if family not in (1, 2, 3, 4, 5) or not family_group:
                    continue
                nested = list(chunks(data, family_begin, family_end))
                if not nested or nested[0][0] != 0 or nested[0][1]:
                    continue
                id_begin, id_end = nested[0][2], nested[0][3]
                if id_end - id_begin == 4:
                    counts[struct.unpack_from("<I", data, id_begin)[0]] += 1
    return "v2", counts, emitter_count, Counter()


def meg_members(path: Path):
    with path.open("rb") as source:
        header = source.read(8)
        if len(header) != 8:
            return
        name_count, file_count = struct.unpack("<II", header)
        names = []
        for _ in range(name_count):
            size_data = source.read(2)
            if len(size_data) != 2:
                return
            size = struct.unpack("<H", size_data)[0]
            names.append(source.read(size).decode("latin1"))
        entries = []
        for _ in range(file_count):
            record = source.read(20)
            if len(record) != 20:
                return
            _, _, size, offset, name_index = struct.unpack("<IIIII", record)
            if name_index < len(names) and names[name_index].lower().endswith(".alo"):
                entries.append((names[name_index], size, offset))
        for name, size, offset in entries:
            source.seek(offset)
            yield name, source.read(size)


def scan_layer(root: Path, layer: str, counts: Counter[int], stats: Counter[str],
               conversion_only: Counter[str], errors: list[dict[str, str]]):
    for archive in sorted(root.rglob("*.meg"), key=lambda item: str(item).lower()):
        try:
            for name, data in meg_members(archive):
                inspect_occurrence(data, f"{layer}:{archive.name}!/{name}", counts, stats,
                                   conversion_only, errors)
        except (OSError, ValueError, struct.error) as error:
            errors.append({"source": f"{layer}:{archive.name}", "reason": str(error)})
    for alo in sorted(root.rglob("*.alo"), key=lambda item: str(item).lower()):
        try:
            inspect_occurrence(alo.read_bytes(), f"{layer}:loose/{alo.relative_to(root).as_posix()}",
                               counts, stats, conversion_only, errors)
        except (OSError, ValueError, struct.error) as error:
            errors.append({"source": f"{layer}:loose/{alo.name}", "reason": str(error)})


def inspect_occurrence(data: bytes, source: str, counts: Counter[int], stats: Counter[str],
                       conversion_only: Counter[str], errors: list[dict[str, str]]):
    try:
        version, plugins, emitters, inferred_only = inspect_alo(data)
        stats["alo_occurrences"] += 1
        if version == "not-particle":
            stats["model_occurrences"] += 1
            return
        stats[f"particle_{version}_occurrences"] += 1
        stats[f"particle_{version}_emitters"] += emitters
        counts.update(plugins)
        conversion_only.update(inferred_only)
    except (ValueError, struct.error) as error:
        errors.append({"source": source, "reason": str(error)})


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--game-root", type=Path, required=True)
    parser.add_argument("--mod-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    layers = [
        (args.game_root / "GameData" / "Data", "eaw"),
        (args.game_root / "corruption" / "Data", "foc"),
    ]
    if args.mod_root:
        layers.append((args.mod_root, "remake"))
    counts: Counter[int] = Counter()
    stats: Counter[str] = Counter()
    for key in ("alo_occurrences", "model_occurrences", "particle_v1_occurrences",
                "particle_v1_emitters", "particle_v2_occurrences", "particle_v2_emitters"):
        stats[key] = 0
    conversion_only: Counter[str] = Counter()
    errors: list[dict[str, str]] = []
    for root, layer in layers:
        if root.is_dir():
            scan_layer(root, layer, counts, stats, conversion_only, errors)
    plugins = []
    status_occurrences: Counter[str] = Counter()
    for plugin_id in sorted(set(PLUGIN_NAMES) | set(counts)):
        family, name = PLUGIN_NAMES.get(plugin_id, ("unknown", f"Unknown{plugin_id}"))
        if plugin_id in CPU_SUPPORTED:
            status, reason = "cpu-supported", CPU_REASONS.get(
                plugin_id, "implemented in the portable presentation CPU runtime")
        elif plugin_id in RENDER_METADATA:
            status, reason = "metadata-only", "RenderingServer adapter and visual comparison are deferred"
        else:
            status = "unsupported"
            reason = UNSUPPORTED_REASONS.get(
                plugin_id, "not present in the available V1-converted corpus; behavior is not in this CPU slice")
        status_occurrences[status] += counts[plugin_id]
        plugins.append({"id": plugin_id, "name": name, "family": family,
                        "occurrences": counts[plugin_id],
                        "occurrence_basis": "inferred V1 conversion plus encoded V2 IDs",
                        "status": status, "reason": reason})
    report = {
        "schema_version": 1,
        "scope": "physical ALO occurrences in all MEG archives and loose files under supplied roots",
        "layers": [name for root, name in layers if root.is_dir()],
        "counts": dict(sorted(stats.items())),
        "plugin_occurrences_by_status": dict(sorted(status_occurrences.items())),
        "plugins": plugins,
        "conversion_only_plugins": [
            {"name": name, "occurrences": count, "status": "cpu-supported",
             "reason": "implemented during legacy V1 conversion; this helper has no encoded V2 plugin ID"}
            for name, count in sorted(conversion_only.items())
        ],
        "scan_errors": errors,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
