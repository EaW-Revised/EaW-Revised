"""Public report accounting, literal text, determinism and freshness contracts."""
import copy
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/inventory"))
import tag_report as report


class TagReportTests(unittest.TestCase):
    def fixture(self):
        return {"rows": [
            {"area": "combat", "classes": ["UniqueUnit", "SpaceUnit"], "tag": "Max_Speed", "status": "partial",
             "missing_types": ["ship"], "ticket": 650, "note": "a | b\n[next](https://example.invalid)",
             "applied": [{"code": "src/units/unit_motion.cpp#field", "types": ["craft"], "rules": ["ESU-16"]}]},
            {"area": "combat", "classes": ["GroundUnit"], "tag": "Speed", "status": "land-or-galactic"},
            {"area": "ai", "classes": ["Goals"], "tag": "Goal", "status": "deferred", "ticket": 737},
        ]}

    def test_counts_and_scope(self):
        pages = report.render(self.fixture())
        self.assertIn("| partial | 2 | 2 |", pages["README.md"])
        self.assertIn("| land-or-galactic | 0 | 1 |", pages["README.md"])
        self.assertIn("| **Total** | **3** | **4** |", pages["README.md"])
        self.assertIn("| [combat](combat.md) | 2 | 3 |", pages["README.md"])

    def test_preserves_details_as_literal_public_text(self):
        text = report.render(self.fixture())["combat.md"]
        self.assertIn("## SpaceUnit, UniqueUnit", text)
        self.assertIn("craft (src/units/unit_motion.cpp); missing: ship", text)
        self.assertIn("| ESU-16 | 650 | src/units/unit_motion.cpp |", text)
        self.assertNotIn("#field", text)
        self.assertIn("a &#124; b<br>&#91;next&#93;", text)
        self.assertNotIn("[next]", text)

    def test_input_order_does_not_change_output(self):
        registry = self.fixture()
        reversed_registry = copy.deepcopy(registry)
        reversed_registry["rows"].reverse()
        reversed_registry["rows"][-1]["classes"].reverse()
        self.assertEqual(report.render(registry), report.render(reversed_registry))

    def test_gate_detects_missing_changed_extra_and_crlf_pages(self):
        pages = report.render(self.fixture())
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary)
            self.assertEqual(report.check_report(pages, destination), sorted(pages))
            report.write_report(pages, destination)
            self.assertEqual(report.check_report(pages, destination), [])
            (destination / "combat.md").write_bytes(pages["combat.md"].replace("\n", "\r\n").encode())
            (destination / "ai.md").unlink()
            (destination / "obsolete.md").write_text("stale")
            self.assertEqual(report.check_report(pages, destination), ["ai.md", "combat.md", "obsolete.md"])
            report.write_report(pages, destination)
            self.assertEqual(report.check_report(pages, destination), [])


if __name__ == "__main__":
    unittest.main()
