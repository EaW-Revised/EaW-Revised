#!/usr/bin/env python3
"""Compare two behaviour traces (docs/traces.md) and print the first divergence.

With --report, walk every row instead and print, per field, the largest difference, the tick
where it occurs, and how many values exceed the tolerance.

Exit status: 0 when the traces match within tolerance, 1 at the first divergence
(including incompatible headers), 2 for malformed input or usage errors.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import pathlib
import re
import sys
from typing import Dict, List, Optional, Tuple, Union

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / "common"))
import eawr_evidence_io  # noqa: E402

TRACE_FORMAT = "eawr-trace"
TRACE_VERSION = 1
SCENARIO_FORMAT = "eawr-scenario"
SCENARIO_VERSION = 1
CSV_HEADER = "tick,object,field,value"

UNIT_LABEL = re.compile(r"[a-z0-9][a-z0-9_.-]*\Z")
OBJECT_LABEL = re.compile(r"[a-z0-9][a-z0-9_.-]*(?:/[a-z0-9][a-z0-9_.-]*)?\Z")
INTEGER = re.compile(r"(?:0|-?[1-9][0-9]*)\Z")
NATURAL = re.compile(r"(?:0|[1-9][0-9]*)\Z")
HEX64 = re.compile(r"[0-9a-f]{64}\Z")
GIT_SHA = re.compile(r"[0-9a-f]{40}(?:[0-9a-f]{24})?\Z")
SCENARIO_ID = re.compile(r"S-[0-9]{2}\Z")

Q24_FIELDS = ("fwd.x", "fwd.y", "fwd.z", "hull", "pos.x", "pos.y", "pos.z", "shield")
UNIT_FIELDS = ("alive",) + Q24_FIELDS
HARDPOINT_FIELDS = ("shots", "target")
FIELD_KIND = dict(
    [(field, "q24") for field in Q24_FIELDS]
    + [("alive", "flag"), ("shots", "count"), ("target", "label")]
)
INT64_MIN = -(1 << 63)
INT64_MAX = (1 << 63) - 1
UINT64_MAX = (1 << 64) - 1
MAX_DECIMAL_LENGTH = 21  # sign and 20 digits cover every 64-bit value
MAX_SOURCE_UNITS = 1_000_000

Row = Tuple[int, str, str, str]
Tolerance = Union[int, str]


class InputError(ValueError):
    """Malformed trace, header or scenario input (exit status 2)."""


class Divergence(ValueError):
    """The traces are not equal within tolerance (exit status 1)."""


def q24_from_binary32_bits(bits: int) -> int:
    """Q24 raw value of a binary32 bit pattern, nearest-even (scene::fixed_from_binary32)."""
    if not 0 <= bits <= 0xFFFFFFFF:
        raise ValueError("binary32 bit pattern out of range")
    negative = (bits >> 31) != 0
    exponent = (bits >> 23) & 0xFF
    fraction = bits & 0x7FFFFF
    if exponent == 0xFF:
        raise ValueError("binary32 value is NaN or infinite")
    mantissa = fraction if exponent == 0 else fraction | 0x800000
    shift = (-149 if exponent == 0 else exponent - 150) + 24
    if mantissa == 0:
        return 0
    if shift >= 0:
        if mantissa.bit_length() + shift > 63:
            raise ValueError("binary32 value exceeds the Q24 range")
        magnitude = mantissa << shift
    else:
        right = -shift
        quotient = mantissa >> right
        remainder = mantissa & ((1 << right) - 1)
        half = 1 << (right - 1)
        magnitude = quotient
        if remainder > half or (remainder == half and quotient & 1):
            magnitude += 1
    return -magnitude if negative else magnitude


def sha256_file(path: pathlib.Path) -> str:
    try:
        return hashlib.sha256(eawr_evidence_io.read_bytes(path)).hexdigest()
    except OSError as error:
        raise InputError(f"{path}: {error}") from error


def header_path(trace: pathlib.Path) -> pathlib.Path:
    if trace.suffix != ".csv":
        raise InputError(f"{trace}: a trace file name must end in .csv")
    return trace.with_suffix(".json")


def bounded_int(text: str, low: int, high: int) -> Optional[int]:
    """The value of a canonical decimal in low..high, or None; long digit runs are never parsed."""
    if len(text) > MAX_DECIMAL_LENGTH or not INTEGER.match(text):
        return None
    value = int(text)
    return value if low <= value <= high else None


def check_value(where: str, field: str, value: str) -> None:
    kind = FIELD_KIND[field]
    if kind == "label":
        if value and not UNIT_LABEL.match(value):
            raise InputError(f"{where}: target must be a unit label or empty, not {value!r}")
        return
    if kind == "flag" and value not in ("0", "1"):
        raise InputError(f"{where}: alive must be 0 or 1, not {value!r}")
    if kind == "count" and not NATURAL.match(value):
        raise InputError(f"{where}: shots must be a non-negative integer, not {value!r}")
    if bounded_int(value, INT64_MIN, INT64_MAX) is None:
        raise InputError(f"{where}: {field} must be a signed 64-bit integer, not {value!r}")


def read_trace(path: pathlib.Path) -> List[Row]:
    try:
        raw = eawr_evidence_io.read_bytes(path)
    except OSError as error:
        raise InputError(f"{path}: {error}") from error
    if raw.startswith(b"\xef\xbb\xbf") or b"\r" in raw or not raw.endswith(b"\n"):
        raise InputError(f"{path}: expected UTF-8 without BOM and LF-terminated lines")
    try:
        lines = raw.decode("utf-8").split("\n")[:-1]
    except UnicodeDecodeError as error:
        raise InputError(f"{path}: invalid UTF-8: {error}") from error
    if not lines or lines[0] != CSV_HEADER:
        raise InputError(f"{path}: first line must be exactly {CSV_HEADER}")
    rows: List[Row] = []
    previous: Optional[Tuple[int, str, str]] = None
    for number, line in enumerate(lines[1:], 2):
        where = f"{path}:{number}"
        parts = line.split(",")
        if len(parts) != 4:
            raise InputError(f"{where}: expected four comma-separated columns")
        tick_text, label, field, value = parts
        tick = bounded_int(tick_text, 0, UINT64_MAX)
        if tick is None:
            raise InputError(f"{where}: malformed tick {tick_text!r}")
        if not OBJECT_LABEL.match(label):
            raise InputError(f"{where}: malformed object label {label!r}")
        allowed = HARDPOINT_FIELDS if "/" in label else UNIT_FIELDS
        if field not in allowed:
            kind = "hardpoint" if "/" in label else "unit"
            raise InputError(f"{where}: {field!r} is not a {kind} field")
        check_value(where, field, value)
        key = (tick, label, field)
        if previous is not None and key <= previous:
            raise InputError(f"{where}: rows must be unique and sorted by tick, object, field")
        previous = key
        rows.append((key[0], label, field, value))
    if not rows:
        raise InputError(f"{path}: the trace has no rows")
    return rows


def read_json(path: pathlib.Path) -> dict:
    try:
        value = json.loads(eawr_evidence_io.read_bytes(path).decode("utf-8"))
    except (OSError, UnicodeDecodeError, ValueError, RecursionError) as error:
        raise InputError(f"{path}: {error}") from error
    if not isinstance(value, dict):
        raise InputError(f"{path}: expected a JSON object")
    return value


def is_int(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def is_format(value: dict, name: str, version: int) -> bool:
    """True for exactly this format name and integer version (true and 1.0 are not 1)."""
    number = value["format_version"]
    return value["format"] == name and is_int(number) and number == version


def require_keys(where: str, value: object, required: Tuple[str, ...], optional=()) -> dict:
    if not isinstance(value, dict):
        raise InputError(f"{where}: expected an object")
    missing = [key for key in required if key not in value]
    unknown = sorted(set(value) - set(required) - set(optional))
    if missing or unknown:
        raise InputError(f"{where}: missing keys {missing}, unknown keys {unknown}")
    return value


def check_rational(where: str, value: object) -> Tuple[int, int]:
    value = require_keys(where, value, ("numerator", "denominator"))
    numerator, denominator = value["numerator"], value["denominator"]
    if not (is_int(numerator) and is_int(denominator) and numerator > 0 and denominator > 0):
        raise InputError(f"{where}: numerator and denominator must be positive integers")
    if math.gcd(numerator, denominator) != 1:
        raise InputError(f"{where}: the rational must be in lowest terms")
    return numerator, denominator


def read_header(path: pathlib.Path) -> dict:
    header = require_keys(
        str(path),
        read_json(path),
        (
            "format",
            "format_version",
            "source",
            "content_identity",
            "tick_seconds",
            "build_identity",
            "scenario_sha256",
        ),
        ("replay_sha256", "run"),
    )
    if not is_format(header, TRACE_FORMAT, TRACE_VERSION):
        raise InputError(f"{path}: expected format {TRACE_FORMAT} version {TRACE_VERSION}")
    if header["source"] not in ("original", "remake"):
        raise InputError(f"{path}: source must be original or remake")
    for key in ("content_identity", "scenario_sha256", "replay_sha256"):
        value = header.get(key)
        if key in header and value is not None and not (isinstance(value, str) and HEX64.match(value)):
            raise InputError(f"{path}: {key} must be 64 lowercase hex digits")
    if header["content_identity"] is None:
        raise InputError(f"{path}: content_identity is required")
    check_rational(f"{path}: tick_seconds", header["tick_seconds"])
    build = require_keys(f"{path}: build_identity", header["build_identity"], ("kind", "value"))
    patterns = {"executable-sha256": HEX64, "git-sha": GIT_SHA}
    pattern = patterns.get(build["kind"]) if isinstance(build["kind"], str) else None
    if pattern is None or not isinstance(build["value"], str) or not pattern.match(build["value"]):
        raise InputError(f"{path}: build_identity must be executable-sha256 or git-sha with hex value")
    if "run" in header and not (is_int(header["run"]) and header["run"] >= 1):
        raise InputError(f"{path}: run must be a positive integer")
    return header


def require_list(where: str, value: object) -> list:
    if not isinstance(value, list):
        raise InputError(f"{where}: expected a list")
    return value


def require_text(where: str, value: object, pattern=None) -> str:
    if not isinstance(value, str) or not value or (pattern and not pattern.match(value)):
        raise InputError(f"{where}: expected a well-formed non-empty string, not {value!r}")
    return value


def is_pose_number(value: object) -> bool:
    return (is_int(value) or isinstance(value, float) and math.isfinite(value)) and abs(value) <= MAX_SOURCE_UNITS


def check_position(where: str, value: object, *, allow_decimal: bool = False) -> None:
    if not (
        isinstance(value, list)
        and len(value) == 3
        and all(is_pose_number(item) if allow_decimal else is_int(item) and abs(item) <= MAX_SOURCE_UNITS
                for item in value)
    ):
        kind = "finite numbers" if allow_decimal else "integers"
        raise InputError(f"{where}: position must be three {kind} in source units")


def check_pin(where: str, value: object) -> dict:
    pin = require_keys(where, value, ("archive", "path", "sha256"))
    if not all(isinstance(pin[key], str) and pin[key] for key in ("archive", "path")):
        raise InputError(f"{where}: archive and path must be non-empty strings")
    if not (isinstance(pin["sha256"], str) and HEX64.match(pin["sha256"])):
        raise InputError(f"{where}: sha256 must be 64 lowercase hex digits")
    return pin


def load_scenario(path: pathlib.Path) -> dict:
    scenario = require_keys(
        str(path),
        read_json(path),
        (
            "format",
            "format_version",
            "id",
            "label",
            "title",
            "intent",
            "behaviour",
            "executable",
            "content",
            "map",
            "players",
            "staging",
            "units",
            "hardpoints",
            "events",
            "duration_ticks",
            "tolerances",
            "expect",
        ),
        ("notes", "record_only", "fire_windows"),
    )
    if not is_format(scenario, SCENARIO_FORMAT, SCENARIO_VERSION):
        raise InputError(f"{path}: expected format {SCENARIO_FORMAT} version {SCENARIO_VERSION}")
    if not (isinstance(scenario["id"], str) and SCENARIO_ID.match(scenario["id"])):
        raise InputError(f"{path}: id must look like S-01")
    if not (isinstance(scenario["label"], str) and UNIT_LABEL.match(scenario["label"])):
        raise InputError(f"{path}: label must be a lowercase label")
    executable = require_keys(f"{path}: executable", scenario["executable"], ("path", "sha256"))
    if not (isinstance(executable["sha256"], str) and HEX64.match(executable["sha256"])):
        raise InputError(f"{path}: executable sha256 must be 64 lowercase hex digits")
    content = scenario["content"]
    if not isinstance(content, list) or not content:
        raise InputError(f"{path}: content must list the pinned data files")
    pins = [check_pin(f"{path}: content[{index}]", pin) for index, pin in enumerate(content)]
    if len({(pin["archive"], pin["path"]) for pin in pins}) != len(pins):
        raise InputError(f"{path}: content lists a file twice")
    game_map = require_keys(f"{path}: map", scenario["map"], ("path",))
    if not any(pin["path"].lower() == str(game_map["path"]).lower() for pin in pins):
        raise InputError(f"{path}: the map must also be pinned under content")

    staging = require_keys(f"{path}: staging", scenario["staging"], ("fog", "ai"))
    if staging["fog"] != "revealed" or staging["ai"] != "suspended":
        raise InputError(f"{path}: staging must reveal fog and suspend the AI")
    players = require_list(f"{path}: players", scenario["players"])
    if len(players) < 2:
        raise InputError(f"{path}: players must list at least two players")
    player_labels = set()
    for index, player in enumerate(players):
        where = f"{path}: players[{index}]"
        player = require_keys(where, player, ("label", "faction"))
        require_text(f"{where}.faction", player["faction"])
        if require_text(f"{where}.label", player["label"], UNIT_LABEL) in player_labels:
            raise InputError(f"{where}: label is repeated")
        player_labels.add(player["label"])

    duration = scenario["duration_ticks"]
    if not (is_int(duration) and duration > 0):
        raise InputError(f"{path}: duration_ticks must be a positive integer")
    units: Dict[str, dict] = {}
    for index, unit in enumerate(require_list(f"{path}: units", scenario["units"])):
        where = f"{path}: units[{index}]"
        unit = require_keys(
            where,
            unit,
            ("label", "type", "owner", "position", "facing_degrees", "spawn"),
            ("staging", "shieldless", "apply_initial_pose"),
        )
        if require_text(f"{where}.label", unit["label"], UNIT_LABEL) in units:
            raise InputError(f"{where}: label is repeated")
        require_text(f"{where}.type", unit["type"])
        if require_text(f"{where}.owner", unit["owner"]) not in player_labels:
            raise InputError(f"{where}: owner is not a declared player")
        initial_pose = unit.get("apply_initial_pose", False)
        if not isinstance(initial_pose, bool) or initial_pose and unit["spawn"] != "observed":
            raise InputError(f"{where}: apply_initial_pose is a boolean for observed units only")
        check_position(where, unit["position"], allow_decimal=initial_pose)
        facing = unit["facing_degrees"]
        valid_facing = is_pose_number(facing) if initial_pose else is_int(facing)
        if not (valid_facing and 0 <= facing < 360):
            raise InputError(f"{where}: facing_degrees must be {'a finite number' if initial_pose else 'an integer'} in [0, 360)")
        if unit["spawn"] not in ("start", "event", "observed"):
            raise InputError(f"{where}: spawn must be start, event or observed")
        flags = require_list(f"{where}.staging", unit.get("staging", []))
        if not all(flag in ("hold_position", "hold_fire", "invulnerable") for flag in flags):
            raise InputError(f"{where}: staging flags are hold_position, hold_fire, invulnerable")
        if unit.get("shieldless", True) is not True:
            raise InputError(f"{where}: shieldless is true or absent")
        units[unit["label"]] = unit
    if not units:
        raise InputError(f"{path}: units must not be empty")

    hardpoints: Dict[str, dict] = {}
    for index, hardpoint in enumerate(require_list(f"{path}: hardpoints", scenario["hardpoints"])):
        where = f"{path}: hardpoints[{index}]"
        hardpoint = require_keys(
            where,
            hardpoint,
            ("label", "unit", "hardpoint", "range", "priority_set", "source"),
        )
        label = require_text(f"{where}.label", hardpoint["label"], OBJECT_LABEL)
        if "/" not in label or label in hardpoints:
            raise InputError(f"{where}: label must be <unit>/<name> and unique")
        for key in ("hardpoint", "priority_set", "source"):
            require_text(f"{where}.{key}", hardpoint[key])
        if label.split("/")[0] != hardpoint["unit"] or label.split("/")[0] not in units:
            raise InputError(f"{where}: label prefix must name a declared unit")
        if not (is_int(hardpoint["range"]) and hardpoint["range"] > 0):
            raise InputError(f"{where}: range must be a positive integer")
        hardpoints[label] = hardpoint

    spawned = {label for label, unit in units.items() if unit["spawn"] == "start"}
    removed = set()
    last_tick = 0
    for index, event in enumerate(require_list(f"{path}: events", scenario["events"])):
        where = f"{path}: events[{index}]"
        event = require_keys(
            where, event, ("tick", "action", "unit"),
            ("position", "target", "ability", "amount", "hardpoint", "with")
        )
        tick, action, label = event["tick"], event["action"], event["unit"]
        arguments = {
            "spawn": set(), "remove": set(), "move": {"position"}, "face": {"position"},
            "stop": set(), "attack": {"target"}, "ability": {"ability"}, "ability_probe": {"ability"},
            "damage": {"amount", "hardpoint"},
            # #452: an attack-move or guard of a point or of a unit (docs/behaviour/space-orders.md).
            "attack_move": {"position"}, "guard": {"position"},
        }
        if action in ("attack_move", "guard") and "target" in event:
            arguments[action] = {"target"}
        # #599: a move given to further units by the same command (a player's group move).
        if action == "move" and "with" in event:
            arguments[action] = {"position", "with"}
        # #561: a targeted ability (ION_CANNON_SHOT) names its target unit.
        if action == "ability" and "target" in event:
            arguments[action] = {"ability", "target"}
        if (not isinstance(action, str) or action not in arguments
                or set(event) != {"tick", "action", "unit"} | arguments[action]):
            raise InputError(f"{where}: unknown action or incorrect order arguments")
        if not (is_int(tick) and last_tick <= tick < duration and tick > 0):
            raise InputError(f"{where}: ticks must be ascending, positive and before the end")
        last_tick = tick
        if not isinstance(label, str) or label not in units:
            raise InputError(f"{where}: unit is not declared")
        if action == "spawn":
            if units[label]["spawn"] != "event" or label in spawned:
                raise InputError(f"{where}: only an event unit is spawned, once")
            spawned.add(label)
        elif action == "remove":
            if label not in spawned or label in removed:
                raise InputError(f"{where}: a unit is removed once, after it exists")
            removed.add(label)
        elif action in ("move", "face", "stop", "attack", "ability", "ability_probe", "damage", "attack_move", "guard"):
            if label not in spawned or label in removed:
                raise InputError(f"{where}: order requires a live staged unit")
            if action in ("move", "face") or (action in ("attack_move", "guard") and "position" in event):
                check_position(where, event["position"])
            if action == "move" and "with" in event:
                others = event["with"]
                if (not isinstance(others, list) or not others
                        or any(not isinstance(other, str) or other not in spawned or other in removed
                               or other == label for other in others)
                        or len(set(others)) != len(others)):
                    raise InputError(f"{where}: 'with' lists other live staged units, once each")
            if action in ("attack_move", "guard", "ability") and "target" in event and (
                    not isinstance(event["target"], str) or event["target"] not in spawned
                    or event["target"] in removed or event["target"] == label):
                raise InputError(f"{where}: {action} target requires another live staged unit")
            if action == "attack" and (not isinstance(event["target"], str)
                                        or event["target"] not in spawned or event["target"] in removed
                                        or event["target"] == label):
                raise InputError(f"{where}: attack target requires another live staged unit")
            if action in ("ability", "ability_probe"):
                if (not isinstance(event["ability"], str)
                        or not re.fullmatch(r"[A-Za-z0-9_-]+", event["ability"])):
                    raise InputError(f"{where}: ability name required")
            if action == "damage":
                if not is_int(event["amount"]) or event["amount"] < 0:
                    raise InputError(f"{where}: nonnegative damage amount required")
                if (not isinstance(event["hardpoint"], str)
                        or not re.fullmatch(r"[A-Za-z0-9_-]+", event["hardpoint"])):
                    raise InputError(f"{where}: hardpoint name required")
        else:
            raise InputError(f"{where}: unknown action")
    if spawned != {label for label, unit in units.items() if unit["spawn"] != "observed"}:
        raise InputError(f"{path}: every event unit needs a spawn event")

    load_tolerances(f"{path}: tolerances", scenario["tolerances"], bool(hardpoints))
    if "fire_windows" in scenario:
        check_fire_windows(path, scenario["fire_windows"], hardpoints, duration)
    if "record_only" in scenario and not isinstance(scenario["record_only"], bool):
        raise InputError(f"{path}: record_only must be boolean")
    if scenario.get("record_only") is True:
        if scenario["expect"] != []:
            raise InputError(f"{path}: record_only requires empty expect")
    else:
        check_expectations(path, scenario["expect"], units, hardpoints, duration)
    return scenario


def load_tolerances(where: str, value: object, has_hardpoints: bool) -> Dict[str, Tolerance]:
    if not isinstance(value, dict):
        raise InputError(f"{where}: expected an object of field tolerances")
    recorded = set(UNIT_FIELDS) | (set(HARDPOINT_FIELDS) if has_hardpoints else set())
    if set(value) != recorded:
        raise InputError(f"{where}: must give exactly the recorded fields {sorted(recorded)}")
    result: Dict[str, Tolerance] = {}
    for field, tolerance in value.items():
        result[field] = check_tolerance(f"{where}.{field}", field, tolerance)
    return result


def check_tolerance(where: str, field: str, tolerance: object) -> Tolerance:
    if tolerance == "ignore":
        return "ignore"
    if not (is_int(tolerance) and 0 <= tolerance <= INT64_MAX):
        raise InputError(f"{where}: tolerance must be a non-negative integer or \"ignore\"")
    if FIELD_KIND[field] in ("label", "flag") and tolerance != 0:
        raise InputError(f"{where}: {field} is compared exactly (0) or ignored")
    return tolerance


def check_expectations(
    path: pathlib.Path, value: object, units: dict, hardpoints: dict, duration: int
) -> None:
    if not isinstance(value, list) or not value:
        raise InputError(f"{path}: expect must list at least one expectation")
    for index, expectation in enumerate(value):
        where = f"{path}: expect[{index}]"
        if not isinstance(expectation, dict):
            raise InputError(f"{where}: expected an object")
        kind = expectation.get("kind")
        fields = {
            "first_target": ("kind", "hardpoint", "value", "text"),
            "holds_target": ("kind", "hardpoint", "value", "until_tick", "text"),
            "retarget": ("kind", "hardpoint", "value", "after_tick", "max_ticks", "text"),
        }.get(kind if isinstance(kind, str) else "")
        if fields is None:
            raise InputError(f"{where}: kind must be first_target, holds_target or retarget")
        require_keys(where, expectation, fields)
        require_text(f"{where}.text", expectation["text"])
        names = (expectation["hardpoint"], expectation["value"])
        if not all(isinstance(name, str) for name in names) or (
            names[0] not in hardpoints or names[1] not in units
        ):
            raise InputError(f"{where}: hardpoint and value must name declared objects")
        for key in ("until_tick", "after_tick"):
            if key in expectation and not (is_int(expectation[key]) and 0 < expectation[key] < duration):
                raise InputError(f"{where}: {key} must be a recorded tick (1..{duration - 1})")
        if "max_ticks" in expectation and not (
            is_int(expectation["max_ticks"]) and 0 < expectation["max_ticks"] <= duration
        ):
            raise InputError(f"{where}: max_ticks must be a tick count inside the scenario")


def check_fire_windows(path: pathlib.Path, value: object, hardpoints: dict, duration: int) -> None:
    """Shot-count windows: each listed hardpoint fires min_shots..max_shots (null: no upper
    bound) in total over ticks from_tick..to_tick, inclusive."""
    if not isinstance(value, list) or not value:
        raise InputError(f"{path}: fire_windows must list at least one window")
    for index, window in enumerate(value):
        where = f"{path}: fire_windows[{index}]"
        window = require_keys(
            where, window, ("hardpoints", "from_tick", "to_tick", "min_shots", "max_shots", "text")
        )
        require_text(f"{where}.text", window["text"])
        labels = require_list(f"{where}.hardpoints", window["hardpoints"])
        if not labels or not all(isinstance(label, str) and label in hardpoints for label in labels):
            raise InputError(f"{where}: hardpoints must list declared hardpoints")
        if len(set(labels)) != len(labels):
            raise InputError(f"{where}: hardpoints lists a hardpoint twice")
        first, last = window["from_tick"], window["to_tick"]
        if not (is_int(first) and is_int(last) and 0 <= first <= last < duration):
            raise InputError(f"{where}: from_tick..to_tick must be recorded ticks (0..{duration - 1})")
        low, high = window["min_shots"], window["max_shots"]
        if not (is_int(low) and low >= 0 and (high is None or (is_int(high) and high >= low))):
            raise InputError(f"{where}: min_shots must be a count and max_shots null or at least min_shots")
        if low == 0 and high is None:
            raise InputError(f"{where}: a window must bound the shot count")


def check_fire_rows(path: pathlib.Path, rows: List[Row], scenario: dict) -> None:
    """Raise Divergence when a trace fires outside a scenario's fire windows."""
    shots: Dict[str, List[Tuple[int, int]]] = {}
    for tick, label, field, value in rows:
        if field == "shots" and value != "0":
            shots.setdefault(label, []).append((tick, int(value)))
    for index, window in enumerate(scenario.get("fire_windows", [])):
        first, last = window["from_tick"], window["to_tick"]
        low, high = window["min_shots"], window["max_shots"]
        for label in window["hardpoints"]:
            fired = sum(count for tick, count in shots.get(label, []) if first <= tick <= last)
            if fired < low or (high is not None and fired > high):
                bound = f"{low}..{high}" if high is not None else f"at least {low}"
                raise Divergence(
                    f"{path}: fire window {index}, {label} fired {fired} shots in ticks {first}..{last}, "
                    f"expected {bound} ({window['text']})"
                )


def check_scenario_rows(path: pathlib.Path, rows: List[Row], scenario: dict) -> None:
    """A scenario trace covers ticks 0..duration_ticks - 1 with an alive row per declared unit,
    every other unit field exactly while it is alive, and both hardpoint fields exactly while
    their unit is."""
    units = {unit["label"] for unit in scenario["units"]}
    inventory = {
        unit["label"]: {"alive=1", *Q24_FIELDS} - ({"shield"} if unit.get("shieldless") else set())
        for unit in scenario["units"]
    }
    hardpoints = {hardpoint["label"]: hardpoint["unit"] for hardpoint in scenario["hardpoints"]}
    duration = scenario["duration_ticks"]
    recorded: Dict[int, Dict[str, set]] = {}
    for tick, label, field, value in rows:
        if label not in units and label not in hardpoints:
            raise InputError(f"{path}: tick {tick}: {label} is not a scenario unit or hardpoint")
        if tick >= duration:
            raise InputError(f"{path}: tick {tick} is past the scenario's last tick {duration - 1}")
        fields = recorded.setdefault(tick, {}).setdefault(label, set())
        fields.add(field if field != "alive" else f"alive={value}")
    for tick in range(duration):
        objects = recorded.get(tick, {})
        for unit in sorted(units):
            fields = objects.get(unit, set())
            if not fields & {"alive=0", "alive=1"}:
                raise InputError(f"{path}: tick {tick}: {unit} has no alive row")
            if "alive=0" in fields and len(fields) > 1:
                raise InputError(f"{path}: tick {tick}: {unit} has fields while it is not alive")
            if "alive=1" in fields and fields != inventory[unit]:
                raise InputError(
                    f"{path}: tick {tick}: alive {unit} records {', '.join(sorted(fields - {'alive=1'}))}, "
                    f"not {', '.join(sorted(inventory[unit] - {'alive=1'}))}"
                )
        for hardpoint, unit in sorted(hardpoints.items()):
            if ("alive=1" in objects[unit]) != (hardpoint in objects):
                raise InputError(
                    f"{path}: tick {tick}: {hardpoint} must have rows exactly while {unit} is alive"
                )
            missing = sorted(set(HARDPOINT_FIELDS) - objects.get(hardpoint, set(HARDPOINT_FIELDS)))
            if missing:
                raise InputError(f"{path}: tick {tick}: {hardpoint} lacks {', '.join(missing)}")


def content_identity(scenario: dict) -> str:
    lines = sorted(f"{pin['archive']}|{pin['path']}|{pin['sha256']}\n" for pin in scenario["content"])
    return hashlib.sha256("".join(lines).encode("utf-8")).hexdigest()


def compare_headers(
    expected: dict, actual: dict, scenario: Optional[dict], scenario_sha256: Optional[str]
) -> None:
    for key in ("content_identity", "tick_seconds", "scenario_sha256"):
        if expected[key] != actual[key]:
            raise Divergence(
                f"headers differ in {key}: expected {json.dumps(expected[key])}, "
                f"actual {json.dumps(actual[key])}"
            )
    if scenario is not None:
        if expected["scenario_sha256"] != scenario_sha256:
            raise Divergence("headers do not name the given scenario file (scenario_sha256)")
        if expected["content_identity"] != content_identity(scenario):
            raise Divergence("headers do not carry the scenario's content identity")


def show(field: str, value: str) -> str:
    return json.dumps(value) if FIELD_KIND[field] == "label" else value


def compare_rows(
    expected: List[Row], actual: List[Row], tolerances: Dict[str, Tolerance]
) -> Tuple[int, int]:
    """Raise Divergence at the first differing key; return (tolerated, ignored) counts."""
    tolerated = ignored = 0
    index_e = index_a = 0
    while index_e < len(expected) or index_a < len(actual):
        row_e = expected[index_e] if index_e < len(expected) else None
        row_a = actual[index_a] if index_a < len(actual) else None
        if row_a is None or (row_e is not None and row_e[:3] < row_a[:3]):
            tick, label, field, value = row_e
            raise Divergence(
                f"first divergence at tick {tick}, object {label}, field {field}: "
                f"row missing from actual (expected {show(field, value)})"
            )
        if row_e is None or row_a[:3] < row_e[:3]:
            tick, label, field, value = row_a
            raise Divergence(
                f"first divergence at tick {tick}, object {label}, field {field}: "
                f"row missing from expected (actual {show(field, value)})"
            )
        tick, label, field, value_e = row_e
        value_a = row_a[3]
        index_e += 1
        index_a += 1
        tolerance = tolerances.get(field, 0)
        if tolerance == "ignore":
            ignored += 1
            continue
        if value_e == value_a:
            continue
        where = f"first divergence at tick {tick}, object {label}, field {field}"
        if FIELD_KIND[field] in ("label", "flag"):
            raise Divergence(f"{where}: expected {show(field, value_e)}, actual {show(field, value_a)}")
        difference = abs(int(value_e) - int(value_a))
        if difference > tolerance:
            raise Divergence(
                f"{where}: expected {value_e}, actual {value_a} "
                f"(difference {difference} > tolerance {tolerance})"
            )
        tolerated += 1
    return tolerated, ignored


def report_rows(
    expected: List[Row], actual: List[Row], tolerances: Dict[str, Tolerance]
) -> Tuple[List[str], bool]:
    """Every row pair: per field the largest difference and the count above tolerance.

    Rows must pair one to one (a missing or extra row is still a divergence at its key)."""
    if [row[:3] for row in expected] != [row[:3] for row in actual]:
        compare_rows(expected, actual, {field: "ignore" for field in FIELD_KIND})
    fields: Dict[str, Dict[str, int]] = {}
    for row_e, row_a in zip(expected, actual):
        tick, _label, field, value_e = row_e
        value_a = row_a[3]
        entry = fields.setdefault(field, {"rows": 0, "over": 0, "largest": 0, "tick": tick, "first_over": -1})
        entry["rows"] += 1
        tolerance = tolerances.get(field, 0)
        if tolerance == "ignore":
            continue
        if FIELD_KIND[field] in ("label", "flag"):
            difference = 0 if value_e == value_a else 1
            limit = 0
        else:
            difference = abs(int(value_e) - int(value_a))
            limit = tolerance
        if difference > entry["largest"]:
            entry["largest"], entry["tick"] = difference, tick
        if difference > limit:
            entry["over"] += 1
            if entry["first_over"] < 0:
                entry["first_over"] = tick
    lines = [f"{'field':<8} {'tolerance':>10} {'largest':>14} {'at tick':>8} {'over':>7} {'rows':>7} {'first over':>10}"]
    within = True
    for field in sorted(fields):
        entry = fields[field]
        tolerance = tolerances.get(field, 0)
        within = within and entry["over"] == 0
        first = str(entry["first_over"]) if entry["first_over"] >= 0 else "-"
        lines.append(
            f"{field:<8} {str(tolerance):>10} {entry['largest']:>14} {entry['tick']:>8} "
            f"{entry['over']:>7} {entry['rows']:>7} {first:>10}"
        )
    return lines, within


def parse_override(text: str) -> Tuple[str, Tolerance]:
    field, separator, value = text.partition("=")
    if not separator or field not in FIELD_KIND:
        raise InputError(f"--tolerance expects FIELD=VALUE with a trace field, not {text!r}")
    parsed = value if value == "ignore" else bounded_int(value, 0, INT64_MAX)
    return field, check_tolerance(f"--tolerance {field}", field, parsed)


def run(arguments: List[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("expected", type=pathlib.Path, help="reference trace (.csv)")
    parser.add_argument("actual", type=pathlib.Path, help="trace under test (.csv)")
    parser.add_argument(
        "--scenario",
        type=pathlib.Path,
        help="scenario file: supplies tolerances and must match both headers",
    )
    parser.add_argument(
        "--tolerance",
        action="append",
        default=[],
        metavar="FIELD=VALUE",
        help="override one field tolerance (raw integer or ignore); repeatable",
    )
    parser.add_argument(
        "--report",
        action="store_true",
        help="summarise every field (largest difference, rows over tolerance) instead of "
        "stopping at the first divergence",
    )
    args = parser.parse_args(arguments)
    try:
        scenario = scenario_sha256 = None
        tolerances: Dict[str, Tolerance] = {}
        if args.scenario is not None:
            scenario = load_scenario(args.scenario)
            scenario_sha256 = sha256_file(args.scenario)
            tolerances = load_tolerances(
                f"{args.scenario}: tolerances", scenario["tolerances"], bool(scenario["hardpoints"])
            )
        for text in args.tolerance:
            field, tolerance = parse_override(text)
            tolerances[field] = tolerance
        headers = [read_header(header_path(path)) for path in (args.expected, args.actual)]
        rows = [read_trace(path) for path in (args.expected, args.actual)]
        if scenario is not None:
            for path, trace in zip((args.expected, args.actual), rows):
                check_scenario_rows(path, trace, scenario)
    except InputError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    try:
        compare_headers(headers[0], headers[1], scenario, scenario_sha256)
        if scenario is not None:
            for path, trace in zip((args.expected, args.actual), rows):
                check_fire_rows(path, trace, scenario)
        if args.report:
            lines, within = report_rows(rows[0], rows[1], tolerances)
            print("\n".join(lines))
            return 0 if within else 1
        tolerated, ignored = compare_rows(rows[0], rows[1], tolerances)
    except Divergence as error:
        print(error)
        return 1
    print(
        f"traces match: {len(rows[0])} rows, ticks {rows[0][0][0]}..{rows[0][-1][0]}; "
        f"{tolerated} values within tolerance, {ignored} ignored"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(run(sys.argv[1:]))
