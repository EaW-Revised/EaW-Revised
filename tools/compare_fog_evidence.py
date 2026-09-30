#!/usr/bin/env python3
"""Require complete, byte-bound fog-stub-v1 evidence for every named lane."""

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
WORKERS = (1, 2, 4)
ARTIFACT_PREFIX = "fog-evidence-"
DIGEST = re.compile(r"[0-9a-f]{64}\Z")
TARGET = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*\Z")
ROW = re.compile(r"(0|[1-9][0-9]*),([0-9a-f]{64})\Z")


def _read_lf(path: pathlib.Path) -> list[str]:
    """Read a strict UTF-8/LF text file without accepting normalized variants."""

    try:
        raw = eawr_evidence_io.read_bytes(path)
    except OSError as error:
        raise ValueError(f"missing required result {path}: {error}") from error
    if raw.startswith(b"\xef\xbb\xbf") or b"\r" in raw or not raw.endswith(b"\n"):
        raise ValueError(f"{path}: expected UTF-8 without BOM and LF-only lines")
    try:
        return raw.decode("utf-8").splitlines()
    except UnicodeDecodeError as error:
        raise ValueError(f"{path}: invalid UTF-8: {error}") from error


def _read_digest(path: pathlib.Path, label: str) -> str:
    lines = _read_lf(path)
    if len(lines) != 1 or DIGEST.fullmatch(lines[0]) is None:
        raise ValueError(f"{path}: {label} must contain one lowercase SHA-256 line")
    return lines[0]


def _read_target(path: pathlib.Path, expected: str) -> None:
    lines = _read_lf(path)
    if len(lines) != 1 or lines[0] != expected:
        found = lines[0] if len(lines) == 1 else "<malformed>"
        raise ValueError(
            f"target identity mismatch in {path}: expected {expected!r}, found {found!r}"
        )


def read_rows(path: pathlib.Path) -> list[tuple[int, str]]:
    lines = _read_lf(path)
    if not lines or lines[0] != "tick,sha256":
        raise ValueError(f"{path}: missing exact tick,sha256 header")
    rows: list[tuple[int, str]] = []
    for line_number, line in enumerate(lines[1:], 2):
        match = ROW.fullmatch(line)
        if not match:
            raise ValueError(f"{path}:{line_number}: malformed hash row")
        tick = int(match.group(1))
        if tick != len(rows):
            raise ValueError(
                f"{path}:{line_number}: first missing/out-of-order tick is {len(rows)}"
            )
        rows.append((tick, match.group(2)))
    if not rows:
        raise ValueError(f"{path}: missing required tick 0 row")
    return rows


def compare_rows(
    expected: list[tuple[int, str]],
    candidate: list[tuple[int, str]],
    candidate_name: str,
) -> None:
    shared = min(len(expected), len(candidate))
    for index in range(shared):
        if expected[index] != candidate[index]:
            tick = expected[index][0]
            raise ValueError(
                f"first divergent tick {tick}: independent oracle={expected[index][1]} "
                f"{candidate_name}={candidate[index][1]}"
            )
    if len(expected) != len(candidate):
        raise ValueError(
            f"first missing tick {shared}: independent oracle has {len(expected)} rows, "
            f"{candidate_name} has {len(candidate)} rows"
        )


def _read_file_digest(path: pathlib.Path, label: str) -> str:
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"{label} must be a regular file: {path}")
    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as error:
        raise ValueError(f"could not read {label} {path}: {error}") from error
    return digest.hexdigest()


def _audit_digest(value: object, label: str) -> str:
    if not isinstance(value, str) or DIGEST.fullmatch(value) is None:
        raise ValueError(f"audit {label} is not a lowercase SHA-256 digest")
    return value


def _audit_rows(audit: object) -> tuple[dict[str, object], list[tuple[int, str]]]:
    if not isinstance(audit, dict):
        raise ValueError("fog audit must be a JSON object")
    fixtures = audit.get("fixtures")
    if not isinstance(fixtures, dict) or not isinstance(fixtures.get("fog-stub-v1"), dict):
        raise ValueError("fog audit is missing fixtures.fog-stub-v1")
    fixture = fixtures["fog-stub-v1"]
    evidence = fixture.get("evidence")
    if not isinstance(evidence, list) or not evidence:
        raise ValueError("fog audit is missing non-empty fog-stub-v1 evidence")
    rows: list[tuple[int, str]] = []
    for index, item in enumerate(evidence):
        if not isinstance(item, dict):
            raise ValueError(f"fog audit evidence row {index} is not an object")
        tick = item.get("completed_tick")
        if isinstance(tick, bool) or not isinstance(tick, int) or tick != index:
            raise ValueError(
                f"fog audit evidence is not contiguous from tick 0 at row {index}"
            )
        rows.append((tick, _audit_digest(item.get("sha256"), f"evidence row {index}")))
    return fixture, rows


def _strict_targets(requested: list[str] | None) -> tuple[str, ...]:
    targets = tuple(requested) if requested else DEFAULT_TARGETS
    if not targets:
        raise ValueError("at least one target is required")
    if len(targets) != len(set(targets)):
        raise ValueError(f"duplicate target identity requested: {targets}")
    if any(TARGET.fullmatch(target) is None for target in targets):
        raise ValueError(f"invalid target identity requested: {targets}")
    return targets


def _evidence_directories(root: pathlib.Path, targets: tuple[str, ...]) -> dict[str, pathlib.Path]:
    if root.is_symlink() or not root.is_dir():
        raise ValueError(f"evidence root is not a directory: {root}")
    expected = {f"{ARTIFACT_PREFIX}{target}" for target in targets}
    try:
        actual = {entry.name for entry in root.iterdir()}
    except OSError as error:
        raise ValueError(f"could not inspect evidence root {root}: {error}") from error
    missing = sorted(expected - actual)
    unexpected = sorted(actual - expected)
    if missing or unexpected:
        raise ValueError(
            f"target artifact directory mismatch: missing={missing}, unexpected={unexpected}"
        )
    directories: dict[str, pathlib.Path] = {}
    for target in targets:
        directory = root / f"{ARTIFACT_PREFIX}{target}"
        if directory.is_symlink() or not directory.is_dir():
            raise ValueError(f"target artifact directory must be a real directory: {directory}")
        directories[target] = directory
    return directories


def _check_target_directory(
    directory: pathlib.Path,
    target: str,
    replay_digest: str,
    sidecar_digest: str,
) -> None:
    expected_entries = {
        "target.txt",
        "replay.sha256",
        "sidecar.sha256",
        *(f"workers-{workers}.csv" for workers in WORKERS),
    }
    try:
        actual_entries = {entry.name for entry in directory.iterdir()}
    except OSError as error:
        raise ValueError(f"could not inspect target artifact {directory}: {error}") from error
    missing = sorted(expected_entries - actual_entries)
    unexpected = sorted(actual_entries - expected_entries)
    if missing or unexpected:
        raise ValueError(
            f"{directory}: lane artifact mismatch: missing={missing}, unexpected={unexpected}"
        )
    _read_target(directory / "target.txt", target)
    recorded_replay = _read_digest(directory / "replay.sha256", "replay identity")
    recorded_sidecar = _read_digest(directory / "sidecar.sha256", "sidecar identity")
    if recorded_replay != replay_digest:
        raise ValueError(
            f"raw replay identity mismatch in {directory}: expected {replay_digest}, "
            f"found {recorded_replay}"
        )
    if recorded_sidecar != sidecar_digest:
        raise ValueError(
            f"raw sidecar identity mismatch in {directory}: expected {sidecar_digest}, "
            f"found {recorded_sidecar}"
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence-root", type=pathlib.Path, required=True)
    parser.add_argument(
        "--audit",
        type=pathlib.Path,
        required=True,
        help="frozen independent fog-stub-v1 audit JSON",
    )
    parser.add_argument(
        "--oracle",
        type=pathlib.Path,
        help="independent tick,sha256 oracle CSV; checked against the audit when supplied",
    )
    parser.add_argument(
        "--replay",
        type=pathlib.Path,
        required=True,
        help="raw original-v1 replay bytes used to bind the sidecar",
    )
    parser.add_argument(
        "--sidecar",
        type=pathlib.Path,
        required=True,
        help="raw fog-stub-v1 sidecar bytes used to produce evidence",
    )
    parser.add_argument(
        "--target",
        action="append",
        dest="targets",
        metavar="TARGET",
        help="expected target name; repeat for an explicit reduced local set",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        targets = _strict_targets(args.targets)
        try:
            audit = json.loads(args.audit.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise ValueError(f"could not read frozen fog audit {args.audit}: {error}") from error
        fixture, expected = _audit_rows(audit)

        replay_digest = _read_file_digest(args.replay, "raw replay")
        sidecar_digest = _read_file_digest(args.sidecar, "raw sidecar")
        expected_replay = _audit_digest(fixture.get("replay_sha256"), "replay_sha256")
        expected_sidecar = _audit_digest(fixture.get("sha256"), "sidecar sha256")
        if replay_digest != expected_replay:
            raise ValueError(
                f"raw replay does not match frozen audit: expected {expected_replay}, found {replay_digest}"
            )
        if sidecar_digest != expected_sidecar:
            raise ValueError(
                f"raw sidecar does not match frozen audit: expected {expected_sidecar}, found {sidecar_digest}"
            )

        original_v1 = audit.get("original_v1")
        if isinstance(original_v1, dict):
            frozen_original = _audit_digest(original_v1.get("sha256"), "original_v1.sha256")
            if frozen_original != replay_digest:
                raise ValueError("fog audit original_v1 identity disagrees with fog fixture identity")

        if args.oracle is not None:
            oracle = read_rows(args.oracle)
            compare_rows(expected, oracle, "independent oracle CSV")

        directories = _evidence_directories(args.evidence_root, targets)
        for target, directory in directories.items():
            _check_target_directory(directory, target, replay_digest, sidecar_digest)
            for workers in WORKERS:
                lane = f"{target}/workers-{workers}"
                rows = read_rows(directory / f"workers-{workers}.csv")
                compare_rows(expected, rows, lane)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 1

    print(
        f"fog evidence agrees across {len(targets)} named targets, {len(WORKERS)} worker counts, "
        f"and {len(expected)} frozen ticks (including tick 0); raw replay/sidecar identities verified"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
