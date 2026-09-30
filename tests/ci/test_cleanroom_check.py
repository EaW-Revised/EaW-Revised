#!/usr/bin/env python3
"""The tracked tree has no FoC engine names in its files (#825)."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import cleanroom_check  # noqa: E402


class CleanroomCheckTests(unittest.TestCase):
    def test_pattern_catches_engine_names(self) -> None:
        # Made-up names of each shape; the parts are joined so this file itself scans clean.
        for name in ("al" + "Widget:" + ":Check_Thing", "Gizmo" + "Class", "Gizmo" + "Class:" + ":Get_X",
                     "m_" + "some_field", "0x" + "140000000", "al" + "HGizmo"):
            self.assertIsNotNone(cleanroom_check.PATTERN.search(name), name)

    def test_tracked_files_are_clean(self) -> None:
        found = cleanroom_check.scan()
        self.assertEqual(found, [], "\n".join(f"{r}:{n}: {t}" for r, n, t, _ in found))


if __name__ == "__main__":
    unittest.main()
