#!/usr/bin/env python3
"""Add the release evidence to a finished release manifest.

  python tools/release/release_evidence.py add <name>.zip.manifest.json \\
      --replay76 <replay76 run>/receipt.json \\
      --fresh-user-player <player run>/fresh-user-verdict.json --fresh-user-debug <debug run>/fresh-user-verdict.json

A release that ships no Debug archive says so with `--no-debug-archive` instead of `--fresh-user-debug`; the manifest then
records `fresh_user_debug: {"shipped": false}` and nothing else about Debug.

Each receipt is checked for what it claims, then only its SHA-256 goes into the manifest (the files stay where the runs wrote
them): the replay76 receipt must credit the full 19-fixture x 1/2/4/8 run (`remote_replays.py --check-full`), and each
fresh-user verdict must pass, belong to its kind and name the archive it ran on. The player verdict must name the delivered
archive of this manifest. The sidecar next to the delivered zip is rewritten, and so is its line in the `.zip.sha256` file.
Then `python tools/release/kit_manifest.py verify <sidecar> --zip <zip> --folder <folder> --repo . --release` decides.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "release"))
import kit_manifest  # noqa: E402

HEX64 = re.compile(r"[0-9a-f]{64}")


class EvidenceError(ValueError):
    pass


def read_json(path: Path) -> dict:
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8-sig"))
    except (OSError, ValueError) as error:
        raise EvidenceError(f"cannot read {path}: {error}") from error
    if not isinstance(data, dict):
        raise EvidenceError(f"{path} is not a JSON object")
    return data


def full_replay76(receipt: dict) -> bool:
    sys.path.insert(0, str(ROOT / "tools" / "offload"))
    import remote_replays
    return bool(remote_replays.credits_full_replay76(receipt))


def replay76_item(path: Path, credits=full_replay76) -> dict:
    receipt = read_json(path)
    if not credits(receipt):
        raise EvidenceError(f"{path} does not credit a full replay76 (19 fixtures x 1/2/4/8 workers, 76 runs, passing)")
    return {"file": Path(path).name, "sha256": kit_manifest.sha256_file(path), "size": Path(path).stat().st_size,
            "credits": "full replay76"}


def fresh_user_item(path: Path, kind: str) -> dict:
    verdict = read_json(path)
    if verdict.get("kind") != kind:
        raise EvidenceError(f"{path} is a {verdict.get('kind')!r} verdict, expected {kind!r}")
    if not str(verdict.get("result", "")).startswith("PASS") or verdict.get("cases_failed") != 0:
        raise EvidenceError(f"{path}: the fresh-user run did not pass ({verdict.get('result')!r}, failed {verdict.get('cases_failed')!r})")
    if verdict.get("receipt_matches_archive") is not True:
        raise EvidenceError(f"{path}: the fresh-user receipt does not match the archive it ran on")
    if not HEX64.fullmatch(str(verdict.get("archive_sha256", ""))):
        raise EvidenceError(f"{path} names no archive SHA-256")
    return {"file": Path(path).name, "sha256": kit_manifest.sha256_file(path), "size": Path(path).stat().st_size,
            "archive_sha256": verdict["archive_sha256"], "result": verdict["result"], "kind": kind}


def add(sidecar: Path, replay76: Path, player: Path, debug: Path | None, credits=full_replay76) -> dict:
    """`debug` is the Debug fresh-user verdict, or None for a release that ships no Debug archive."""
    manifest = kit_manifest.read(sidecar)
    if manifest.get("mode") != "release" or "finish" not in manifest:
        raise EvidenceError("evidence is added to a finished release manifest")
    if "evidence" in manifest:
        raise EvidenceError("this manifest already has its evidence")
    items = {"replay76": replay76_item(replay76, credits),
             "fresh_user_player": fresh_user_item(player, "player"),
             "fresh_user_debug": fresh_user_item(debug, "debug") if debug is not None else {"shipped": False}}
    if items["fresh_user_player"]["archive_sha256"] != manifest["finish"]["delivered"]["sha256"]:
        raise EvidenceError("the player verdict names another archive than the one this manifest delivered")
    kit_manifest.add_evidence(manifest, items)
    kit_manifest.write(sidecar, manifest)
    sums = sidecar.with_name(sidecar.name[: -len(".manifest.json")] + ".sha256")  # <name>.zip.sha256
    if sums.is_file():
        lines = []
        for line in sums.read_text(encoding="utf-8").splitlines():
            value, _, name = line.partition("  ")
            lines.append(f"{kit_manifest.sha256_file(sidecar)}  {name}" if name == sidecar.name else line)
        sums.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return manifest


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    one = sub.add_parser("add")
    one.add_argument("sidecar", type=Path)
    one.add_argument("--replay76", type=Path, required=True)
    one.add_argument("--fresh-user-player", type=Path, required=True)
    debug = one.add_mutually_exclusive_group(required=True)
    debug.add_argument("--fresh-user-debug", type=Path)
    debug.add_argument("--no-debug-archive", action="store_true", help="this release ships no Debug archive")
    args = parser.parse_args(argv)
    try:
        manifest = add(args.sidecar, args.replay76, args.fresh_user_player, None if args.no_debug_archive else args.fresh_user_debug)
    except (EvidenceError, kit_manifest.ManifestError, OSError) as error:
        print(f"release-evidence: {error}", file=sys.stderr)
        return 1
    print(f"evidence added: {', '.join(manifest['evidence']['items'])}; evidence_sha256 {manifest['evidence_sha256'][:12]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
