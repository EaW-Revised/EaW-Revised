#!/usr/bin/env python3
"""Contract tests for strict fog evidence discovery and mismatch reporting."""

from __future__ import annotations

import hashlib
import argparse
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


TARGETS = (
    "windows-msvc",
    "windows-clang",
    "linux-x64-gcc",
    "linux-x64-clang",
    "linux-arm64-gcc",
)
WORKERS = (1, 2, 4)


def write_lf_text(path: Path, text: str) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as output:
        output.write(text)


class ComparatorContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.root = Path(__file__).resolve().parents[2]
        cls.comparator = cls.root / "tools/compare_fog_evidence.py"
        cls.audit = cls.root / "tests/fog/fixtures/fog-stub-v1.audit.json"
        cls.oracle = cls.root / "tests/fog/fixtures/fog-stub-v1.evidence.csv"
        cls.replay = cls.root / "tests/replay/fixtures/original-v1.eawr-replay"
        cls.sidecar = cls.root / "tests/fog/fixtures/fog-stub-v1.eawr-fog"
        cls.oracle_bytes = cls.oracle.read_bytes()
        cls.replay_digest = hashlib.sha256(cls.replay.read_bytes()).hexdigest()
        cls.sidecar_digest = hashlib.sha256(cls.sidecar.read_bytes()).hexdigest()

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="fog-evidence-comparator-")
        self.evidence = Path(self.temporary.name)
        for target in TARGETS:
            self.write_target(target)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_target(
        self,
        target: str,
        *,
        replay_digest: str | None = None,
        sidecar_digest: str | None = None,
    ) -> Path:
        directory = self.evidence / f"fog-evidence-{target}"
        directory.mkdir(parents=True, exist_ok=True)
        write_lf_text(directory / "target.txt", target + "\n")
        write_lf_text(directory / "replay.sha256", (replay_digest or self.replay_digest) + "\n")
        write_lf_text(directory / "sidecar.sha256", (sidecar_digest or self.sidecar_digest) + "\n")
        for workers in WORKERS:
            (directory / f"workers-{workers}.csv").write_bytes(self.oracle_bytes)
        return directory

    def compare(self, *extra: str, oracle: Path | None = None) -> subprocess.CompletedProcess[str]:
        command = [
            sys.executable,
            str(self.comparator),
            "--evidence-root",
            str(self.evidence),
            "--audit",
            str(self.audit),
            "--oracle",
            str(oracle or self.oracle),
            "--replay",
            str(self.replay),
            "--sidecar",
            str(self.sidecar),
        ]
        command.extend(extra)
        return subprocess.run(command, text=True, capture_output=True, check=False)

    def assert_rejected(self, *extra: str, oracle: Path | None = None) -> str:
        result = self.compare(*extra, oracle=oracle)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def test_exact_five_targets_and_fifteen_lanes_pass(self) -> None:
        result = self.compare()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("5 named targets", result.stdout)
        self.assertIn("including tick 0", result.stdout)

    def test_missing_worker_lane_is_rejected(self) -> None:
        (self.evidence / "fog-evidence-linux-arm64-gcc/workers-4.csv").unlink()
        message = self.assert_rejected()
        self.assertIn("workers-4.csv", message)

    def test_missing_tick_zero_is_rejected(self) -> None:
        path = self.evidence / "fog-evidence-windows-msvc/workers-1.csv"
        path.write_bytes(b"\n".join(self.oracle_bytes.splitlines()[:1] + self.oracle_bytes.splitlines()[2:]) + b"\n")
        message = self.assert_rejected()
        self.assertIn("first missing/out-of-order tick is 0", message)

    def test_divergent_row_is_rejected(self) -> None:
        path = self.evidence / "fog-evidence-linux-x64-clang/workers-2.csv"
        lines = self.oracle_bytes.decode("utf-8").splitlines()
        lines[2] = lines[2].rsplit(",", 1)[0] + "," + ("0" * 64)
        write_lf_text(path, "\n".join(lines) + "\n")
        message = self.assert_rejected()
        self.assertIn("first divergent tick 1", message)

    def test_raw_replay_identity_is_required(self) -> None:
        directory = self.evidence / "fog-evidence-windows-clang"
        write_lf_text(directory / "replay.sha256", "0" * 64 + "\n")
        message = self.assert_rejected()
        self.assertIn("raw replay identity mismatch", message)

    def test_raw_sidecar_identity_is_required(self) -> None:
        directory = self.evidence / "fog-evidence-windows-clang"
        write_lf_text(directory / "sidecar.sha256", "0" * 64 + "\n")
        message = self.assert_rejected()
        self.assertIn("raw sidecar identity mismatch", message)

    def test_copied_target_identity_is_rejected(self) -> None:
        directory = self.evidence / "fog-evidence-windows-clang"
        write_lf_text(directory / "target.txt", "windows-msvc\n")
        message = self.assert_rejected()
        self.assertIn("target identity mismatch", message)

    def test_independent_oracle_disagreement_is_rejected(self) -> None:
        oracle = self.evidence / "oracle.csv"
        lines = self.oracle_bytes.decode("utf-8").splitlines()
        lines[1] = lines[1].rsplit(",", 1)[0] + "," + ("0" * 64)
        write_lf_text(oracle, "\n".join(lines) + "\n")
        message = self.assert_rejected(oracle=oracle)
        self.assertIn("first divergent tick 0", message)

    def test_missing_target_is_rejected_by_default(self) -> None:
        shutil.rmtree(self.evidence / "fog-evidence-linux-arm64-gcc")
        message = self.assert_rejected()
        self.assertIn("linux-arm64-gcc", message)

    def test_explicit_reduced_local_set_is_named(self) -> None:
        for target in TARGETS[2:]:
            shutil.rmtree(self.evidence / f"fog-evidence-{target}")
        result = self.compare(
            "--target",
            TARGETS[0],
            "--target",
            TARGETS[1],
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("2 named targets", result.stdout)

    def test_unexpected_target_artifact_is_rejected(self) -> None:
        self.write_target("unexpected-target")
        message = self.assert_rejected()
        self.assertIn("unexpected", message)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path)
    options, _ = parser.parse_known_args()
    if options.tool is not None:
        ComparatorContracts.comparator = options.tool
    unittest.main(argv=[sys.argv[0]])
