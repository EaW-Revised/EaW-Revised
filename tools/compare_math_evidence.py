#!/usr/bin/env python3
"""Require five identical P0-03 target outputs and agreement with frozen oracles."""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "common"))
import eawr_evidence_io  # noqa: E402


EXPECTED_TARGETS = (
    "windows-msvc",
    "windows-clang",
    "linux-x64-gcc",
    "linux-x64-clang",
    "linux-arm64-gcc",
)
ARTIFACT_PREFIX = "math-evidence-"
PAYLOAD_NAMES = ("digest.txt", "vectors.csv")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence-root", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--oracle-vectors", type=Path, required=True)
    parser.add_argument(
        "--target",
        action="append",
        choices=EXPECTED_TARGETS,
        dest="targets",
        metavar="TARGET",
        help=(
            "target identity to compare; repeat for an explicit reduced local set "
            "(default: require all five CI targets)"
        ),
    )
    return parser.parse_args()


def read_rows(path: Path) -> list[dict[str, str]]:
    def read() -> list[dict[str, str]]:
        with path.open(encoding="utf-8", newline="") as stream:
            return list(csv.DictReader(stream))

    return eawr_evidence_io.retrying(read)


def evidence_paths(root: Path, targets: list[str] | None) -> tuple[list[Path], list[Path]]:
    if not root.is_dir():
        raise SystemExit(f"evidence root is not a directory: {root}")

    expected_targets = tuple(targets) if targets else EXPECTED_TARGETS
    if len(set(expected_targets)) != len(expected_targets):
        raise SystemExit(f"duplicate target identity requested: {expected_targets}")

    expected_directories = {f"{ARTIFACT_PREFIX}{target}" for target in expected_targets}
    actual_directories = {path.name for path in root.iterdir() if path.is_dir()}
    missing = sorted(expected_directories - actual_directories)
    unexpected = sorted(actual_directories - expected_directories)
    if missing or unexpected:
        raise SystemExit(
            f"target artifact directory mismatch: missing={missing}, unexpected={unexpected}"
        )

    digest_files: list[Path] = []
    vector_files: list[Path] = []
    expected_payloads: set[Path] = set()
    for target in expected_targets:
        directory = root / f"{ARTIFACT_PREFIX}{target}"
        if directory.is_symlink():
            raise SystemExit(f"target artifact directory must not be a symlink: {directory}")
        digest = directory / "digest.txt"
        vectors = directory / "vectors.csv"
        for payload in (digest, vectors):
            if not payload.is_file() or payload.is_symlink():
                raise SystemExit(f"missing or non-regular target payload: {payload}")
            expected_payloads.add(payload)
        digest_files.append(digest)
        vector_files.append(vectors)

    discovered_payloads = {
        path for name in PAYLOAD_NAMES for path in root.rglob(name) if path.is_file()
    }
    extras = sorted(discovered_payloads - expected_payloads)
    if extras:
        raise SystemExit(f"duplicate or nested target payloads found: {extras}")
    return digest_files, vector_files


def main() -> int:
    args = parse_args()
    digest_files, vector_files = evidence_paths(args.evidence_root, args.targets)
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    expected_digest = manifest["expected_sha256"]
    digests = [eawr_evidence_io.read_text(path).strip() for path in digest_files]
    if any(value != expected_digest for value in digests):
        raise SystemExit(f"target digest mismatch: {digests}")

    vector_payloads = [eawr_evidence_io.read_bytes(path) for path in vector_files]
    if any(payload != vector_payloads[0] for payload in vector_payloads[1:]):
        raise SystemExit("approximate output artifacts differ across targets")
    actual = read_rows(vector_files[0])
    oracle = read_rows(args.oracle_vectors)
    if len(actual) != len(oracle):
        raise SystemExit(f"vector count mismatch: {len(actual)} != {len(oracle)}")
    for index, (produced, expected) in enumerate(zip(actual, oracle)):
        keys = ("operation", "a", "b", "c")
        if any(produced[key] != expected[key] for key in keys):
            raise SystemExit(f"vector identity mismatch at row {index + 2}")
        error = abs(int(produced["actual_raw"]) - int(expected["expected_raw"]))
        if error > 4:
            raise SystemExit(f"vector error {error} exceeds four quanta at row {index + 2}")
    print(
        f"accepted {len(digest_files)} targets: digest {expected_digest}, "
        f"{len(actual)} identical approximate outputs"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
