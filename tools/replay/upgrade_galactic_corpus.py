"""Explicit format upgrade using the repository's synthetic C++ generators.

Invoked only by the opt-in galactic_corpus_upgrade target. Generates into a fresh
directory, verifies the existing fixture inventory, then publishes fixtures and
their binary allowlist digests together. The zip is a portable offload artifact.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile


def generator_commands(args: argparse.Namespace, staged: Path) -> list[list]:
    return [
        [args.basic, staged / "corpus"],
        [args.economy, "galactic_galaxy_graph_tests", "--generate", staged / "graph-corpus"],
        [args.economy, "galactic_fleet_tests", "--generate", staged / "fleet-corpus"],
        [args.transitions, "galactic_movement_tests", "--generate", staged / "movement-corpus"],
        [args.checkpoint, "galactic_production_tests", "--write-corpus", staged / "production-corpus"],
        [args.control, "--generate", staged / "control-corpus"],
        [args.economy, "galactic_auto_resolve_outcome_tests", "--generate", staged / "outcome-corpus/space-loss.eawr-gcreplay"],
    ]


def run_generators(args: argparse.Namespace, staged: Path, root: Path) -> None:
    """Run the synthetic C++ generators into `staged` (shared by the upgrade and the read-only dry run)."""
    (staged / "outcome-corpus").mkdir(exist_ok=True)
    for command in generator_commands(args, staged):
        subprocess.run([str(value) for value in command], check=True, cwd=root, timeout=600)


def upgrade(args: argparse.Namespace) -> None:
    root = args.root.resolve()
    fixtures = root / "tests/galactic/fixtures"
    config_path = root / "tools/publish_scan/config.json"
    config = json.loads(config_path.read_text(encoding="utf-8"))
    pins = config["assets"]["binary_allowlist"]
    expected = {Path(name).relative_to("tests/galactic/fixtures") for name in pins
                if name.startswith("tests/galactic/fixtures/") and name.endswith(".eawr-gcreplay")}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="gc-corpus-upgrade-", dir=args.out.parent) as temporary:
        staged = Path(temporary)
        run_generators(args, staged, root)
        generated = {path.relative_to(staged) for path in staged.rglob("*.eawr-gcreplay")}
        if generated != expected:
            raise ValueError(f"generator inventory differs: missing={sorted(expected-generated)}, extra={sorted(generated-expected)}")
        for relative in sorted(generated):
            target = fixtures / relative
            # Existing corpora with pins must continue to have generator-authored pins.
            if target.with_suffix(".hashes.csv").exists() and not (staged / relative).with_suffix(".hashes.csv").exists():
                raise ValueError(f"generator omitted hashes: {relative}")
            pins[target.relative_to(root).as_posix()] = hashlib.sha256((staged / relative).read_bytes()).hexdigest()
        outputs = sorted(path for path in staged.rglob("*") if path.is_file())
        for source in outputs:
            target = fixtures / source.relative_to(staged)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
        config_path.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8", newline="\n")
        with zipfile.ZipFile(args.out, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for source in outputs:
                target = fixtures / source.relative_to(staged)
                archive.write(target, target.relative_to(root).as_posix())
            archive.write(config_path, config_path.relative_to(root).as_posix())
    print(f"GC corpus upgrade: {len(expected)} generator-authored replays; artifact {args.out}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    for name in ("basic", "economy", "transitions", "checkpoint", "control"):
        parser.add_argument("--" + name, type=Path, required=True)
    upgrade(parser.parse_args())


if __name__ == "__main__":
    main()
