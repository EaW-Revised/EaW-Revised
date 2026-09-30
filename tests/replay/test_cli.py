#!/usr/bin/env python3
"""Exercise the documented sim_headless replay/hash/worker contract."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import tempfile


EXPECTED = (
    "tick,sha256\n"
    "1,5f13df4359da99bbb781435f31303c6184510a08577f86868f58a02766c1da7e\n"
    "2,8a45cdf1c63c1e54b56b09a65dcaa1927adf83cad189592d1b24f5644499c98a\n"
    "3,f43e3626a0b5f1ea20aa8e5d53ccf9a12d125533a99e4139bf2d00822f92df54\n"
    "4,f99072695d44e55bcb3d02986787340221beac6fd6587c7b048f0ea71350eed9\n"
    "5,b716e13851efdfd2854dd16ddeefe14f5f031d81c2e8a927aa0109ef67e484bc\n"
)


# Replay-v2 was added without touching v1: these are the committed v1 fixture bytes.
V1_FIXTURE_SHA256 = {
    "original-v1.eawr-replay": "fcf7f050a4ae8be540def3e4609fc7ff68a21e4ed4fce59e46c1fc62e59a4f90",
    "zero-tick-v1.eawr-replay": "f92cda7a6f86d6a321a1f0a8858b35318959d54b6f35a6457f353d0dbeab9345",
    "mutated-count.eawr-replay": "54701bba43c0cf4eb339bd7fdf62f907ce075237f71519a2eec5d452361f46f1",
    "mutated-length-zero.eawr-replay": "e5c0b79bfe2e36782e0d756a479b5900db6a5690e41dbd4b52ccca4e67f7ba50",
    "mutated-order.eawr-replay": "3db71a83eac4a8e956bb2a2037c4beb813aaede2f4fc8642f7a4ebf5598453b4",
    "mutated-overflow.eawr-replay": "2fffae87684f816e6513d35da784bbc8bda5295b890d0194fa212615297833ee",
    "mutated-trailing.eawr-replay": "2b3564aca6736d4122543e609662a24c683ce98c76b32739dfc957fb08adfe8a",
    "mutated-version.eawr-replay": "520b700df650d37eb3d2127c35d709ec55ee7e62b6b6c08631c6ef1e10198811",
}


def run(program: str, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [program, *arguments], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
    )


def tree(root: pathlib.Path) -> dict[str, bytes | None]:
    """Every path under root with its bytes, or None for a directory."""
    return {
        path.relative_to(root).as_posix(): None if path.is_dir() else path.read_bytes()
        for path in root.rglob("*")
    }


def check_tactical_aliases(program: str, replay: pathlib.Path, root: pathlib.Path) -> str:
    """With a replay-v2 input, the replay and every output must be different files too."""
    aliases = root / "tactical-aliases"
    aliases.mkdir()
    replay_bytes = replay.read_bytes()
    copy = aliases / "input.eawr-replay"
    copy.write_bytes(replay_bytes)
    dotted = f"{aliases}{os.sep}.{os.sep}dotted.csv"
    cases = {
        "hash and event output are one file": [
            "--hash-out", str(aliases / "same.csv"), "--events-out", str(aliases / "same.csv"),
        ],
        "snapshot output overwrites the replay": [
            "--hash-out", str(aliases / "hashes.csv"), "--snapshot-out", str(copy),
        ],
        "event and snapshot output are one file through ./": [
            "--hash-out", str(aliases / "hashes.csv"),
            "--events-out", dotted,
            "--snapshot-out", str(aliases / "dotted.csv"),
        ],
        "snapshot output differs from the hash output only in case": [
            "--hash-out", str(aliases / "Cased.csv"), "--snapshot-out", str(aliases / "cased.csv"),
        ],
        "trace output with a replay-v2 input": [
            "--hash-out", str(aliases / "hashes.csv"), "--trace-out", str(aliases / "trace.csv"),
        ],
    }
    for name, outputs in cases.items():
        rejected = run(program, "--replay", str(copy), *outputs)
        if rejected.returncode != 2 or "EAWR-CORE-0001" not in rejected.stdout:
            return f"{name}: not rejected as an argument error ({rejected.returncode}) {rejected.stdout}"
        if copy.read_bytes() != replay_bytes:
            return f"{name}: the replay input was overwritten"
        written = sorted(path.name for path in aliases.iterdir() if path != copy)
        if written:
            return f"{name}: wrote {written} before rejecting the paths"
    return ""


def check_scenario_arguments(program: str, replay: pathlib.Path, root: pathlib.Path) -> str:
    """--scenario (#70) needs --game-root and --trace-out, takes no replay-mode output, and its
    trace header may not overwrite the scenario; a scenario that cannot be read publishes nothing."""
    work = root / "scenario-arguments"
    work.mkdir()
    scenario = work / "S-99.json"
    scenario.write_text("{}", encoding="utf-8")
    game = str(work)
    trace = str(work / "trace.csv")
    cases = {
        "scenario without a game root": ["--scenario", str(scenario), "--trace-out", trace],
        "scenario without a trace": ["--scenario", str(scenario), "--game-root", game],
        "scenario with a replay": ["--scenario", str(scenario), "--game-root", game, "--trace-out", trace,
                                   "--replay", str(replay)],
        "scenario with a census": ["--scenario", str(scenario), "--game-root", game, "--trace-out", trace,
                                   "--census-out", str(work / "census.json")],
        "scenario with ticks": ["--scenario", str(scenario), "--game-root", game, "--trace-out", trace,
                                "--ticks", "3"],
        "trace header overwrites the scenario": ["--scenario", str(scenario), "--game-root", game,
                                                 "--trace-out", str(work / "S-99.csv")],
    }
    for name, arguments in cases.items():
        rejected = run(program, *arguments)
        if rejected.returncode != 2 or "EAWR-CORE-0001" not in rejected.stdout:
            return f"{name}: not rejected as an argument error ({rejected.returncode}) {rejected.stdout}"
    missing = run(program, "--scenario", str(work / "missing.json"), "--game-root", game, "--trace-out", trace)
    if missing.returncode != 3 or "EAWR-SIM-CLI-0003" not in missing.stdout:
        return f"an unreadable scenario is not a scenario error ({missing.returncode}) {missing.stdout}"
    written = sorted(path.name for path in work.iterdir() if path != scenario)
    if written:
        return f"scenario arguments wrote {written}"
    return ""


def check_tactical(program: str, fixtures: pathlib.Path, root: pathlib.Path) -> bool:
    """Replay-v2: all three outputs equal the independent oracle for 1, 2 and 4 workers."""
    replay = fixtures / "tactical-v2.eawr-replay"
    goldens = {
        kind: (fixtures / f"tactical-v2.{kind}.csv").read_bytes()
        for kind in ("hashes", "events", "snapshots")
    }
    for workers in (1, 2, 4):
        outputs = {kind: root / f"tactical-{workers}.{kind}.csv" for kind in goldens}
        completed = run(
            program,
            "--replay", str(replay),
            "--hash-out", str(outputs["hashes"]),
            "--events-out", str(outputs["events"]),
            "--snapshot-out", str(outputs["snapshots"]),
            "--workers", str(workers),
        )
        if completed.returncode != 0:
            print(completed.stdout, end="")
            print(f"replay-v2 with {workers} workers failed with {completed.returncode}")
            return False
        for kind, golden in goldens.items():
            if outputs[kind].read_bytes() != golden:
                print(f"replay-v2 {kind} with {workers} workers differs from the independent oracle")
                return False

    hash_only = root / "tactical-hash-only.csv"
    completed = run(program, "--replay", str(replay), "--hash-out", str(hash_only))
    if completed.returncode != 0 or hash_only.read_bytes() != goldens["hashes"]:
        print("replay-v2 without event/snapshot outputs did not write the oracle hashes")
        return False

    mutations = json.loads((fixtures / "tactical-v2.mutations.json").read_text(encoding="utf-8"))
    for mutation in mutations["mutations"]:
        outputs = [root / f"{mutation['file']}.{kind}.csv" for kind in goldens]
        completed = run(
            program,
            "--replay", str(fixtures / mutation["file"]),
            "--hash-out", str(outputs[0]),
            "--events-out", str(outputs[1]),
            "--snapshot-out", str(outputs[2]),
            "--workers", "2",
        )
        if completed.returncode == 0 or mutation["code"] not in completed.stdout:
            print(completed.stdout, end="")
            print(f"{mutation['file']} was not rejected with {mutation['code']}")
            return False
        if any(output.exists() for output in outputs):
            print(f"{mutation['file']} published a partial output file")
            return False

    # A later output that cannot be staged (missing parent) or published (a directory is in
    # the way) leaves the directory as it was: earlier targets keep their previous contents,
    # files named like staging files are someone else's and stay, and nothing is left behind.
    for label, events_name in (
        ("unstageable", pathlib.Path("missing-parent") / "events.csv"),
        ("unpublishable", pathlib.Path("events-dir")),
    ):
        failed_root = root / f"failed-{label}"
        failed_root.mkdir()
        (failed_root / "events-dir").mkdir()
        (failed_root / "hashes.csv").write_bytes(b"previous hashes\n")
        (failed_root / "snapshots.csv").write_bytes(b"previous snapshots\n")
        for index in range(3):
            (failed_root / f"hashes.csv.sim_headless-{index}.tmp").write_bytes(b"unrelated\n")
        before = tree(failed_root)
        completed = run(
            program,
            "--replay", str(replay),
            "--hash-out", str(failed_root / "hashes.csv"),
            "--events-out", str(failed_root / events_name),
            "--snapshot-out", str(failed_root / "snapshots.csv"),
        )
        if completed.returncode != 4 or "EAWR-SIM-CLI-0001" not in completed.stdout:
            print(completed.stdout, end="")
            print(f"{label} event output did not fail with EAWR-SIM-CLI-0001 and exit 4")
            return False
        after = tree(failed_root)
        if after != before:
            changed = sorted(name for name in before.keys() | after.keys() if before.get(name) != after.get(name))
            print(f"{label} event output changed or left files: {changed}")
            return False

    # A successful run replaces existing targets and leaves only them beside unrelated files.
    replaced_root = root / "replaced"
    replaced_root.mkdir()
    for kind in goldens:
        (replaced_root / f"{kind}.csv").write_bytes(b"previous\n")
    (replaced_root / "hashes.csv.sim_headless-0.tmp").write_bytes(b"unrelated\n")
    completed = run(
        program,
        "--replay", str(replay),
        "--hash-out", str(replaced_root / "hashes.csv"),
        "--events-out", str(replaced_root / "events.csv"),
        "--snapshot-out", str(replaced_root / "snapshots.csv"),
    )
    expected_tree = {f"{kind}.csv": golden for kind, golden in goldens.items()}
    expected_tree["hashes.csv.sim_headless-0.tmp"] = b"unrelated\n"
    if completed.returncode != 0 or tree(replaced_root) != expected_tree:
        print(completed.stdout, end="")
        print("replay-v2 over existing outputs did not leave exactly the new outputs behind")
        return False

    alias_error = check_tactical_aliases(program, replay, root)
    if alias_error:
        print(alias_error)
        return False

    v1_events = root / "v1-events.csv"
    completed = run(
        program,
        "--replay", str(fixtures / "original-v1.eawr-replay"),
        "--hash-out", str(root / "v1-with-events.csv"),
        "--events-out", str(v1_events),
    )
    if completed.returncode == 0 or "EAWR-CORE-0001" not in completed.stdout or v1_events.exists():
        print("--events-out with a replay-v1 input did not fail as an argument error")
        return False
    return True


def audit_trace(audit: dict) -> str:
    """The docs/traces.md trace of original-v1, built from the frozen audit, not the program."""
    states = [(0, audit["initial_entities"])] + [
        (tick["completed_tick"], tick["entities"]) for tick in audit["ticks"]
    ]
    ids = {entity["entity_id"] for entity in audit["initial_entities"]}
    ids |= {command["entity"]["entity_id"] for command in audit["commands"] if command["opcode"] == 1}
    labels = sorted((f"entity.{entity_id}", entity_id) for entity_id in ids)
    lines = ["tick,object,field,value"]
    for tick, entities in states:
        live = {entity["entity_id"]: entity["position_raw"] for entity in entities}
        for label, entity_id in labels:
            lines.append(f"{tick},{label},alive,{int(entity_id in live)}")
            if entity_id in live:
                lines.extend(
                    f"{tick},{label},pos.{axis},{value}"
                    for axis, value in zip("xyz", live[entity_id])
                )
    return "\n".join(lines) + "\n"


def check_output_aliases(program: str, replay: pathlib.Path, root: pathlib.Path) -> str:
    """The replay, the hash CSV, the trace CSV and its .json header must be four files."""
    aliases = root / "aliases"
    aliases.mkdir()
    replay_bytes = replay.read_bytes()
    copy = aliases / "input.json"
    copy.write_bytes(replay_bytes)
    dotted = f"{aliases}{os.sep}.{os.sep}dotted.csv"
    cases = {
        "hash output is the trace header": (
            ["--replay", str(replay), "--hash-out", str(aliases / "same.json")],
            ["--trace-out", str(aliases / "same.csv")],
        ),
        "trace header overwrites the replay": (
            ["--replay", str(copy), "--hash-out", str(aliases / "hashes.csv")],
            ["--trace-out", str(aliases / "input.csv")],
        ),
        "hash output overwrites the replay": (
            ["--replay", str(copy), "--hash-out", str(copy)],
            [],
        ),
        "same trace and hash file through ./": (
            ["--replay", str(replay), "--hash-out", dotted],
            ["--trace-out", str(aliases / "dotted.csv")],
        ),
        "trace header differs from the hash output only in case": (
            ["--replay", str(replay), "--hash-out", str(aliases / "Cased.json")],
            ["--trace-out", str(aliases / "cased.csv")],
        ),
    }
    for name, (inputs, trace) in cases.items():
        rejected = run(program, *inputs, *trace)
        if rejected.returncode != 2 or "EAWR-CORE-0001" not in rejected.stdout:
            return f"{name}: not rejected as an argument error ({rejected.returncode}) {rejected.stdout}"
        if copy.read_bytes() != replay_bytes:
            return f"{name}: the replay input was overwritten"
        written = sorted(path.name for path in aliases.iterdir() if path != copy)
        if written:
            return f"{name}: wrote {written} before rejecting the paths"
    return ""


def check_trace(program: str, fixtures: pathlib.Path, root: pathlib.Path) -> str:
    """Return an error message, or an empty string when --trace-out keeps its contract."""
    audit = json.loads((fixtures / "original-v1.audit.json").read_text(encoding="utf-8"))
    expected_trace = audit_trace(audit)
    replay = fixtures / "original-v1.eawr-replay"
    expected_header = {
        "format": "eawr-trace",
        "format_version": 1,
        "source": "remake",
        "content_identity": audit["header"]["content_identity"],
        "tick_seconds": {"numerator": 1, "denominator": 30},
        "build_identity": {
            "kind": "executable-sha256",
            "value": hashlib.sha256(pathlib.Path(program).read_bytes()).hexdigest(),
        },
        "scenario_sha256": None,
        "replay_sha256": hashlib.sha256(replay.read_bytes()).hexdigest(),
    }
    for workers in (1, 2, 4):
        hashes = root / f"traced-{workers}.csv"
        trace = root / f"trace-{workers}.csv"
        completed = run(
            program,
            "--replay",
            str(replay),
            "--hash-out",
            str(hashes),
            "--trace-out",
            str(trace),
            "--workers",
            str(workers),
        )
        if completed.returncode != 0:
            return f"worker {workers} traced replay failed: {completed.stdout}"
        if hashes.read_text(encoding="utf-8") != EXPECTED:
            return f"worker {workers}: --trace-out changed the replay hashes"
        if trace.read_bytes() != expected_trace.encode("utf-8"):
            return f"worker {workers}: trace differs from the audit-derived trace"
        header = json.loads(trace.with_suffix(".json").read_bytes().decode("utf-8"))
        if header != expected_header:
            return f"worker {workers}: trace header {header} != {expected_header}"

    for trace_name in ("trace.txt", "traced-1.csv"):
        rejected = run(
            program,
            "--replay",
            str(replay),
            "--hash-out",
            str(root / "traced-1.csv"),
            "--trace-out",
            str(root / trace_name),
        )
        if rejected.returncode == 0 or "EAWR-CORE-0001" not in rejected.stdout:
            return f"--trace-out {trace_name} was not rejected as an argument error"

    alias_error = check_output_aliases(program, replay, root)
    if alias_error:
        return alias_error

    # The trace and its header are published with the hashes: a header that cannot be
    # published leaves the earlier outputs with their previous contents.
    held = root / "trace-held"
    held.mkdir()
    (held / "hashes.csv").write_bytes(b"previous hashes\n")
    (held / "trace.csv").write_bytes(b"previous trace\n")
    (held / "trace.json").mkdir()
    before = tree(held)
    completed = run(
        program,
        "--replay",
        str(replay),
        "--hash-out",
        str(held / "hashes.csv"),
        "--trace-out",
        str(held / "trace.csv"),
    )
    if completed.returncode != 4 or "EAWR-SIM-CLI-0001" not in completed.stdout:
        return f"an unpublishable trace header did not fail with exit 4: {completed.stdout}"
    if tree(held) != before:
        return "an unpublishable trace header changed the earlier outputs or left files behind"

    malformed = root / "malformed-trace.csv"
    completed = run(
        program,
        "--replay",
        str(fixtures / "mutated-overflow.eawr-replay"),
        "--hash-out",
        str(root / "malformed-hash.csv"),
        "--trace-out",
        str(malformed),
    )
    if completed.returncode == 0 or malformed.exists() or malformed.with_suffix(".json").exists():
        return "a failed replay published a partial trace"
    return ""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", required=True)
    parser.add_argument("--fixtures", required=True)
    args = parser.parse_args()
    fixtures = pathlib.Path(args.fixtures)

    for name, digest in V1_FIXTURE_SHA256.items():
        if hashlib.sha256((fixtures / name).read_bytes()).hexdigest() != digest:
            print(f"replay-v1 fixture {name} is no longer byte-for-byte frozen")
            return 1

    with tempfile.TemporaryDirectory() as temporary:
        root = pathlib.Path(temporary)
        outputs: list[str] = []
        for workers in ("1", "2", "3", "4", "8", "hardware"):
            output = root / f"worker-{workers}.csv"
            completed = run(
                args.program,
                "--replay",
                str(fixtures / "original-v1.eawr-replay"),
                "--hash-out",
                str(output),
                "--workers",
                str(workers),
            )
            if completed.returncode != 0:
                print(completed.stdout, end="")
                print(f"worker {workers} replay failed with {completed.returncode}")
                return 1
            text = output.read_text(encoding="utf-8")
            if text != EXPECTED:
                print(f"worker {workers} output differs from the independent fixture oracle")
                return 1
            outputs.append(text)
        if len(set(outputs)) != 1:
            print("worker outputs diverged")
            return 1

        zero_output = root / "zero.csv"
        zero = run(
            args.program,
            "--replay",
            str(fixtures / "zero-tick-v1.eawr-replay"),
            "--hash-out",
            str(zero_output),
            "--workers",
            "4",
        )
        if zero.returncode != 0 or zero_output.read_bytes() != b"tick,sha256\n":
            print("zero-tick replay did not produce the specified header-only output")
            return 1

        for malformed in (
            "mutated-length-zero.eawr-replay",
            "mutated-version.eawr-replay",
            "mutated-order.eawr-replay",
            "mutated-count.eawr-replay",
            "mutated-trailing.eawr-replay",
            "mutated-overflow.eawr-replay",
        ):
            output = root / f"{malformed}.csv"
            completed = run(
                args.program,
                "--replay",
                str(fixtures / malformed),
                "--hash-out",
                str(output),
                "--workers",
                "4",
            )
            if completed.returncode == 0 or "EAWR-SIM-" not in completed.stdout:
                print(f"{malformed} was not rejected with a simulation diagnostic")
                return 1
            if output.exists():
                print(f"{malformed} published a partial hash file")
                return 1

        for invalid in ("0", "257", "-1", "four"):
            invalid_workers = run(
                args.program,
                "--replay",
                str(fixtures / "original-v1.eawr-replay"),
                "--hash-out",
                str(root / "invalid.csv"),
                "--workers",
                invalid,
            )
            if invalid_workers.returncode == 0 or "EAWR-CORE-0001" not in invalid_workers.stdout:
                print(f"invalid worker count {invalid} did not fail as a CLI argument error")
                return 1

        missing_output = run(
            args.program, "--replay", str(fixtures / "original-v1.eawr-replay")
        )
        if missing_output.returncode == 0:
            print("missing --hash-out unexpectedly succeeded")
            return 1

        if not check_tactical(args.program, fixtures, root):
            return 1

        trace_error = check_trace(args.program, fixtures, root)
        if trace_error:
            print(trace_error)
            return 1

        scenario_error = check_scenario_arguments(args.program, fixtures / "original-v1.eawr-replay", root)
        if scenario_error:
            print(scenario_error)
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
