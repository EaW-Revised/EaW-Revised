#!/usr/bin/env python3
"""Contract tests for tools/compare_traces.py and the checked-in fidelity scenarios."""

from __future__ import annotations

import argparse
import copy
import hashlib
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
from typing import Dict, List, Tuple

ONE = 1 << 24
FAILURES: List[str] = []


def check(condition: bool, message: str) -> None:
    if not condition:
        FAILURES.append(message)


def load_module(tool: pathlib.Path):
    spec = importlib.util.spec_from_file_location("compare_traces", tool)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def synthetic_rows(ticks: int) -> Dict[Tuple[int, str, str], str]:
    """Ticks 0..ticks-1 of S-01: three units and the hardpoint under test."""
    rows: Dict[Tuple[int, str, str], str] = {}
    units = {"defender": (0, 0, 0), "fighter.1": (200, 20, 0), "bomber.1": (420, -30, 0)}
    for tick in range(ticks):
        for label, (x, y, z) in units.items():
            rows[(tick, label, "alive")] = "1"
            for axis, value in zip("xyz", (x, y, z)):
                rows[(tick, label, f"pos.{axis}")] = str(value * ONE + tick)
            forward = (ONE, 0, 0) if label == "defender" else (-ONE, 0, 0)
            for axis, value in zip("xyz", forward):
                rows[(tick, label, f"fwd.{axis}")] = str(value)
            rows[(tick, label, "hull")] = str(100 * ONE)
            rows[(tick, label, "shield")] = str(70 * ONE)
        rows[(tick, "defender/ion", "target")] = "bomber.1" if tick >= 1 else ""
        rows[(tick, "defender/ion", "shots")] = "1" if tick == 2 else "0"
    return rows


def write_trace(
    path: pathlib.Path,
    rows: Dict[Tuple[int, str, str], str],
    header: dict,
    newline: str = "\n",
) -> None:
    lines = ["tick,object,field,value"] + [
        f"{tick},{label},{field},{value}" for (tick, label, field), value in sorted(rows.items())
    ]
    path.write_bytes((newline.join(lines) + newline).encode("utf-8"))
    path.with_suffix(".json").write_bytes((json.dumps(header, indent=2) + "\n").encode("utf-8"))


def invoke(tool: pathlib.Path, *arguments: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(tool), *arguments],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )


def test_q24(module) -> None:
    cases = {
        0x3F800000: ONE,  # 1.0
        0xBF800000: -ONE,  # -1.0
        0x3F000000: ONE // 2,  # 0.5
        0x33000000: 0,  # 2^-25: half of one raw unit, ties to even zero
        0x33C00000: 2,  # 1.5 * 2^-24: ties to even two
        0xB3C00000: -2,  # sign-symmetric rounding
        0x33A00000: 1,  # 1.25 * 2^-24: nearest
        0x00000001: 0,  # smallest subnormal
        0x80000000: 0,  # negative zero
        0x4B7FFFFF: 16777215 * ONE,  # largest odd binary32 integer below 2^24
    }
    for bits, expected in cases.items():
        actual = module.q24_from_binary32_bits(bits)
        check(actual == expected, f"q24 of {bits:#010x}: expected {expected}, got {actual}")
    check(module.q24_from_binary32_bits(0x52FFFFFF) == 0x7FFFFF8000000000, "largest Q24 binary32")
    for bits in (0x53000000, 0x7F800000, 0x7FC00000):  # 2^39 overflows; infinity; NaN
        try:
            module.q24_from_binary32_bits(bits)
            FAILURES.append(f"q24 of {bits:#010x} should fail")
        except ValueError:
            pass


def check_conversion_bounds(name: str, tolerances: dict) -> None:
    """The shape docs/traces.md gives each tolerance: exact labels, binary32 steps, trig bound."""
    for field in ("alive", "target"):
        check(tolerances.get(field, 0) == 0, f"{name}: {field} is compared exactly")
    for field in ("pos.x", "pos.y", "pos.z", "hull", "shield"):
        value = tolerances[field]
        check(
            value == 0 or (value != "ignore" and value & (value - 1) == 0),
            f"{name}: {field} tolerance {value} is 0 or a binary32 step (a power of two in raw units)",
        )
    fwd = [tolerances[f"fwd.{axis}"] for axis in "xyz"]
    check(
        len(set(fwd)) == 1 and fwd[0] != "ignore" and fwd[0] >= 16,
        f"{name}: fwd tolerances {fwd} are one value, at least the 16-raw whole-degree trig bound",
    )


def test_scenarios(module, scenarios: pathlib.Path) -> dict:
    files = sorted(scenarios.glob("S-*.json"))
    check({"S-01", "S-02", "S-03"}.issubset({path.name[:4] for path in files}), "S-01..S-03 are present")
    loaded = {}
    for path in files:
        check(b"\r" not in path.read_bytes(), f"{path.name} must keep LF bytes (hashed)")
        scenario = module.load_scenario(path)
        check(path.name.startswith(f"{scenario['id']}-{scenario['label']}"), f"{path.name} name")
        loaded[scenario["id"]] = scenario
        check_conversion_bounds(path.name, scenario["tolerances"])
    base = loaded["S-01"]
    mutations = {
        "empty expectation without record-only": lambda s: s.update(expect=[]),
        "record-only with an expectation": lambda s: s.update(record_only=True),
        "nonboolean record-only": lambda s: s.update(record_only="yes"),
        "move without destination": lambda s: s["events"].append({"tick": 1, "action": "move", "unit": "defender"}),
        "stop with extra argument": lambda s: s["events"].append({"tick": 1, "action": "stop", "unit": "defender", "position": [0, 0, 0]}),
        "duplicate unit label": lambda s: s["units"].append(copy.deepcopy(s["units"][0])),
        "event for an undeclared unit": lambda s: s["events"].append(
            {"tick": 10, "action": "remove", "unit": "ghost"}
        ),
        "missing tolerance": lambda s: s["tolerances"].pop("pos.x"),
        "non-zero label tolerance": lambda s: s["tolerances"].update(target=1),
        "hardpoint on an unknown unit": lambda s: s["hardpoints"][0].update(label="ghost/ion"),
        "expectation on an unknown object": lambda s: s["expect"][0].update(value="ghost"),
        "unpinned map": lambda s: s["map"].update(path="data/art/maps/other.ted"),
        "boolean format_version": lambda s: s.update(format_version=True),
        "fractional format_version": lambda s: s.update(format_version=1.0),
        "fractional staging position": lambda s: s["units"][0].update(position=[0.5, 0, 0]),
        "fractional staging facing": lambda s: s["units"][0].update(facing_degrees=0.5),
        "initial pose on a start unit": lambda s: s["units"][0].update(apply_initial_pose=True),
        "until_tick past the last recorded tick": lambda s: next(
            e for e in s["expect"] if "until_tick" in e
        ).update(until_tick=s["duration_ticks"]),
    }
    with tempfile.TemporaryDirectory() as temporary:
        for name, value in (("boolean position", [True, 0, 0]), ("infinite position", [float("inf"), 0, 0]),
                            ("outside source bounds", [module.MAX_SOURCE_UNITS + 1, 0, 0])):
            scenario = copy.deepcopy(loaded["S-97"])
            craft = next(unit for unit in scenario["units"] if unit.get("apply_initial_pose"))
            craft["position"] = value
            path = pathlib.Path(temporary) / "initial-pose-invalid.json"
            path.write_text(json.dumps(scenario), encoding="utf-8")
            try:
                module.load_scenario(path)
                FAILURES.append(f"initial pose with {name} was accepted")
            except module.InputError:
                pass
        for name, mutate in mutations.items():
            scenario = copy.deepcopy(base)
            mutate(scenario)
            path = pathlib.Path(temporary) / "S-99-mutated.json"
            path.write_text(json.dumps(scenario), encoding="utf-8")
            try:
                module.load_scenario(path)
                FAILURES.append(f"scenario with {name} was accepted")
            except module.InputError:
                pass
        ordered = copy.deepcopy(loaded["S-26"])
        ordered["events"][0]["target"] = "ghost"
        path = pathlib.Path(temporary) / "S-26-invalid-target.json"
        path.write_text(json.dumps(ordered), encoding="utf-8")
        try:
            module.load_scenario(path)
            FAILURES.append("attack targeting an undeclared unit was accepted")
        except module.InputError:
            pass
        ordered["events"][0]["target"] = []
        path.write_text(json.dumps(ordered), encoding="utf-8")
        try:
            module.load_scenario(path)
            FAILURES.append("attack targeting a non-label was accepted")
        except module.InputError:
            pass
        # #561: a targeted ability names another live staged unit.
        shot = copy.deepcopy(loaded["S-52"])
        for target in ("ghost", "ywing"):
            shot["events"][-1]["target"] = target
            path = pathlib.Path(temporary) / "S-52-invalid-target.json"
            path.write_text(json.dumps(shot), encoding="utf-8")
            try:
                module.load_scenario(path)
                FAILURES.append(f"ability targeting {target} was accepted")
            except module.InputError:
                pass
        # #392: an attack on a target spawned by an event, on its spawn tick, is a valid scenario
        # (sim_headless stages it too: test_scenario_traces.py).
        spawned = copy.deepcopy(loaded["S-26"])
        spawned["units"][1]["spawn"] = "event"
        spawned["events"].insert(0, {"tick": 30, "action": "spawn", "unit": "target"})
        path.write_text(json.dumps(spawned), encoding="utf-8")
        try:
            module.load_scenario(path)
        except module.InputError as error:
            FAILURES.append(f"attack on an event-spawned target was refused: {error}")
        window_mutations = {
            "an empty fire window list": lambda w: w.clear(),
            "a fire window on an undeclared hardpoint": lambda w: w[0]["hardpoints"].append("shooter/ghost"),
            "a fire window listing a hardpoint twice": lambda w: w[0]["hardpoints"].append(w[0]["hardpoints"][0]),
            "a fire window past the last tick": lambda w: w[-1].update(to_tick=1800),
            "a reversed fire window": lambda w: w[1].update(from_tick=300, to_tick=299),
            "max_shots below min_shots": lambda w: w[1].update(max_shots=0),
            "an unbounded fire window": lambda w: w[1].update(min_shots=0, max_shots=None),
            "a fractional min_shots": lambda w: w[1].update(min_shots=1.5),
            "a fire window without text": lambda w: w[1].pop("text"),
        }
        for name, mutate in window_mutations.items():
            scenario = copy.deepcopy(loaded["S-26"])
            mutate(scenario["fire_windows"])
            path.write_text(json.dumps(scenario), encoding="utf-8")
            try:
                module.load_scenario(path)
                FAILURES.append(f"scenario with {name} was accepted")
            except module.InputError:
                pass
        scenario = copy.deepcopy(loaded["S-26"])
        scenario["units"][1]["shieldless"] = False
        path.write_text(json.dumps(scenario), encoding="utf-8")
        try:
            module.load_scenario(path)
            FAILURES.append("shieldless false was accepted")
        except module.InputError:
            pass
    # #392: the rear arc fixtures bound every hardpoint's shots (docs/traces.md#fire-windows).
    for case in ("S-22", "S-23", "S-24", "S-25", "S-26", "S-27", "S-32", "S-33"):
        covered = {label for window in loaded[case].get("fire_windows", []) for label in window["hardpoints"]}
        declared = {hardpoint["label"] for hardpoint in loaded[case]["hardpoints"]}
        check(covered == declared, f"{case}: fire windows cover every hardpoint: {sorted(declared - covered)}")
    return loaded


def test_comparer(tool: pathlib.Path, module, scenario_path: pathlib.Path) -> None:
    scenario = module.load_scenario(scenario_path)
    header = {
        "format": "eawr-trace",
        "format_version": 1,
        "source": "original",
        "content_identity": module.content_identity(scenario),
        "tick_seconds": {"numerator": 1, "denominator": 30},
        "build_identity": {"kind": "executable-sha256", "value": scenario["executable"]["sha256"]},
        "scenario_sha256": hashlib.sha256(scenario_path.read_bytes()).hexdigest(),
        "run": 1,
    }
    remake = dict(header, source="remake", build_identity={"kind": "git-sha", "value": "a" * 40})
    del remake["run"]
    ticks = scenario["duration_ticks"]
    rows = synthetic_rows(ticks)

    with tempfile.TemporaryDirectory() as temporary:
        root = pathlib.Path(temporary)
        expected = root / "expected.csv"
        write_trace(expected, rows, header)

        def compare(changed: dict, *extra: str, changed_header: dict = None) -> subprocess.CompletedProcess:
            actual = root / "actual.csv"
            write_trace(actual, changed, changed_header or remake)
            return invoke(tool, str(expected), str(actual), *extra)

        same = compare(rows, "--scenario", str(scenario_path))
        check(same.returncode == 0 and "traces match: " in same.stdout, f"identical: {same.stdout}")

        changed = dict(rows)
        changed[(2, "fighter.1", "pos.x")] = str(200 * ONE + 2 + 5)
        changed[(4, "bomber.1", "pos.y")] = "0"
        result = compare(changed)
        check(
            result.returncode == 1
            and f"first divergence at tick 2, object fighter.1, field pos.x: expected {200 * ONE + 2}, "
            f"actual {200 * ONE + 7} (difference 5 > tolerance 0)" in result.stdout,
            f"changed value: {result.stdout}",
        )
        result = compare(changed, "--tolerance", "pos.x=5", "--tolerance", "pos.y=4")
        check(
            result.returncode == 1 and "tick 4, object bomber.1, field pos.y" in result.stdout,
            f"tolerance is inclusive and per field: {result.stdout}",
        )
        # --report (#70): every field, its largest difference and the rows over tolerance.
        report = compare(changed, "--report", "--tolerance", "pos.x=5")
        lines = {line.split()[0]: line.split() for line in report.stdout.splitlines()[1:] if line.strip()}
        check(
            report.returncode == 1
            and lines.get("pos.x", [])[1:6] == ["5", "5", "2", "0", str(ticks * 3)]
            and lines.get("pos.y", [])[2:5] == [str(30 * ONE - 4), "4", "1"]
            and lines.get("pos.y", [])[6:] == ["4"]
            and lines.get("hull", [])[2:5] == ["0", "0", "0"],
            f"report: {report.stdout}",
        )
        report = compare(changed, "--report", "--tolerance", "pos.x=5", "--tolerance", f"pos.y={30 * ONE}")
        check(report.returncode == 0, f"report within tolerance: {report.stdout}")
        tolerated = dict(rows)
        tolerated[(3, "defender/ion", "shots")] = "2"
        result = compare(tolerated, "--scenario", str(scenario_path))
        check(
            result.returncode == 0 and f"0 values within tolerance, {ticks} ignored" in result.stdout,  # shots
            f"scenario ignored field: {result.stdout}",
        )
        tolerated[(2, "fighter.1", "pos.x")] = str(200 * ONE + 2 + 5)
        result = compare(tolerated, "--scenario", str(scenario_path))
        check(
            result.returncode == 1 and "(difference 5 > tolerance 0)" in result.stdout,
            f"scenario tolerance: {result.stdout}",
        )
        result = compare(tolerated, "--scenario", str(scenario_path), "--tolerance", "pos.x=5")
        check(
            result.returncode == 0 and f"1 values within tolerance, {ticks} ignored" in result.stdout,
            f"override on top of scenario tolerances: {result.stdout}",
        )
        bound = scenario["tolerances"]["fwd.y"]
        check(bound != "ignore" and bound > 0, "fwd allows the trig conversion bound (docs/traces.md)")
        turned = dict(rows)
        turned[(5, "fighter.1", "fwd.y")] = str(bound)
        result = compare(turned, "--scenario", str(scenario_path))
        check(
            result.returncode == 0 and f"1 values within tolerance, {ticks} ignored" in result.stdout,
            f"fwd within the conversion bound: {result.stdout}",
        )
        turned[(6, "fighter.1", "fwd.y")] = str(-bound - 1)
        result = compare(turned, "--scenario", str(scenario_path))
        check(
            result.returncode == 1
            and f"tick 6, object fighter.1, field fwd.y: expected 0, actual {-bound - 1} "
            f"(difference {bound + 1} > tolerance {bound})" in result.stdout,
            f"fwd past the conversion bound: {result.stdout}",
        )
        shielded = dict(rows)
        shielded[(3, "bomber.1", "shield")] = "0"
        result = compare(shielded, "--scenario", str(scenario_path))
        check(
            result.returncode == 1 and "tick 3, object bomber.1, field shield" in result.stdout,
            f"shield compared: {result.stdout}",
        )

        missing = dict(rows)
        del missing[(3, "bomber.1", "alive")]
        missing[(5, "bomber.1", "hull")] = "0"
        result = compare(missing)
        check(
            result.returncode == 1
            and "first divergence at tick 3, object bomber.1, field alive: "
            "row missing from actual (expected 1)" in result.stdout,
            f"missing row: {result.stdout}",
        )
        extra = dict(rows)
        extra[(ticks, "bomber.1", "alive")] = "0"
        result = compare(extra)
        check(
            result.returncode == 1
            and f"tick {ticks}, object bomber.1, field alive: row missing from expected (actual 0)"
            in result.stdout,
            f"extra row: {result.stdout}",
        )

        retarget = dict(rows)
        retarget[(4, "defender/ion", "target")] = "fighter.1"
        retarget[(5, "defender/ion", "target")] = ""
        result = compare(retarget, "--scenario", str(scenario_path))
        check(
            result.returncode == 1
            and 'first divergence at tick 4, object defender/ion, field target: expected "bomber.1", '
            'actual "fighter.1"' in result.stdout,
            f"changed target: {result.stdout}",
        )
        result = compare(retarget, "--tolerance", "target=ignore")
        check(result.returncode == 0, f"ignored target: {result.stdout}")

        slower = dict(remake, tick_seconds={"numerator": 1, "denominator": 15})
        result = compare(rows, changed_header=slower)
        check(
            result.returncode == 1 and "headers differ in tick_seconds" in result.stdout,
            f"cadence mismatch: {result.stdout}",
        )
        other = dict(remake, scenario_sha256="0" * 64)
        result = compare(rows, changed_header=other)
        check(result.returncode == 1 and "scenario_sha256" in result.stdout, "scenario mismatch")
        foreign = dict(header, scenario_sha256="0" * 64)
        write_trace(expected, rows, foreign)
        result = compare(rows, "--scenario", str(scenario_path), changed_header=foreign)
        check(result.returncode == 1 and "given scenario file" in result.stdout, "scenario file")
        write_trace(expected, rows, header)

        def damage_header(**changes):
            return lambda path: path.with_suffix(".json").write_text(
                json.dumps(dict(remake, **changes)), encoding="utf-8"
            )

        malformed = {
            "CRLF lines": lambda path: write_trace(path, rows, remake, "\r\n"),
            "unsorted rows": lambda path: path.write_bytes(
                b"tick,object,field,value\n1,defender,alive,1\n0,defender,alive,1\n"
            ),
            "duplicate row": lambda path: path.write_bytes(
                b"tick,object,field,value\n0,defender,alive,1\n0,defender,alive,1\n"
            ),
            "unit field on a hardpoint": lambda path: path.write_bytes(
                b"tick,object,field,value\n0,defender/ion,alive,1\n"
            ),
            "fractional value": lambda path: path.write_bytes(
                b"tick,object,field,value\n0,defender,pos.x,1.5\n"
            ),
            "BOM": lambda path: path.write_bytes(b"\xef\xbb\xbftick,object,field,value\n"),
            "missing header JSON": lambda path: (
                path.write_bytes(b"tick,object,field,value\n"),
                path.with_suffix(".json").unlink(),
            ),
            "4301-digit Q24 value": lambda path: path.write_bytes(
                b"tick,object,field,value\n0,defender,pos.x," + b"1" * 4301 + b"\n"
            ),
            "4301-digit tick": lambda path: path.write_bytes(
                b"tick,object,field,value\n" + b"1" * 4301 + b",defender,alive,1\n"
            ),
            "boolean format_version": damage_header(format_version=True),
            "fractional format_version": damage_header(format_version=1.0),
            "list build_identity kind": damage_header(
                build_identity={"kind": ["git-sha"], "value": "a" * 40}
            ),
            "deeply nested header": lambda path: path.with_suffix(".json").write_bytes(
                b"[" * 100000 + b"]" * 100000
            ),
        }
        for name, damage in malformed.items():
            actual = root / "actual.csv"
            write_trace(actual, rows, remake)
            damage(actual)
            result = invoke(tool, str(expected), str(actual))
            check(result.returncode == 2 and "error:" in result.stdout, f"{name}: {result.stdout}")
        actual = root / "actual.csv"
        write_trace(actual, rows, remake)
        result = invoke(tool, str(expected), str(actual), "--tolerance", "pos.x=" + "1" * 4301)
        check(result.returncode == 2 and "error:" in result.stdout, f"4301-digit tolerance: {result.stdout}")
        test_incomplete(tool, root, rows, header, remake, scenario_path)


def test_incomplete(
    tool: pathlib.Path,
    root: pathlib.Path,
    rows: Dict[Tuple[int, str, str], str],
    header: dict,
    remake: dict,
    scenario_path: pathlib.Path,
) -> None:
    """Two equal traces that do not record the whole scenario are malformed, not a match."""
    ticks = max(tick for tick, _, _ in rows) + 1

    def without(drop) -> dict:
        return {key: value for key, value in rows.items() if not drop(*key)}

    def killed(unit: str, tick: int, keep_fields: bool = False, keep_hardpoint: bool = False) -> dict:
        changed = without(
            lambda t, label, field: t == tick
            and (
                (label == unit and field != "alive" and not keep_fields)
                or (label.startswith(unit + "/") and not keep_hardpoint)
            )
        )
        changed[(tick, unit, "alive")] = "0"
        return changed

    def compare(changed: dict, *extra: str, scenario_sha256=header["scenario_sha256"]):
        expected, actual = root / "expected.csv", root / "actual.csv"
        write_trace(expected, changed, dict(header, scenario_sha256=scenario_sha256))
        write_trace(actual, changed, dict(remake, scenario_sha256=scenario_sha256))
        return invoke(tool, str(expected), str(actual), *extra)

    scenario = ("--scenario", str(scenario_path))
    cases = {
        "no rows": {},
        "a declared unit is never recorded": without(lambda tick, label, field: label == "fighter.1"),
        "the trace stops before the last tick": without(lambda tick, label, field: tick == ticks - 1),
        "a tick is skipped": without(lambda tick, label, field: tick == 3),
        "rows past the last tick": {**rows, (ticks, "defender", "alive"): "1"},
        "an undeclared unit": {**rows, (0, "ghost", "alive"): "1"},
        "unit fields while it is not alive": killed("fighter.1", 7, keep_fields=True),
        "hardpoint rows while its unit is not alive": killed("defender", 7, keep_hardpoint=True),
        "no hardpoint rows while its unit is alive": without(
            lambda tick, label, field: tick == 7 and label == "defender/ion"
        ),
    }
    for name, changed in cases.items():
        result = compare(changed, *scenario)
        check(result.returncode == 2 and "error:" in result.stdout, f"incomplete, {name}: {result.stdout}")
    for unit in ("fighter.1", "defender"):
        result = compare(killed(unit, 7), *scenario)
        check(result.returncode == 0, f"{unit} not alive at tick 7 keeps only its alive row: {result.stdout}")
    result = compare({}, scenario_sha256=None)
    check(result.returncode == 2 and "error:" in result.stdout, f"empty replay traces: {result.stdout}")


def scenario_trace(scenario: dict, shots) -> Dict[Tuple[int, str, str], str]:
    """A complete trace of a scenario whose units live throughout; shots(tick, label) per hardpoint."""
    rows: Dict[Tuple[int, str, str], str] = {}
    for tick in range(scenario["duration_ticks"]):
        for unit in scenario["units"]:
            label = unit["label"]
            rows[(tick, label, "alive")] = "1"
            for axis, value in zip("xyz", unit["position"]):
                rows[(tick, label, f"pos.{axis}")] = str(value * ONE)
            for axis, value in zip("xyz", (ONE, 0, 0)):
                rows[(tick, label, f"fwd.{axis}")] = str(value)
            rows[(tick, label, "hull")] = str(100 * ONE)
            if not unit.get("shieldless"):
                rows[(tick, label, "shield")] = str(70 * ONE)
        for hardpoint in scenario["hardpoints"]:
            rows[(tick, hardpoint["label"], "shots")] = str(shots(tick, hardpoint["label"]))
            rows[(tick, hardpoint["label"], "target")] = ""
    return rows


def test_fire_windows(tool: pathlib.Path, module, scenarios: pathlib.Path) -> None:
    """#392: fire windows reject a trace in which no weapon fires, or one fires where none may;
    a scenario trace must record every field of its units and hardpoints."""

    def bursts(tick: int, label: str) -> int:
        # A 5-pulse burst every 100 ticks from tick 50, pulses 6 ticks apart (the Tartan's cadence).
        return 1 if tick >= 50 and (tick - 50) % 100 < 25 and (tick - 50) % 6 == 0 else 0

    with tempfile.TemporaryDirectory() as temporary:
        root = pathlib.Path(temporary)

        def compare(path: pathlib.Path, rows: dict) -> subprocess.CompletedProcess:
            scenario = module.load_scenario(path)
            header = {
                "format": "eawr-trace",
                "format_version": 1,
                "source": "original",
                "content_identity": module.content_identity(scenario),
                "tick_seconds": {"numerator": 1, "denominator": 30},
                "build_identity": {"kind": "executable-sha256", "value": scenario["executable"]["sha256"]},
                "scenario_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            }
            expected, actual = root / "expected.csv", root / "actual.csv"
            write_trace(expected, rows, header)
            write_trace(actual, rows, dict(header, source="remake"))
            return invoke(tool, str(expected), str(actual), "--scenario", str(path))

        ordered_path = next(scenarios.glob("S-26-*.json"))
        ordered = module.load_scenario(ordered_path)
        firing = scenario_trace(ordered, bursts)
        result = compare(ordered_path, firing)
        check(result.returncode == 0 and "traces match" in result.stdout, f"S-26 bursts: {result.stdout}")
        result = compare(ordered_path, scenario_trace(ordered, lambda tick, label: 0))
        check(
            result.returncode == 1 and "fire window 1, shooter/hp_tartan_cruiser_00 fired 0 shots in ticks 30..299"
            in result.stdout,
            f"S-26 without a shot must diverge: {result.stdout}",
        )
        early = scenario_trace(ordered, lambda tick, label: 1 if tick == 20 else bursts(tick, label))
        result = compare(ordered_path, early)
        check(result.returncode == 1 and "fire window 0" in result.stdout, f"S-26 fire before the order: {result.stdout}")
        rapid = scenario_trace(ordered, lambda tick, label: 1 if tick >= 300 else bursts(tick, label))
        result = compare(ordered_path, rapid)
        check(result.returncode == 1 and "expected 5..25" in result.stdout, f"S-26 fire every tick: {result.stdout}")

        idle_path = next(scenarios.glob("S-22-*.json"))
        idle = module.load_scenario(idle_path)
        result = compare(idle_path, scenario_trace(idle, lambda tick, label: 0))
        check(result.returncode == 0, f"S-22 silent: {result.stdout}")
        stray = scenario_trace(idle, lambda tick, label: int(tick == 900 and label.endswith("_02")))
        result = compare(idle_path, stray)
        check(
            result.returncode == 1 and "shooter/hp_tartan_cruiser_02 fired 1 shots in ticks 0..1799, expected 0..0"
            in result.stdout,
            f"S-22 with a shot dead astern must diverge: {result.stdout}",
        )

        # The field inventory: both traces omitting the same field is malformed, not a match.
        omissions = {
            "shots": lambda tick, label, field: label == "shooter/hp_tartan_cruiser_03" and field == "shots",
            "target": lambda tick, label, field: tick == 700 and "/" in label and field == "target",
            "fwd.y": lambda tick, label, field: label == "shooter" and field == "fwd.y",
            "pos.x": lambda tick, label, field: tick >= 1000 and label == "target" and field == "pos.x",
            "hull": lambda tick, label, field: label == "target" and field == "hull",
            "shield": lambda tick, label, field: label == "shooter" and field == "shield",
        }
        for name, drop in omissions.items():
            rows = {key: value for key, value in firing.items() if not drop(*key)}
            result = compare(ordered_path, rows)
            check(result.returncode == 2 and "error:" in result.stdout, f"a trace without {name}: {result.stdout}")
        shieldless = copy.deepcopy(ordered)
        shieldless["units"][1]["shieldless"] = True
        shieldless_path = root / "S-26-shieldless.json"
        shieldless_path.write_text(json.dumps(shieldless), encoding="utf-8")
        result = compare(shieldless_path, scenario_trace(shieldless, bursts))
        check(result.returncode == 0, f"a shieldless unit records no shield: {result.stdout}")
        result = compare(shieldless_path, firing)
        check(result.returncode == 2 and "error:" in result.stdout, f"shield rows on a shieldless unit: {result.stdout}")


def test_producer(tool: pathlib.Path, program: pathlib.Path, replay: pathlib.Path) -> None:
    """sim_headless --trace-out output is a valid trace that the comparer accepts."""
    with tempfile.TemporaryDirectory() as temporary:
        root = pathlib.Path(temporary)
        traces = []
        for workers in ("1", "4"):
            trace = root / f"workers-{workers}.csv"
            completed = subprocess.run(
                [
                    str(program),
                    "--replay",
                    str(replay),
                    "--hash-out",
                    str(root / f"hashes-{workers}.csv"),
                    "--trace-out",
                    str(trace),
                    "--workers",
                    workers,
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            check(completed.returncode == 0, f"sim_headless --trace-out: {completed.stdout}")
            traces.append(str(trace))
        result = invoke(tool, *traces)
        check(
            result.returncode == 0 and "ticks 0..5" in result.stdout,
            f"sim_headless traces must compare equal: {result.stdout}",
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=pathlib.Path, required=True)
    parser.add_argument("--scenarios", type=pathlib.Path, required=True)
    parser.add_argument("--program", type=pathlib.Path, help="sim_headless, for a producer check")
    parser.add_argument("--replay", type=pathlib.Path, help="replay fixture for --program")
    args = parser.parse_args()
    module = load_module(args.tool)
    test_q24(module)
    test_scenarios(module, args.scenarios)
    test_comparer(args.tool, module, args.scenarios / "S-01-priority-beats-distance.json")
    test_fire_windows(args.tool, module, args.scenarios)
    if args.program is not None:
        test_producer(args.tool, args.program, args.replay)
    for failure in FAILURES:
        print(f"FAIL: {failure}")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    raise SystemExit(main())
