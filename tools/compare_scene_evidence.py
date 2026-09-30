#!/usr/bin/env python3
"""Require canonical imported-scene evidence from every named CI target."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / "common"))
import eawr_evidence_io  # noqa: E402


DEFAULT_TARGETS = (
    "windows-msvc",
    "windows-clang",
    "linux-x64-gcc",
    "linux-x64-clang",
    "linux-arm64-gcc",
)
HEX64 = re.compile(r"[0-9a-f]{64}\Z")
HEX40 = re.compile(r"[0-9a-f]{40}\Z")
WORKERS = (1, 2, 4)
FIELDS = frozenset({"schema", "target", "build_id", "fixture_sha256",
                    "scene_sha256", "map_sha256", "contract_version", "workers_requested",
                    "executor", "parallel_stage", "partitions_completed", "placement_count",
                    "partition_placement_counts", "observed_worker_threads"})


def unique_pairs(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def reject_constant(value: str) -> object:
    raise ValueError(f"invalid JSON constant {value}")


def read_evidence(directory: pathlib.Path, target: str, workers: int, fixture_hash: str,
                  build_id: str) -> bytes:
    if not directory.is_dir():
        raise ValueError(f"missing scene evidence for {target}: {directory}")
    files = {p.name for p in directory.iterdir()}
    if files != {"evidence.json", "scene.txt"}:
        raise ValueError(f"{directory}: expected evidence.json and scene.txt only, found {sorted(files)}")
    try:
        raw = eawr_evidence_io.read_bytes(directory / "evidence.json")
        canonical = eawr_evidence_io.read_bytes(directory / "scene.txt")
        if raw.startswith(b"\xef\xbb\xbf") or b"\r" in raw or not raw.endswith(b"\n"):
            raise ValueError("metadata must be BOM-free, LF-only UTF-8 ending in LF")
        if canonical.startswith(b"\xef\xbb\xbf") or b"\r" in canonical or not canonical.endswith(b"\n"):
            raise ValueError("canonical scene must be BOM-free, LF-only UTF-8 ending in LF")
        metadata = json.loads(raw.decode("utf-8"), object_pairs_hook=unique_pairs,
                              parse_constant=reject_constant)
        canonical.decode("utf-8")
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValueError(f"{directory}: unreadable evidence: {error}") from error
    if type(metadata) is not dict or set(metadata) != FIELDS:
        raise ValueError(f"{directory}: metadata fields differ from the scene evidence contract")
    if type(metadata["schema"]) is not int or metadata["schema"] != 2 \
            or type(metadata["contract_version"]) is not int or metadata["contract_version"] != 1:
        raise ValueError(f"{directory}: unsupported scene evidence or builder contract version")
    if type(metadata["target"]) is not str or metadata["target"] != target:
        raise ValueError(f"{directory}: target identity mismatch: {metadata['target']!r} != {target!r}")
    if type(metadata["build_id"]) is not str or metadata["build_id"] != build_id:
        raise ValueError(f"{directory}: build identity mismatch")
    if type(metadata["fixture_sha256"]) is not str or metadata["fixture_sha256"] != fixture_hash:
        raise ValueError(f"{directory}: fixture identity mismatch")
    if type(metadata["map_sha256"]) is not str or not HEX64.fullmatch(metadata["map_sha256"]):
        raise ValueError(f"{directory}: malformed map hash")
    if type(metadata["scene_sha256"]) is not str or not HEX64.fullmatch(metadata["scene_sha256"]):
        raise ValueError(f"{directory}: malformed scene hash")
    if type(metadata["executor"]) is not str or metadata["executor"] != "thread-worker-adapter" \
            or type(metadata["parallel_stage"]) is not str \
            or metadata["parallel_stage"] != "placement-finalization-v1":
        raise ValueError(f"{directory}: executor or parallel stage mismatch")
    for field, expected in (("workers_requested", workers), ("partitions_completed", workers),
                            ("placement_count", 7), ("observed_worker_threads", workers)):
        if type(metadata[field]) is not int or metadata[field] != expected:
            raise ValueError(f"{directory}: {field} mismatch")
    expected_counts = [sum(index % workers == part for index in range(7)) for part in range(workers)]
    counts = metadata["partition_placement_counts"]
    if type(counts) is not list or any(type(value) is not int for value in counts) \
            or counts != expected_counts:
        raise ValueError(f"{directory}: partition placement counts mismatch")
    if not canonical.startswith(b"eawr-static-scene 1\n"):
        raise ValueError(f"{directory}: canonical scene header mismatch")
    lines = canonical.split(b"\n", 2)
    expected_map = (b"map data/art/maps/eawr_scene_synthetic.ted "
                    + metadata["map_sha256"].encode("ascii") + b" land")
    if len(lines) < 3 or lines[1] != expected_map:
        raise ValueError(f"{directory}: canonical map identity mismatch")
    actual = hashlib.sha256(canonical).hexdigest()
    if metadata["scene_sha256"] != actual:
        raise ValueError(f"{directory}: scene hash does not match canonical input")
    return canonical


def compare(root: pathlib.Path, fixture: pathlib.Path, build_id: str,
            targets: tuple[str, ...]) -> str:
    if not HEX40.fullmatch(build_id):
        raise ValueError("build identity must be a lowercase 40-digit Git SHA")
    if not targets or len(targets) != len(set(targets)) or any(t not in DEFAULT_TARGETS for t in targets):
        raise ValueError("target list must contain distinct named CI targets")
    try:
        fixture_hash = hashlib.sha256(fixture.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
        present = {p.name for p in root.iterdir()}
    except OSError as error:
        raise ValueError(f"cannot read scene evidence or fixture: {error}") from error
    expected = {f"scene-evidence-{target}" for target in targets}
    if present != expected:
        raise ValueError(f"scene evidence directories mismatch: missing {sorted(expected - present)}, extra {sorted(present - expected)}")
    scenes = {}
    for target in targets:
        directory = root / f"scene-evidence-{target}"
        if not directory.is_dir():
            raise ValueError(f"missing scene evidence directory: {directory}")
        lanes = {p.name for p in directory.iterdir()}
        wanted = {f"workers-{workers}" for workers in WORKERS}
        if lanes != wanted:
            raise ValueError(f"{directory}: worker lanes mismatch: missing {sorted(wanted - lanes)}, extra {sorted(lanes - wanted)}")
        for workers in WORKERS:
            scenes[target, workers] = read_evidence(directory / f"workers-{workers}", target,
                                                    workers, fixture_hash, build_id)
    if len(set(scenes.values())) != 1:
        raise ValueError("cross-worker or cross-target canonical scene mismatch")
    return hashlib.sha256(next(iter(scenes.values()))).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence-root", type=pathlib.Path, required=True)
    parser.add_argument("--fixture", type=pathlib.Path, required=True)
    parser.add_argument("--build-id", required=True)
    parser.add_argument("--target", action="append", dest="targets")
    args = parser.parse_args()
    try:
        digest = compare(args.evidence_root, args.fixture, args.build_id,
                         tuple(args.targets) if args.targets else DEFAULT_TARGETS)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 1
    print(f"canonical imported scene agrees across {len(args.targets or DEFAULT_TARGETS) * 3} receipts: {digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
