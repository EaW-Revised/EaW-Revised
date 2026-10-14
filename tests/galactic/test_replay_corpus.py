#!/usr/bin/env python3
"""Negative contracts: missing evidence and a changed fiscal rule cannot pass the GC gate."""
from __future__ import annotations

import argparse
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools"))
from compare_replay_hashes import compare_galactic_bundle, galactic_cases


def refuses(action, fragment: str) -> None:
    try:
        action()
    except ValueError as error:
        if fragment in str(error):
            return
        raise AssertionError(f"expected {fragment!r}, got {str(error)!r}") from error
    raise AssertionError(f"expected refusal containing {fragment!r}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", required=True)
    parser.add_argument("--corpus", required=True, type=pathlib.Path)
    args = parser.parse_args()
    cases = galactic_cases(args.corpus)
    with tempfile.TemporaryDirectory() as temporary:
        root = pathlib.Path(temporary)
        evidence = root / "replay-evidence-test"
        evidence.mkdir()
        (evidence / "target.txt").write_bytes(b"test\n")
        for name, _, _ in cases:
            for workers in (1, 2, 4, 8):
                shutil.copyfile(args.corpus / f"{name}.hashes.csv", evidence / f"{name}-workers-{workers}.hashes.csv")
        check = lambda: compare_galactic_bundle(root, args.corpus, ("test",))
        check()
        tool = pathlib.Path(__file__).resolve().parents[2] / "tools/compare_replay_hashes.py"
        command = [sys.executable, str(tool), "--evidence-root", str(root),
                   "--galactic-corpus", str(args.corpus), "--target", "test"]
        done = subprocess.run(command, capture_output=True, text=True, timeout=30)
        if done.returncode:
            raise AssertionError(f"galactic compare CLI refused valid evidence: {done.stdout}{done.stderr}")
        missing = evidence / f"{cases[0][0]}-workers-8.hashes.csv"
        original = missing.read_bytes()
        missing.unlink()
        refuses(check, "missing required result")
        done = subprocess.run(command, capture_output=True, text=True, timeout=30)
        if done.returncode != 1 or "missing required result" not in done.stderr:
            raise AssertionError("galactic compare CLI accepted missing worker-8 evidence")
        missing.write_bytes(original)
        lines = original.splitlines(keepends=True)
        lines[2] = b"2," + b"0" * 64 + b"\n"
        missing.write_bytes(b"".join(lines))
        refuses(check, "first divergent tick 2")
        missing.write_bytes(b"".join(original.splitlines(keepends=True)[:-1]))
        refuses(check, "first missing tick")
        missing.write_bytes(original)
        (evidence / "target.txt").write_bytes(b"other\n")
        refuses(check, "target identity mismatch")
        (evidence / "target.txt").write_bytes(b"test\n")

        corpus = root / "changed-corpus"
        shutil.copytree(args.corpus, corpus)
        name, replay, _ = cases[0]
        changed = bytearray(replay.read_bytes())
        # Documented EAWRGCR DTO offsets: skip variable player and seat tables to the fiscal rule.
        players = struct.unpack_from("<I", changed, 96)[0]
        seats_offset = 100 + players * 12
        seats = struct.unpack_from("<I", changed, seats_offset)[0]
        rules_offset = seats_offset + 4 + seats * 18
        struct.pack_into("<I", changed, rules_offset, struct.unpack_from("<I", changed, rules_offset)[0] + 1)
        (corpus / replay.name).write_bytes(changed)
        # The changed replay remains valid, and the actual CLI run must fail against frozen pins.
        gate = pathlib.Path(__file__).with_name("replay_corpus_check.py")
        done = subprocess.run([sys.executable, str(gate), "--program", args.program, "--corpus", str(corpus)],
                              capture_output=True, text=True, timeout=120)
        if done.returncode != 1 or f"{name}/" not in done.stderr or "first divergent tick 1" not in done.stderr:
            raise AssertionError(f"changed fiscal rule did not fail at first tick: {done.stdout}{done.stderr}")
        # A malformed inventory and shortened pins must fail before running any simulation.
        (corpus / "manifest.json").write_bytes(b'{"cases": ["../escape"]}\n')
        refuses(lambda: galactic_cases(corpus), "unique nonempty case names")
        shutil.copyfile(args.corpus / "manifest.json", corpus / "manifest.json")
        (corpus / f"{name}.hashes.csv").write_bytes(b"tick,sha256\n")
        refuses(lambda: galactic_cases(corpus), "pinned row count differs")
    print("galactic replay corpus negative contracts passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
