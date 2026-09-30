#!/usr/bin/env python3
"""Regression checks for strict fixed-point evidence target identity."""

from __future__ import annotations

import json
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


class ComparatorContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.root = Path(__file__).resolve().parents[2]
        cls.comparator = cls.root / "tools/compare_math_evidence.py"
        cls.manifest = cls.root / "tests/math/sequence-manifest.json"
        cls.oracle = cls.root / "tests/math/oracle-vectors.csv"
        cls.digest = json.loads(cls.manifest.read_text(encoding="utf-8"))["expected_sha256"]
        cls.vectors = cls.oracle.read_text(encoding="utf-8").replace(
            "expected_raw", "actual_raw", 1
        )

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="math-evidence-comparator-")
        self.evidence = Path(self.temporary.name)
        for target in TARGETS:
            self.write_target(target)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_target(self, target: str) -> Path:
        directory = self.evidence / f"math-evidence-{target}"
        directory.mkdir()
        (directory / "digest.txt").write_text(self.digest + "\n", encoding="utf-8")
        (directory / "vectors.csv").write_text(self.vectors, encoding="utf-8")
        return directory

    def compare(self, *extra: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(self.comparator),
                "--evidence-root",
                str(self.evidence),
                "--manifest",
                str(self.manifest),
                "--oracle-vectors",
                str(self.oracle),
                *extra,
            ],
            text=True,
            capture_output=True,
            check=False,
        )

    def assert_rejected(self, *extra: str) -> None:
        result = self.compare(*extra)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_exact_five_target_set_passes(self) -> None:
        result = self.compare()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("accepted 5 targets", result.stdout)

    def test_missing_target_is_rejected(self) -> None:
        shutil.rmtree(self.evidence / "math-evidence-linux-arm64-gcc")
        self.assert_rejected()

    def test_malformed_target_entry_is_rejected(self) -> None:
        path = self.evidence / "math-evidence-linux-arm64-gcc"
        shutil.rmtree(path)
        path.write_text("not an artifact directory\n", encoding="utf-8")
        self.assert_rejected()

    def test_unpaired_target_is_rejected(self) -> None:
        (self.evidence / "math-evidence-linux-x64-clang/vectors.csv").unlink()
        self.assert_rejected()

    def test_wrong_target_identity_is_rejected(self) -> None:
        source = self.evidence / "math-evidence-linux-arm64-gcc"
        source.rename(self.evidence / "math-evidence-unexpected-target")
        self.assert_rejected()

    def test_duplicate_alias_is_rejected(self) -> None:
        shutil.copytree(
            self.evidence / "math-evidence-windows-msvc",
            self.evidence / "windows-msvc",
        )
        self.assert_rejected()

    def test_nested_duplicate_payload_is_rejected(self) -> None:
        nested = self.evidence / "math-evidence-windows-msvc/duplicate"
        nested.mkdir()
        shutil.copy2(self.evidence / "math-evidence-windows-msvc/digest.txt", nested)
        shutil.copy2(self.evidence / "math-evidence-windows-msvc/vectors.csv", nested)
        self.assert_rejected()

    def test_digest_mismatch_is_rejected(self) -> None:
        (self.evidence / "math-evidence-linux-x64-gcc/digest.txt").write_text(
            "0" * 64 + "\n", encoding="utf-8"
        )
        self.assert_rejected()

    def test_cross_target_vector_mismatch_is_rejected(self) -> None:
        path = self.evidence / "math-evidence-windows-clang/vectors.csv"
        path.write_text(path.read_text(encoding="utf-8") + "\n", encoding="utf-8")
        self.assert_rejected()

    def test_identical_oracle_mismatch_is_rejected(self) -> None:
        lines = self.vectors.splitlines()
        lines[1] = lines[1].rsplit(",", 1)[0] + ",5"
        mismatched_vectors = "\n".join(lines) + "\n"
        for target in TARGETS:
            (self.evidence / f"math-evidence-{target}/vectors.csv").write_text(
                mismatched_vectors, encoding="utf-8"
            )
        self.assert_rejected()

    def test_reduced_local_set_requires_explicit_target_names(self) -> None:
        for target in TARGETS[2:]:
            shutil.rmtree(self.evidence / f"math-evidence-{target}")
        self.assert_rejected()
        result = self.compare("--target", TARGETS[0], "--target", TARGETS[1])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("accepted 2 targets", result.stdout)

    def test_duplicate_requested_target_is_rejected(self) -> None:
        for target in TARGETS[1:]:
            shutil.rmtree(self.evidence / f"math-evidence-{target}")
        self.assert_rejected("--target", TARGETS[0], "--target", TARGETS[0])


if __name__ == "__main__":
    unittest.main()
