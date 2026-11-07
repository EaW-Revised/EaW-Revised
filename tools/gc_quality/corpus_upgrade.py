"""Read-only dry run of the GC corpus upgrade for the async-AI format bumps F1..F5 (tools/gc_quality/README.md).

Regenerates the checked-in galactic replay corpus (the 12 .eawr-gcreplay fixtures with their hash CSVs and manifests)
with the repository's own synthetic generators into a staging folder, optionally twice to prove the generators are
deterministic, compares every produced file with the checked-in one and the pin table of tools/publish_scan/config.json,
and lists exactly what moved. It never writes below --root: the real upgrade stays tools/replay/upgrade_galactic_corpus.py
(CMake target galactic_corpus_upgrade), which a format-bump slice runs once its format change is in.

    python tools/gc_quality/corpus_upgrade.py --root . --out out/gcq/corpus-dry-run \\
        --basic <galactic_replay_corpus_generate> --economy <galactic_area_campaign_economy> \\
        --transitions <galactic_area_campaign_transitions> --checkpoint <galactic_area_checkpoint> \\
        --control <galactic_control_tests> [--twice] [--expect clean|moved|any]

Exit code: 0 as expected, 1 when --expect is violated or generation is not deterministic.
"""
from __future__ import annotations

import argparse
import hashlib
import runpy
import json
import shutil
import sys
import tempfile
import types
from dataclasses import dataclass, field
from pathlib import Path

FIXTURES = "tests/galactic/fixtures"
PIN_CONFIG = "tools/publish_scan/config.json"

# What each planned bump is expected to touch (design: ai-async-design.md section 7). Documentation of the dry-run
# output, not an assertion: the dry run reports what moved, whichever bump caused it.
BUMPS = (
    ("F1", "script-bridge state hash domain v1 -> v2", "state hashes in *.hashes.csv of fixtures that run the script bridge"),
    ("F2", "GalacticScriptSave AI part +1", "script save bytes inside checkpoint-bearing replays"),
    ("F3", "galactic_ai checkpoint section 14 version 1 -> 2, native state version", "checkpoint sections in replays that embed checkpoints"),
    ("F4", "lifecycle encoding v2 -> v3", "AI lifecycle state inside native AI checkpoints (none of the 12 fixtures)"),
    ("F5", "galactic replay format 11 -> 12", "version word of every .eawr-gcreplay, hence its pin in the publish-scan table"),
)


@dataclass
class FileDelta:
    path: str
    status: str                   # unchanged | moved | new | not_regenerated
    before: str | None = None     # sha256 of the checked-in file
    after: str | None = None      # sha256 of the regenerated file
    size_before: int | None = None
    size_after: int | None = None
    detail: str = ""


@dataclass
class DryRun:
    deltas: list[FileDelta] = field(default_factory=list)
    pins: list[dict] = field(default_factory=list)
    deterministic: bool | None = None
    nondeterministic_files: list[str] = field(default_factory=list)

    @property
    def moved(self) -> list[FileDelta]:
        return [d for d in self.deltas if d.status in ("moved", "new")]


def sha256_of(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _first_text_difference(before: Path, after: Path) -> str:
    try:
        a = before.read_text(encoding="utf-8").splitlines()
        b = after.read_text(encoding="utf-8").splitlines()
    except UnicodeDecodeError:
        return "binary"
    changed = [i for i, (x, y) in enumerate(zip(a, b)) if x != y]
    extra = abs(len(a) - len(b))
    if not changed and not extra:
        return "line endings only"
    first = changed[0] + 1 if changed else min(len(a), len(b)) + 1
    return f"{len(changed) + extra} differing rows, first at line {first}"


def _first_byte_difference(before: Path, after: Path) -> str:
    a, b = before.read_bytes(), after.read_bytes()
    for index, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return f"first differing byte at offset {index}"
    return "one is a prefix of the other"


def compare_trees(staged: Path, fixtures: Path) -> list[FileDelta]:
    """Every staged file against the checked-in one; checked-in files the generators do not produce are listed too."""
    deltas: list[FileDelta] = []
    produced = {p.relative_to(staged).as_posix() for p in staged.rglob("*") if p.is_file()}
    for relative in sorted(produced):
        new, old = staged / relative, fixtures / relative
        if not old.exists():
            deltas.append(FileDelta(relative, "new", None, sha256_of(new), None, new.stat().st_size, "not checked in"))
            continue
        before, after = sha256_of(old), sha256_of(new)
        if before == after:
            deltas.append(FileDelta(relative, "unchanged", before, after, old.stat().st_size, new.stat().st_size))
            continue
        detail = _first_text_difference(old, new) if relative.endswith((".csv", ".json")) else _first_byte_difference(old, new)
        deltas.append(FileDelta(relative, "moved", before, after, old.stat().st_size, new.stat().st_size, detail))
    for old in sorted(p for p in fixtures.rglob("*") if p.is_file()):
        relative = old.relative_to(fixtures).as_posix()
        if relative not in produced:
            deltas.append(FileDelta(relative, "not_regenerated", sha256_of(old), None, old.stat().st_size, None,
                                    "static file (README/attributes/synthetic scenario), no generator writes it"))
    return deltas


def compare_pins(root: Path, deltas: list[FileDelta]) -> list[dict]:
    """The binary allowlist of the publish scan pins each replay by sha256: report stale pins and the ones that would move."""
    config = json.loads((root / PIN_CONFIG).read_text(encoding="utf-8"))
    pins = config["assets"]["binary_allowlist"]
    by_path = {f"{FIXTURES}/{d.path}": d for d in deltas}
    rows = []
    for name in sorted(n for n in pins if n.startswith(f"{FIXTURES}/") and n.endswith(".eawr-gcreplay")):
        delta = by_path.get(name)
        pinned = pins[name]
        rows.append({
            "path": name,
            "pinned": pinned,
            "checked_in": delta.before if delta else None,
            "pin_matches_file": bool(delta and delta.before == pinned),
            "regenerated": delta.after if delta else None,
            "pin_would_move": bool(delta and delta.after and delta.after != pinned),
        })
    return rows


def render(run: DryRun, twice: bool) -> str:
    moved = run.moved
    out = ["# GC corpus upgrade dry run", ""]
    out.append(f"Files compared: {len(run.deltas)}; moved or new: {len(moved)}; "
               f"pins that would move: {sum(r['pin_would_move'] for r in run.pins)} of {len(run.pins)}.")
    if run.deterministic is not None:
        out.append(f"Generators deterministic (two runs byte-identical): **{'yes' if run.deterministic else 'NO: ' + ', '.join(run.nondeterministic_files)}**.")
    elif twice:
        out.append("Determinism: not checked.")
    out += ["", "| file | status | before | after | detail |", "|---|---|---|---|---|"]
    for d in run.deltas:
        out.append(f"| {d.path} | {d.status} | {(d.before or '')[:12]} | {(d.after or '')[:12]} | {d.detail} |")
    out += ["", "## Publish-scan pins (tools/publish_scan/config.json)", "", "| fixture | pin matches checked-in file | would move |", "|---|---|---|"]
    for r in run.pins:
        out.append(f"| {r['path']} | {'yes' if r['pin_matches_file'] else 'NO'} | {'yes' if r['pin_would_move'] else 'no'} |")
    out += ["", "## Planned bumps (reference)", "", "| bump | change | expected to touch |", "|---|---|---|"]
    out += [f"| {a} | {b} | {c} |" for a, b, c in BUMPS]
    return "\n".join(out) + "\n"


def _load_upgrade_module(root: Path):
    return types.SimpleNamespace(**runpy.run_path(str(root / "tools/replay/upgrade_galactic_corpus.py"), run_name="upgrade_galactic_corpus"))


def dry_run(args: argparse.Namespace) -> DryRun:
    root = args.root.resolve()
    upgrade = _load_upgrade_module(root)
    fixtures = root / FIXTURES
    args.out.mkdir(parents=True, exist_ok=True)
    result = DryRun()
    with tempfile.TemporaryDirectory(prefix="gc-corpus-dry-", dir=args.out) as temporary:
        first = Path(temporary) / "a"
        first.mkdir()
        upgrade.run_generators(args, first, root)
        if args.twice:
            second = Path(temporary) / "b"
            second.mkdir()
            upgrade.run_generators(args, second, root)
            differing = [p.relative_to(first).as_posix() for p in sorted(first.rglob("*")) if p.is_file()
                         and (not (second / p.relative_to(first)).exists() or sha256_of(p) != sha256_of(second / p.relative_to(first)))]
            result.deterministic = not differing
            result.nondeterministic_files = differing
        result.deltas = compare_trees(first, fixtures)
        result.pins = compare_pins(root, result.deltas)
        if args.keep_staged:
            shutil.copytree(first, args.out / "staged", dirs_exist_ok=True)
    return result


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True, help="report folder (outside --root's fixtures)")
    for name in ("basic", "economy", "transitions", "checkpoint", "control"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--twice", action="store_true", help="generate twice and require byte-identical output")
    parser.add_argument("--expect", choices=("clean", "moved", "any"), default="any")
    parser.add_argument("--keep-staged", action="store_true")
    args = parser.parse_args(argv)
    result = dry_run(args)
    report = render(result, args.twice)
    (args.out / "report.md").write_text(report, encoding="utf-8", newline="\n")
    (args.out / "report.json").write_text(json.dumps({
        "deterministic": result.deterministic, "deltas": [d.__dict__ for d in result.deltas], "pins": result.pins}, indent=1) + "\n",
        encoding="utf-8", newline="\n")
    print(report)
    ok = True
    if result.deterministic is False:
        ok = False
    if args.expect == "clean" and result.moved:
        print(f"expected a clean corpus, {len(result.moved)} files moved", file=sys.stderr)
        ok = False
    if args.expect == "moved" and not result.moved:
        print("expected the upgrade to move files, none did", file=sys.stderr)
        ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
