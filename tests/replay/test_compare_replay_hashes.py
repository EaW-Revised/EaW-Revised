#!/usr/bin/env python3
"""Contract tests for exact replay evidence discovery and mismatch reporting."""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile


def invoke(
    tool: pathlib.Path, root: pathlib.Path, audit: pathlib.Path, targets: tuple[str, ...], tactical: pathlib.Path | None = None
) -> subprocess.CompletedProcess[str]:
    command = [
        sys.executable,
        str(tool),
        "--evidence-root",
        str(root),
        "--audit",
        str(audit),
    ]
    if tactical is not None:
        command.extend(("--tactical-fixtures", str(tactical)))
    for target in targets:
        command.extend(("--target", target))
    return subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def write_lf_text(path: pathlib.Path, value: str) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as output:
        output.write(value)


def write_target(root: pathlib.Path, target: str, hashes: tuple[str, ...]) -> None:
    directory = root / f"replay-evidence-{target}"
    directory.mkdir(parents=True, exist_ok=True)
    write_lf_text(directory / "target.txt", target + "\n")
    for workers in (1, 2, 4):
        rows = "tick,sha256\n" + "".join(
            f"{tick},{digest}\n" for tick, digest in enumerate(hashes, 1)
        )
        write_lf_text(directory / f"workers-{workers}.csv", rows)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=pathlib.Path, required=True)
    args = parser.parse_args()
    targets = ("alpha", "beta")
    hashes = ("a" * 64, "b" * 64)
    with tempfile.TemporaryDirectory() as temporary:
        root = pathlib.Path(temporary)
        audit = root / "audit.json"
        audit.write_text(
            json.dumps(
                {
                    "ticks": [
                        {"completed_tick": tick, "state_sha256": digest}
                        for tick, digest in enumerate(hashes, 1)
                    ]
                }
            ),
            encoding="utf-8",
        )
        for target in targets:
            write_target(root, target, hashes)
        valid = invoke(args.tool, root, audit, targets)
        if valid.returncode != 0:
            print(valid.stdout, end="")
            return 1

        missing = root / "replay-evidence-beta" / "workers-4.csv"
        missing.unlink()
        result = invoke(args.tool, root, audit, targets)
        if result.returncode == 0 or "missing required result" not in result.stdout:
            print("missing named worker result was not rejected")
            return 1
        write_target(root, "beta", hashes)

        divergent = root / "replay-evidence-beta" / "workers-2.csv"
        write_lf_text(
            divergent,
            "tick,sha256\n1," + "a" * 64 + "\n2," + "c" * 64 + "\n",
        )
        result = invoke(args.tool, root, audit, targets)
        if result.returncode == 0 or "first divergent tick 2" not in result.stdout:
            print("first divergent tick was not reported")
            return 1

        write_target(root, "beta", hashes)
        identity = root / "replay-evidence-beta" / "target.txt"
        write_lf_text(identity, "alpha\n")
        result = invoke(args.tool, root, audit, targets)
        if result.returncode == 0 or "target identity mismatch" not in result.stdout:
            print("copied/misnamed target evidence was not rejected")
            return 1
        write_target(root, "beta", hashes)
        fixtures = pathlib.Path(__file__).parent / "fixtures"
        state = "506e48ffa375d8a6fd29c9fa69474f63d4f455a999126e5f232bef3aefd64a2a"
        snapshot = "d" * 64
        census = json.dumps({"tick_zero": {"state_sha256": state, "snapshot_sha256": snapshot}}) + "\n"
        for target in targets:
            directory = root / f"replay-evidence-{target}"
            for workers in (1, 2, 4):
                for kind in ("hashes", "events", "snapshots"):
                    (directory / f"tactical-v2-workers-{workers}.{kind}.csv").write_bytes(
                        (fixtures / f"tactical-v2.{kind}.csv").read_bytes().replace(b"\r\n", b"\n"))
                write_lf_text(directory / f"skirmish-workers-{workers}.hashes.csv", "tick,sha256\n1," + state + "\n")
                write_lf_text(directory / f"skirmish-workers-{workers}.snapshots.csv", "tick,sha256\n0," + snapshot + "\n")
                write_lf_text(directory / f"skirmish-workers-{workers}.census.json", census)
        # Copy goldens to an LF-only directory (Windows checkout may use CRLF).
        golden = root / "golden"
        golden.mkdir()
        for kind in ("hashes", "events", "snapshots"):
            (golden / f"tactical-v2.{kind}.csv").write_bytes(
                (fixtures / f"tactical-v2.{kind}.csv").read_bytes().replace(b"\r\n", b"\n"))
        result = invoke(args.tool, root, audit, targets, golden)
        if result.returncode:
            print(result.stdout)
            return 1
        directory = root / "replay-evidence-beta"
        for name in ("tactical-v2-workers-2.hashes.csv", "tactical-v2-workers-4.events.csv",
                     "tactical-v2-workers-1.snapshots.csv", "skirmish-workers-4.census.json",
                     "skirmish-workers-2.snapshots.csv", "skirmish-workers-1.hashes.csv"):
            path = directory / name
            original = path.read_bytes()
            path.unlink()
            result = invoke(args.tool, root, audit, targets, golden)
            if result.returncode == 0 or "missing required result" not in result.stdout:
                print(f"missing {name} accepted: {result.stdout}")
                return 1
            path.write_bytes(original + b"corrupt\n")
            result = invoke(args.tool, root, audit, targets, golden)
            if result.returncode == 0:
                print(f"mutated {name} accepted")
                return 1
            path.write_bytes(original)
        # Identical-but-wrong tick zero across every lane must not pass by consensus.
        for target in targets:
            for workers in (1, 2, 4):
                write_lf_text(root / f"replay-evidence-{target}" / f"skirmish-workers-{workers}.census.json",
                              census.replace(state, "f" * 64))
        result = invoke(args.tool, root, audit, targets, golden)
        if result.returncode == 0 or "tick-zero state differs" not in result.stdout:
            print(f"wrong tick zero accepted: {result.stdout}")
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
