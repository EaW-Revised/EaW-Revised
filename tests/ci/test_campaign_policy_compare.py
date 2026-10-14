"""Installed policy comparison skips only when game data is not enabled."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("policy", ROOT / "tests/galactic/ai_campaign_policy_compare.py")
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)


class PolicyCompareTests(unittest.TestCase):
    def invoke(self, root, args):
        output = io.StringIO()
        with patch.dict(os.environ, {"EAWR_EAW_GAME_ROOT": root}), contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            code = policy.main(args)
        return code, output.getvalue()

    def test_missing_game_data_skips_before_reading_receipts(self):
        code, output = self.invoke("", [])
        self.assertEqual(code, 0)
        self.assertIn("SKIPPED:", output)

    def test_enabled_game_data_requires_actual_receipts(self):
        with tempfile.TemporaryDirectory() as directory:
            code, output = self.invoke(directory, [directory, directory])
        self.assertEqual(code, 1)
        self.assertNotIn("SKIPPED:", output)

    def test_enabled_comparison_checks_payload_and_verdicts(self):
        with tempfile.TemporaryDirectory() as directory:
            left, right = Path(directory) / "correctness", Path(directory) / "benchmark"
            result = dict(correctness_passed=True, release_gate_passed=True,
                          final_hash="synthetic", synchronized_frames=3, galactic_frames=2,
                          players=2, battles=1, returns=1, terminal_result="win", blocked_invasion=True)
            for path in (left, right):
                path.mkdir()
                (path / "result.json").write_text(json.dumps(result), encoding="utf-8")
                (path / "case.json").write_text('{"seed":42}', encoding="utf-8")
                for name in ("reservation", "transit", "blocked-invasion", "post-return"):
                    (path / f"{name}.eawr").write_bytes(b"synthetic payload")
            args = [str(left), str(right)]
            self.assertEqual(self.invoke(directory, args)[0], 0)
            (right / "post-return.eawr").write_bytes(b"different payload")
            self.assertEqual(self.invoke(directory, args)[0], 1)
            (right / "post-return.eawr").write_bytes(b"synthetic payload")
            result["release_gate_passed"] = False
            (right / "result.json").write_text(json.dumps(result), encoding="utf-8")
            self.assertEqual(self.invoke(directory, args)[0], 1)


if __name__ == "__main__":
    unittest.main()
