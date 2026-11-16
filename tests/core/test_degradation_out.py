#!/usr/bin/env python3
"""sim_headless --degradation-out end to end: the summary is written on success and on failure, names the session and the
build, names the mods of the chain it was given by their mod ids, in chain order (never the mount layer ids), and a bad flag is an argument error."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def run(program: str, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([program, *arguments], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)


def fail(message: str, completed: subprocess.CompletedProcess[str] | None = None) -> int:
    print(message, file=sys.stderr)
    if completed is not None:
        print(completed.stdout, completed.stderr, file=sys.stderr)
    return 1


def check_summary(summary: dict, ended: str, mode: str | None) -> str | None:
    if summary.get("schema_version") != 1:
        return "schema_version is not 1"
    if not summary.get("session_id"):
        return "no session_id"
    if not summary.get("build"):
        return "no build"
    if "commit" not in summary:
        return "no commit field"
    if summary.get("ended") != ended:
        return f"ended is {summary.get('ended')!r}, expected {ended!r}"
    if mode is not None and summary.get("mode") != mode:
        return f"mode is {summary.get('mode')!r}, expected {mode!r}"
    if not summary.get("written_at"):
        return "no written_at"
    if "totals" not in summary or "classes" not in summary:
        return "no totals or classes"
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="eawr-degradation-out-") as temporary:
        root = Path(temporary)

        # Success exit: --help returns 0 and the summary says ok with nothing degraded.
        ok_file = root / "ok" / "summary.json"
        completed = run(args.program, "--help", "--degradation-out", str(ok_file))
        if completed.returncode != 0 or not ok_file.is_file():
            return fail(f"--help with --degradation-out: exit {completed.returncode}, file {ok_file.is_file()}", completed)
        summary = json.loads(ok_file.read_text(encoding="utf-8"))
        problem = check_summary(summary, "ok", None)
        if problem or summary["totals"]["items"] != 0:
            return fail(f"ok summary: {problem or 'items is not 0'}", completed)
        if "EAWR-DEGRADE-0001 0 items degraded in 0 classes" not in completed.stderr:
            return fail("the one log line is missing on a successful run", completed)

        # Failure exit: a missing replay is exit 3 and the summary says failed.
        failed_file = root / "failed.json"
        missing = root / "missing.eawr-replay"
        completed = run(args.program, "--replay", str(missing), "--hash-out", str(root / "h.csv"), "--degradation-out", str(failed_file))
        if completed.returncode != 3 or not failed_file.is_file():
            return fail(f"missing replay: exit {completed.returncode} (expected 3), file {failed_file.is_file()}", completed)
        summary = json.loads(failed_file.read_text(encoding="utf-8"))
        problem = check_summary(summary, "failed", "replay")
        if problem:
            return fail(f"failed summary: {problem}", completed)
        if summary["session_id"] == json.loads(ok_file.read_text(encoding="utf-8"))["session_id"]:
            return fail("two runs share a session id")

        # The chain's mods are named by their mod id, in chain order: a Workshop item folder is `workshop:<item id>`, any
        # other folder its path key. The run fails on the missing replay, but the session already named its mods. (The
        # chain is only parsed and named here, no mod data is read.)
        content = root / "steamapps" / "workshop" / "content" / "32470"
        first, second = content / "111", content / "222"
        first.mkdir(parents=True)
        second.mkdir()
        plain = root / "plain-mod"
        plain.mkdir()
        for name, chain, expected in (("one", str(plain), None),
                                      ("two", f"{first};{second}", ["workshop:111", "workshop:222"]),
                                      ("swapped", f"{second};{first}", ["workshop:222", "workshop:111"]),
                                      # An empty segment is flagged by name, never named after the working folder.
                                      ("empty", f"{first};;{second}", ["workshop:111", "unknown:empty-chain-segment-mod-parent-1",
                                                                       "workshop:222"])):
            mod_file = root / f"mods-{name}.json"
            completed = run(args.program, "--replay", str(missing), "--mod-root", chain, "--degradation-out", str(mod_file))
            if not mod_file.is_file():
                return fail(f"no summary with --mod-root ({name})", completed)
            mods = json.loads(mod_file.read_text(encoding="utf-8")).get("mods")
            if expected is None:
                good = isinstance(mods, list) and len(mods) == 1 and mods[0].startswith("path:")
            else:
                good = mods == expected
            if not good:
                return fail(f"mods for the {name} chain is {mods!r}, expected {expected or 'one path key'}", completed)

        # The run (the campaign of a galactic run, the map of a skirmish run) is in the summary, without a folder or an
        # extension, so the gate can baseline more than one run per mod and mode. The runs fail early on a missing game
        # root, but the session already named its run.
        for name, arguments, expected in (
                ("campaign", ["--galactic", "Some_Campaign", "--game-root", str(root / "none")], "Some_Campaign"),
                ("campaign-path", ["--galactic", str(root / "dir" / "Other.xml"), "--game-root", str(root / "none")], "Other"),
                ("map", ["--skirmish", "m2", "--map", str(root / "maps" / "_mp_x.ted"), "--game-root", str(root / "none")], "_mp_x"),
                ("setup", ["--skirmish", "m2", "--game-root", str(root / "none")], "m2"),
                ("replay", ["--replay", str(missing), "--hash-out", str(root / "h.csv")], "")):
            run_file = root / f"run-{name}.json"
            completed = run(args.program, *arguments, "--degradation-out", str(run_file))
            if not run_file.is_file():
                return fail(f"no summary for the {name} run", completed)
            if json.loads(run_file.read_text(encoding="utf-8")).get("run") != expected:
                return fail(f"run of the {name} run is {json.loads(run_file.read_text(encoding='utf-8')).get('run')!r}, "
                            f"expected {expected!r}", completed)

        # Bad use is an argument error and writes nothing.
        bad = root / "bad.json"
        for arguments in (["--help", "--degradation-out"],
                          ["--help", "--degradation-out", str(bad), "--degradation-out", str(bad)]):
            completed = run(args.program, *arguments)
            if completed.returncode == 0 or bad.exists():
                return fail(f"bad arguments {arguments}: exit {completed.returncode}", completed)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
