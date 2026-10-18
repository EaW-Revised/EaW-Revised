"""Public report accounting, literal text, determinism and freshness contracts."""
import copy
from contextlib import redirect_stderr
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/inventory"))
import tag_report as report


class TagReportTests(unittest.TestCase):
    def git(self, root, *args):
        return subprocess.run(["git", *args], cwd=root, check=True, capture_output=True, text=True)

    def test_pr_base_rejects_hand_edits_but_allows_stale_untouched_pages(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            self.git(root, "init")
            self.git(root, "config", "user.name", "Report Test")
            self.git(root, "config", "user.email", "report@example.invalid")
            self.git(root, "config", "core.autocrlf", "false")
            statuses = root / "statuses.json"
            registry = {"schema_version": 2, "evidence": {}, "rows": [
                {"area": "ai", "classes": ["Goals"], "tag": "Goal", "status": "deferred", "ticket": 737}]}
            statuses.write_text(json.dumps(registry))
            destination = root / "report pages"
            report.write_report(report.render(registry), destination)
            self.git(root, "add", ".")
            self.git(root, "commit", "-m", "base")
            base = self.git(root, "rev-parse", "HEAD").stdout.strip()
            before = {p.name: p.read_bytes() for p in destination.glob("*.md")}
            registry["rows"][0]["ticket"] = 738
            statuses.write_text(json.dumps(registry))
            self.git(root, "add", ".")
            self.git(root, "commit", "-m", "registry change")
            args = ["--statuses", str(statuses), "--out", str(destination), "--pr", "--base", base]
            with patch.object(report, "ROOT", root):
                self.assertEqual(report.main(args), 0)
                self.assertEqual({p.name: p.read_bytes() for p in destination.glob("*.md")}, before)
                (destination / "ai.md").write_text("hand edit\n")
                self.git(root, "add", ".")
                self.git(root, "commit", "-m", "hand edit")
                self.assertEqual(report.main(args), 1)
                report.write_report(report.render(registry), destination)
                self.git(root, "add", ".")
                self.git(root, "commit", "-m", "regenerate")
                self.assertEqual(report.main(args), 0)
                errors = io.StringIO()
                with redirect_stderr(errors), self.assertRaises(SystemExit) as raised:
                    report.main(args[:-1] + ["refs/heads/missing-base"])
                self.assertEqual(raised.exception.code, 2)
                self.assertIn("fetch the base ref", errors.getvalue())
                self.assertNotIn("Traceback", errors.getvalue())

    def test_pr_base_rejects_output_outside_repository_clearly(self):
        with tempfile.TemporaryDirectory() as temporary:
            errors = io.StringIO()
            with redirect_stderr(errors), self.assertRaises(SystemExit) as raised:
                report.main(["--pr", "--base", "HEAD", "--out", temporary])
            self.assertEqual(raised.exception.code, 2)
            self.assertIn("requires --out inside the repository", errors.getvalue())
            self.assertNotIn("Traceback", errors.getvalue())

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

    def test_pr_mode_passes_with_stale_report_and_batch_regenerates(self):
        pages = report.render(self.fixture())
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            statuses = root / "statuses.json"
            import json
            statuses.write_text(json.dumps({"schema_version": 2, "evidence": {}, "rows": [
                {"area": "ai", "classes": ["Goals"], "tag": "Goal", "status": "deferred", "ticket": 737}]}))
            out = root / "report"
            report.write_report(pages, out)
            before = {p.name: p.read_bytes() for p in out.glob("*.md")}
            args = ["--statuses", str(statuses), "--out", str(out)]
            self.assertEqual(report.main(args + ["--pr"]), 0)
            self.assertEqual({p.name: p.read_bytes() for p in out.glob("*.md")}, before)
            self.assertEqual(report.main(args + ["--check"]), 1)
            self.assertEqual(report.main(args + ["--batch"]), 0)
            self.assertEqual(report.main(args + ["--check"]), 0)
            self.assertEqual({p.name for p in out.glob("*.md")}, {"README.md", "ai.md"})


if __name__ == "__main__":
    unittest.main()
