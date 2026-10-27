#!/usr/bin/env python3
"""The sim_headless galactic runs: --galactic <campaign.json> and --galactic-replay <file>.

Checks the hash CSV header and rows, that a recorded campaign replays to the same hashes on 1, 2, 4
and 8 workers, the census, nonzero exits on bad input and that a failed run writes no output.
Run by CTest with the program and the fixtures directory (not a pytest file).
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import sys
import tempfile

ROW = re.compile(r"^(\d+),([0-9a-f]{64})$")
failures: list[str] = []


def expect(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)
        print(f"FAIL: {message}", file=sys.stderr)


def run(program: str, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([program, *arguments], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def files(root: pathlib.Path) -> dict[str, bytes]:
    return {path.relative_to(root).as_posix(): path.read_bytes() for path in root.rglob("*") if path.is_file()}


def check_hash_csv(text: str, ticks: int, label: str) -> None:
    lines = text.splitlines()
    expect(lines[:1] == ["tick,sha256"], f"{label}: the CSV header is tick,sha256")
    rows = [ROW.match(line) for line in lines[1:]]
    expect(len(rows) == ticks and all(rows), f"{label}: {ticks} rows of tick and lowercase SHA-256")
    expect([int(row.group(1)) for row in rows if row] == list(range(1, ticks + 1)), f"{label}: ticks 1..{ticks} in order")
    digests = [row.group(2) for row in rows if row]
    expect(len(set(digests)) == len(digests), f"{label}: every tick hashes differently (the clock is state)")


def campaign_run(program: str, fixture: pathlib.Path, work: pathlib.Path, ticks: int, census: dict[str, object]) -> None:
    name = fixture.stem
    hashes, replay, summary = work / f"{name}.csv", work / f"{name}.eawr-gcreplay", work / f"{name}.json"
    done = run(program, "--galactic", str(fixture), "--ticks", str(ticks), "--hash-out", str(hashes),
               "--replay-out", str(replay), "--census-out", str(summary), "--workers", "2")
    expect(done.returncode == 0, f"{name}: the campaign runs ({done.stdout.strip()})")
    if done.returncode != 0:
        return
    check_hash_csv(hashes.read_text(), ticks, name)
    written = json.loads(summary.read_text())
    for key, value in census.items():
        expect(written.get(key) == value, f"{name}: census {key} = {value!r} (got {written.get(key)!r})")
    expect(written["ticks"] == ticks and re.fullmatch(r"[0-9a-f]{64}", written["campaign_identity"]) is not None,
           f"{name}: census ticks and campaign identity")
    expect(written["final_sha256"] == hashes.read_text().splitlines()[-1].split(",")[1], f"{name}: the census ends on the last hash")
    for workers in ("1", "2", "4", "8"):
        again = work / f"{name}-replayed-{workers}.csv"
        done = run(program, "--galactic-replay", str(replay), "--hash-out", str(again), "--workers", workers)
        expect(done.returncode == 0 and again.read_bytes() == hashes.read_bytes(),
               f"{name}: the recorded replay reproduces every hash on {workers} workers")
    same = work / f"{name}-again.csv"
    run(program, "--galactic", str(fixture), "--ticks", str(ticks), "--hash-out", str(same))
    expect(same.read_bytes() == hashes.read_bytes(), f"{name}: a campaign start is deterministic")


def failures_write_nothing(program: str, fixtures: pathlib.Path, work: pathlib.Path) -> None:
    bad = work / "bad"
    bad.mkdir()
    replay = work / "synthetic-small.eawr-gcreplay"
    truncated = bad / "truncated.eawr-gcreplay"
    truncated.write_bytes(replay.read_bytes()[:-5])
    unknown_key = bad / "unknown.json"
    unknown_key.write_text('{"name": "x", "planets": 4, "colour": 3}')
    not_json = bad / "garbage.json"
    not_json.write_text("{ planets")
    no_name = bad / "noname.json"
    no_name.write_text('{"planets": 4}')
    duplicate = bad / "duplicate.json"
    duplicate.write_text('{"name": "one", "name": "two"}')
    excessive = bad / "excessive.json"
    excessive.write_text(json.dumps({"name": "excessive", "players": 64,
                                    "fleets_per_player": 1000, "members_per_fleet": 1000}))
    out = bad / "out.csv"
    cases = [
        (3, ("--galactic-replay", str(truncated), "--hash-out", str(out)), "a truncated replay"),
        (3, ("--galactic-replay", str(bad / "missing.eawr-gcreplay"), "--hash-out", str(out)), "a missing replay"),
        (3, ("--galactic", str(unknown_key), "--hash-out", str(out)), "an unknown campaign key"),
        (3, ("--galactic", str(not_json), "--hash-out", str(out)), "a campaign that is not JSON"),
        (3, ("--galactic", str(no_name), "--hash-out", str(out)), "a campaign without a name"),
        (3, ("--galactic", str(duplicate), "--hash-out", str(out)), "a duplicate campaign key"),
        (3, ("--galactic", str(excessive), "--hash-out", str(out)), "a campaign over the object quota"),
        (2, ("--galactic", "Campaign_Equal_Footing", "--hash-out", str(out)), "a stock campaign before the setup work"),
        (2, ("--galactic-replay", str(replay)), "a replay run without --hash-out"),
        (2, ("--galactic-replay", str(replay), "--hash-out", str(out), "--ticks", "5"), "--ticks with a replay"),
        (2, ("--galactic", str(fixtures / "synthetic-small.json")), "a campaign run without an output"),
        (2, ("--galactic", str(fixtures / "synthetic-small.json"), "--galactic-replay", str(replay), "--hash-out", str(out)),
         "both modes at once"),
        (2, ("--galactic-replay", str(replay), "--hash-out", str(replay)), "the input as the output"),
        (2, ("--galactic", str(fixtures / "synthetic-small.json"), "--hash-out", str(out), "--replay-out", str(out)),
         "two outputs to one file"),
        (2, ("--galactic", str(fixtures / "synthetic-small.json"), "--census-out", str(bad / "census.txt")), "a census that is not .json"),
        (2, ("--galactic", str(fixtures / "synthetic-small.json"), "--hash-out", str(out), "--workers", "0"), "zero workers"),
        (2, ("--galactic", str(fixtures / "synthetic-small.json"), "--hash-out", str(out), "--hash-out", str(out)),
         "a repeated option"),
        (2, ("--galactic", str(fixtures / "synthetic-small.json"), "--hash-out", str(out), "--live-perception", "Example"),
         "--live-perception on a synthetic campaign"),
        (2, ("--galactic", "Campaign_Equal_Footing", "--game-root", str(bad), "--live", "60", "--live-perception", "Example"),
         "--live-perception without --live-ai on"),
        # --live-goals reads the native AI of a live run: a synthetic campaign, a run without --live-ai on and a zero interval are refused.
        (2, ("--galactic", str(fixtures / "synthetic-small.json"), "--hash-out", str(out), "--live-goals", "600"),
         "--live-goals on a synthetic campaign"),
        (2, ("--galactic", "Campaign_Equal_Footing", "--game-root", str(bad), "--live", "60", "--live-goals", "600"),
         "--live-goals without --live-ai on"),
        (2, ("--galactic", "Campaign_Equal_Footing", "--game-root", str(bad), "--live", "60", "--live-ai", "on", "--live-goals", "0"),
         "--live-goals 0"),
    ]
    for code, arguments, label in cases:
        before = files(bad)
        done = run(program, *arguments)
        expect(done.returncode == code, f"{label}: exit {code} (got {done.returncode}: {done.stdout.strip()})")
        expect(files(bad) == before, f"{label}: no output written")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--program", required=True)
    parser.add_argument("--fixtures", required=True, type=pathlib.Path)
    arguments = parser.parse_args()
    with tempfile.TemporaryDirectory() as directory:
        work = pathlib.Path(directory)
        campaign_run(arguments.program, arguments.fixtures / "synthetic-small.json", work, 1400,
                     {"campaign": "synthetic-small", "seed": 7, "players": 2, "seats": 2, "planets": 12, "routes": 15,
                      "fleets": 4, "fleet_members": 12, "fiscal_cycle_frames": 1350})
        campaign_run(arguments.program, arguments.fixtures / "synthetic-coop.json", work, 200,
                     {"campaign": "synthetic-coop", "seed": 11, "players": 3, "seats": 4, "planets": 40, "routes": 50,
                      "fleets": 9, "fleet_members": 36, "fiscal_cycle_frames": 75})
        escaped = work / "escaped.json"
        name = 'quoted "campaign" \\ path\nnext\tline'
        escaped.write_text(json.dumps({"name": name}))
        summary = work / "escaped-census.json"
        done = run(arguments.program, "--galactic", str(escaped), "--census-out", str(summary))
        expect(done.returncode == 0 and json.loads(summary.read_text())["campaign"] == name,
               "census JSON preserves quotes, backslashes and control characters in campaign names")
        failures_write_nothing(arguments.program, arguments.fixtures, work)
    if failures:
        print(f"{len(failures)} galactic CLI check(s) failed", file=sys.stderr)
        return 1
    print("galactic CLI contracts passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
