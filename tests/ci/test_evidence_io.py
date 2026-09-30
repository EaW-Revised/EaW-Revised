"""tools/common/eawr_evidence_io.py: retry past a transient Windows file lock (#527).

A hash or trace comparator must never be flaky. This guards the retry helper itself,
and that every tools/compare_*.py evidence comparator reads its freshly produced
evidence (not its static, checked-in inputs) through it rather than a bare
`path.read_bytes()`/`read_text()`/`open()`, so the class of bug stays fixed everywhere
a comparator reads evidence another process just finished writing.
"""

import pathlib
import re
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "common"))
import eawr_evidence_io  # noqa: E402

COMPARATORS = tuple((ROOT / "tools").glob("compare_*.py"))
BARE_READ = re.compile(r"(?<!eawr_evidence_io\.)\bpath\.read_(?:bytes|text)\(\)")


class RetryTests(unittest.TestCase):
    def test_a_transient_permission_error_is_retried_and_then_succeeds(self):
        attempts = []

        def read():
            attempts.append(1)
            if len(attempts) < 3:
                raise PermissionError(13, "Permission denied")
            return b"payload"

        self.assertEqual(eawr_evidence_io.retrying(read), b"payload")
        self.assertEqual(len(attempts), 3)

    def test_a_permission_error_past_the_retry_budget_still_raises(self):
        def read():
            raise PermissionError(13, "Permission denied")

        with self.assertRaises(PermissionError):
            eawr_evidence_io.retrying(read)

    def test_a_missing_file_is_never_retried(self):
        attempts = []

        def read():
            attempts.append(1)
            raise FileNotFoundError(2, "No such file or directory")

        with self.assertRaises(FileNotFoundError):
            eawr_evidence_io.retrying(read)
        self.assertEqual(len(attempts), 1)

    def test_read_bytes_and_read_text_round_trip(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp, "evidence.csv")
            path.write_bytes(b"tick,sha256\n1,ab\n")
            self.assertEqual(eawr_evidence_io.read_bytes(path), b"tick,sha256\n1,ab\n")
            self.assertEqual(eawr_evidence_io.read_text(path), "tick,sha256\n1,ab\n")


class ComparatorUsageTests(unittest.TestCase):
    """Every evidence comparator routes its per-run evidence reads through the retry guard."""

    def test_at_least_the_known_comparators_are_present(self):
        names = {path.name for path in COMPARATORS}
        for expected in ("compare_replay_hashes.py", "compare_fog_evidence.py",
                         "compare_scene_evidence.py", "compare_math_evidence.py",
                         "compare_traces.py"):
            self.assertIn(expected, names)

    def test_every_comparator_imports_the_retry_helper(self):
        missing = [
            path.name for path in COMPARATORS
            if "import eawr_evidence_io" not in path.read_text(encoding="utf-8")
        ]
        self.assertEqual(missing, [], "wire these through tools/common/eawr_evidence_io.py")

    def test_no_comparator_reads_a_bare_path_read_bytes_or_read_text(self):
        offenders = {}
        for path in COMPARATORS:
            hits = BARE_READ.findall(path.read_text(encoding="utf-8"))
            if hits:
                offenders[path.name] = len(hits)
        self.assertEqual(offenders, {}, "route these through eawr_evidence_io.read_bytes/read_text")


if __name__ == "__main__":
    unittest.main()
