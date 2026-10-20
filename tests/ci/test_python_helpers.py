"""File-format and error contracts shared by Python test and tool callers."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/common"))
import eawr_python_helpers as helpers


class PythonHelpers(unittest.TestCase):
    def test_json_formats_and_parent_policy(self):
        expected = '{\n  "a": 2,\n  "z": "é"\n}\n'
        self.assertEqual(helpers.json_bytes({"z": "é", "a": 2}, ensure_ascii=False, sort_keys=True), expected.encode())
        self.assertEqual(helpers.json_bytes({"z": "é"}), b'{\n  "z": "\\u00e9"\n}\n')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "nested/report.json"
            with self.assertRaises(FileNotFoundError):
                helpers.write_json(path, {})
            for binary in (False, True):
                helpers.write_json(path, {"z": "é", "a": 2}, sort_keys=True, ensure_ascii=False,
                                   create_parents=True, binary=binary)
                self.assertEqual(path.read_bytes(), expected.replace("\n", "\n" if binary else os.linesep).encode())

    def test_json_errors_and_bom(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "nested/report.json"
            with self.assertRaisesRegex(ValueError, "Out of range float values"):
                helpers.write_json(path, float("nan"), allow_nan=False, create_parents=True)
            self.assertTrue(path.parent.is_dir())
            path.write_bytes(b'\xef\xbb\xbf{"value": 3}')
            with self.assertRaises(json.JSONDecodeError):
                helpers.read_json(path)
            self.assertEqual(helpers.read_json(path, encoding="utf-8-sig"), {"value": 3})

    def test_hash_crosses_chunk_boundary_and_missing_file_raises(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bytes"
            path.write_bytes(b"x" * (1024 * 1024 + 1))
            self.assertEqual(helpers.sha256_file(str(path)), "154b8ed3c2383ce429058768595935faf7851b5c38db2b1732594be1d88bc05a")
            path.unlink()
            with self.assertRaises(FileNotFoundError):
                helpers.sha256_file(path)


if __name__ == "__main__":
    unittest.main()
