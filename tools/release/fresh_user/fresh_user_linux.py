#!/usr/bin/env python3
"""Fresh-user gate for the Linux x64 player zip (runs ON the clean Ubuntu 24.04 box, as its plain user).

  python3 fresh_user_linux.py --package eaw-revised-<v>-linux-x64-player.zip --out <dir> --game-root <FoC folder>
                              --expected-sha256 <zip sha256>   (REQUIRED)

The box is a stock Ubuntu 24.04 with no game-side packages and no sudo for the user (owner 11-21: Ubuntu 24.04 only,
software rendering is the final basis), so a library the player needs but does not ship shows up as a FAILED case with the
library name, instead of being installed away. What it does:
  1. checks the zip's own unix modes (play.sh and the engine must be 0755 in the archive) and extracts it with those modes
     (stock `unzip` semantics, no chmod afterwards);
  2. runs container_checks.sh as the plain user on a READ-ONLY package folder with an empty HOME: sums, exec bits,
     runtime.json engine hash, ldd (no missing libraries), glibc floor, `--version`, the gate tooling, `play.sh --check`,
     the FoC-missing message (exit 3), `play.sh` reaching the main menu and quitting by itself, the Online route reported at
     runtime, logs written into the empty HOME, a 900-tick skirmish vs the AI (about 2 minutes under software rendering),
     no writes outside the user dir (HOME and /tmp) and the package folder unchanged;
     --game-root is REQUIRED (a FoC folder on the box, only read): there is no reduced mode that could pass without the game;
     the box needs xvfb and mesa-vulkan-drivers as the gate's own tooling (not player dependencies; see docs/releasing.md);
  3. writes `fresh-user-verdict.json` in the shape tools/release/release_evidence.py accepts (`--fresh-user-linux`): kind
     player, platform linux-x64, cases_passed/failed, receipt_matches_archive, archive_sha256, result "PASS n/n".

`play.sh` contract used here (the packaging PR defines the real script; override with FU_PLAY_ARGS / FU_SKIRMISH_ARGS):
  `play.sh [--game-root <dir>] [--check] [-- <extra game arguments>]` (it always starts the front end; the skirmish runs through the bundled engine), engine at godot/godot.linuxbsd.editor.x86_64.
From a workstation use fresh_user_linux_remote.py, which copies the zip to the box and runs this there.
"""
from __future__ import annotations

import argparse
import os
import hashlib
import json
import re
import shutil
import stat
import subprocess
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools" / "common"))
try:  # on the Linux box only this file and container_checks.sh are copied; there is no console window to hide there
    import hidden_console  # noqa: E402

    hidden_console.install()
except ImportError:
    pass

ENGINE = "godot/godot.linuxbsd.editor.x86_64"
PLAYER_SCRIPT = "play.sh"
CASE_PREFIX = "FU-CASE"
HEX64 = re.compile(r"[0-9a-f]{64}")
# The cases container_checks.sh reports; the verdict passes only when every one of them is present and PASS.
EXPECTED_CASES = ("sums", "exec_bits", "runtime_engine_hash", "ldd_complete", "glibc_floor", "engine_version",
                  "gate_tooling", "play_check", "foc_missing", "start_reaches_screen", "clean_quit", "online_lib_runtime",
                  "logs_written", "skirmish_runs", "user_dir_only", "package_unchanged")

class GateError(RuntimeError):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_cases(text: str) -> list[dict]:
    """`FU-CASE <name> <PASS|FAIL|SKIP> <detail...>` lines; anything else is console noise. Last line per name wins."""
    found: dict[str, dict] = {}
    for line in text.splitlines():
        parts = line.strip().split(None, 3)
        if len(parts) >= 3 and parts[0] == CASE_PREFIX and parts[2] in ("PASS", "FAIL", "SKIP"):
            found[parts[1]] = {"name": parts[1], "status": parts[2], "detail": parts[3] if len(parts) > 3 else ""}
    return list(found.values())


def zip_mode_problems(archive: Path, required=(PLAYER_SCRIPT, ENGINE)) -> list[str]:
    """The zip must carry unix exec bits for the files a user runs; a zip written on Windows without them fails here."""
    problems = []
    with zipfile.ZipFile(archive) as zf:
        modes = {}
        for info in zf.infolist():
            if info.is_dir():
                continue
            parts = info.filename.split("/", 1)
            modes[parts[1] if len(parts) == 2 else parts[0]] = info.external_attr >> 16
        for name in required:
            mode = modes.get(name)
            if mode is None:
                problems.append(f"{name} is not in the archive")
            elif not mode & 0o111:
                problems.append(f"{name} has no exec bit in the archive (mode {oct(mode & 0o7777)})")
    return problems


def extract_with_modes(archive: Path, target: Path) -> Path:
    """Extract like `unzip` does: keep the unix mode, refuse paths that leave the folder. Returns the single top folder."""
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    root = target.resolve()
    with zipfile.ZipFile(archive) as zf:
        for info in zf.infolist():
            destination = (target / info.filename).resolve()
            if not destination.is_relative_to(root):
                raise GateError(f"unsafe archive member: {info.filename}")
            if info.is_dir():
                destination.mkdir(parents=True, exist_ok=True)
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            with zf.open(info) as source, destination.open("wb") as out:
                shutil.copyfileobj(source, out)
            mode = (info.external_attr >> 16) & 0o7777
            destination.chmod(mode if mode else 0o644)
    tops = [p for p in target.iterdir()]
    if len(tops) != 1 or not tops[0].is_dir():
        raise GateError("the archive must hold one top folder")
    return tops[0]


def build_verdict(*, archive: Path, archive_sha256: str, cases: list[dict], mode_problems: list[str], image: str,
                  machine: str, receipt_sha256_seen: str | None, game_root_used: bool, container_exit: int | None,
                  expected_sha256: str | None) -> dict:
    by_name = {c["name"]: c for c in cases}
    expected = list(EXPECTED_CASES)
    # The mode check is a case of its own (it runs on the host, before the container).
    results = [{"name": "zip_modes", "status": "FAIL" if mode_problems else "PASS",
                "detail": "; ".join(mode_problems) or "play.sh and the engine carry exec bits in the archive"}]
    for name in expected:
        case = by_name.get(name)
        results.append(case or {"name": name, "status": "FAIL", "detail": "case did not report"})
    passed = sum(1 for c in results if c["status"] == "PASS")
    failed = len(results) - passed - sum(1 for c in results if c["status"] == "SKIP")
    # Three independent values must agree: the hash the caller computed from ITS copy of the zip (expected_sha256), the hash of
    # the archive file on this box re-read when the verdict is built (archive_sha256), and the hash the receipt file holds.
    receipt_ok = (HEX64.fullmatch(archive_sha256 or "") is not None and expected_sha256 is not None
                  and receipt_sha256_seen == archive_sha256 == expected_sha256.lower())
    total = len(results)
    ok = failed == 0 and passed == total and receipt_ok and container_exit == 0
    return {"kind": "player", "platform": "linux-x64", "archive": Path(archive).name, "archive_sha256": archive_sha256,
            "machine": machine, "image": image, "rendering": "software (lavapipe)", "game_root_used": game_root_used,
            "container_exit": container_exit, "cases_passed": passed, "cases_failed": failed,
            "expected_cases": total, "receipt_matches_archive": bool(receipt_ok), "cases": results,
            "result": f"PASS {passed}/{total}" if ok else "FAIL"}


def os_image() -> str:
    """Name the environment the run happened in (shown in the verdict): distro and glibc."""
    name = "linux"
    try:
        for line in Path("/etc/os-release").read_text(encoding="utf-8").splitlines():
            if line.startswith("PRETTY_NAME="):
                name = line.split("=", 1)[1].strip().strip('"')
    except OSError:
        pass
    glibc = subprocess.run(["ldd", "--version"], capture_output=True, text=True).stdout.splitlines()[:1]
    return f"{name}, glibc {glibc[0].split()[-1]}" if glibc else name


def run_gate(args) -> dict:
    archive = Path(args.package).resolve()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    archive_sha = sha256_file(archive)
    if args.expected_sha256.lower() != archive_sha:
        raise GateError(f"archive sha256 {archive_sha} is not the expected {args.expected_sha256}")
    work = out.parent / (out.name + "-extracted")
    package = extract_with_modes(archive, work)
    script = Path(__file__).with_name("container_checks.sh").resolve()
    env = dict(os.environ, FU_WORK=str(work), FU_PACKAGE=package.name, FU_OUT=str(out))
    env["FU_GAME"] = str(Path(args.game_root).resolve())
    if args.play_args:
        env["FU_PLAY_ARGS"] = args.play_args
    print("fresh-user-linux: bash container_checks.sh on " + package.name, flush=True)
    result = subprocess.run(["bash", str(script)], stdin=subprocess.DEVNULL, capture_output=True, text=True, errors="replace",
                            timeout=args.timeout, env=env)
    console = result.stdout + result.stderr
    (out / "checks.log").write_text(console, encoding="utf-8")
    # The receipt names the archive by the hash taken when the run started; the verdict compares it with a fresh hash of the
    # archive file after the run and with the hash the caller computed from its own copy (--expected-sha256, required).
    receipt = out / "fresh-user-receipt.json"
    receipt.write_text(json.dumps({"kind": "player", "platform": "linux-x64", "archive_sha256": archive_sha,
                                   "package_folder": package.name, "image": os_image()}, indent=2) + "\n", encoding="utf-8")
    seen = json.loads(receipt.read_text(encoding="utf-8"))["archive_sha256"]
    verdict = build_verdict(archive=archive, archive_sha256=sha256_file(archive), cases=parse_cases(console),
                            mode_problems=zip_mode_problems(archive), image=os_image(), machine=args.machine,
                            receipt_sha256_seen=seen, game_root_used=True, container_exit=result.returncode,
                            expected_sha256=args.expected_sha256)
    (out / "fresh-user-verdict.json").write_text(json.dumps(verdict, indent=2) + "\n", encoding="utf-8")
    if not args.keep_extracted:  # the extracted tree is not evidence (and is read-only after a run)
        for path in [work, *work.rglob("*")]:
            if path.is_dir():
                path.chmod(0o755)
        shutil.rmtree(work, ignore_errors=True)
    return verdict


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--game-root", required=True, type=Path,
                        help="REQUIRED: a FoC install folder on this box (holds GameData and corruption); read-only")
    parser.add_argument("--expected-sha256", required=True,
                        help="REQUIRED: sha256 of the zip as the caller holds it (the remote runner computes it from the local file)")
    parser.add_argument("--play-args", help="override the play.sh arguments for the start case")
    parser.add_argument("--keep-extracted", action="store_true")
    parser.add_argument("--machine", default="linux-host")
    parser.add_argument("--timeout", type=int, default=2700)
    args = parser.parse_args(argv)
    if not Path(args.game_root).is_dir():
        print(f"fresh-user-linux: --game-root {args.game_root} is not a folder on this box", file=sys.stderr)
        return 2
    try:
        verdict = run_gate(args)
    except (GateError, OSError, zipfile.BadZipFile, subprocess.TimeoutExpired) as error:
        print(f"fresh-user-linux: {error}", file=sys.stderr)
        return 2
    print(f"fresh-user linux-x64: {verdict['result']} (cases {verdict['cases_passed']} passed / {verdict['cases_failed']} failed, "
          f"receipt ok {verdict['receipt_matches_archive']}); {args.out}")
    for case in verdict["cases"]:
        print(f"  {case['status']:4} {case['name']}: {case['detail']}")
    return 0 if verdict["result"].startswith("PASS") else 1


if __name__ == "__main__":
    sys.exit(main())
