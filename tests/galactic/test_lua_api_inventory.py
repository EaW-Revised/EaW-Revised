"""Stock-free checks for the GC inventory's evidence and uncertainty rules."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.inventory.galactic_lua_api import generate, dependencies


class GalacticInventoryTests(unittest.TestCase):
    def fixture(self, sources):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        rows = []
        for path, text in sources.items():
            data = text.encode()
            target = root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            rows.append({"logical_path": path, "sha256": hashlib.sha256(data).hexdigest(), "source_id": "fixture"})
        return root, {"manifest_id": "fixture", "profiles": [{"profile": "foc", "file_outcomes": rows}]}

    def test_closure_scopes_helpers_and_reports_unknown_calls(self):
        root, manifest = self.fixture({
            "data/scripts/story/a.lua": 'require("Shared")\nfunction OnlyA() end\nFind_Player("x")\n',
            "data/scripts/story/b.lua": 'require("Shared")\nOnlyA()\n',
            "data/scripts/library/shared.lua": 'function SharedHelper() end\nGetCurrentTime()\n',
            "data/scripts/ai/spacemode/unrelated.lua": 'function OnlyA() end\n',
        })
        registrations = {"entries": [{"exposed_name": "Find_Player", "call_kind": "global"}]}
        result = generate(root, manifest, registrations)
        rows = {row["symbol"]: row for row in result["rows"]}
        self.assertEqual(result["counts"]["scripts"], 3)
        self.assertEqual(rows["Find_Player"]["status"], "missing")
        self.assertEqual(rows["GetCurrentTime"]["status"], "bound")
        self.assertEqual(rows["OnlyA"]["status"], "unresolved")
        self.assertNotIn(str(root), json.dumps(result))
        self.assertEqual(result, generate(root, manifest, registrations))

    def test_pinned_winning_bytes_are_required(self):
        root, manifest = self.fixture({"data/scripts/story/a.lua": 'Find_Player("x")'})
        (root / "data/scripts/story/a.lua").write_text("changed")
        with self.assertRaisesRegex(ValueError, "differs from the committed manifest"):
            generate(root, manifest, {"entries": []})

    def test_unsynchronized_random_is_forbidden_not_an_adapter_gap(self):
        root, manifest = self.fixture({"data/scripts/story/a.lua": 'GameRandom.Free_Random(1, 2)'})
        registrations = {"entries": [{"exposed_name": "Free_Random", "call_kind": "member"}]}
        result = generate(root, manifest, registrations)
        self.assertEqual(result["rows"][0]["status"], "forbidden")
        self.assertEqual(result["rows"][0]["target"], "docs/lua-sandbox.md")

    def test_dynamic_calls_and_ambiguous_require_stay_visible(self):
        root, manifest = self.fixture({
            "data/scripts/story/a.lua": 'require("Duplicate")\nlocal f = {}; f[1]()\n',
            "data/scripts/library/duplicate.lua": '',
            "data/scripts/ai/spacemode/duplicate.lua": '',
        })
        result = generate(root, manifest, {"entries": []})
        self.assertTrue(any("ambiguous" in row["reason"] for row in result["unresolved"]))
        self.assertTrue(any("call" in row["reason"] for row in result["unresolved"]))
        self.assertEqual(result["counts"]["scripts"], 1)

    def test_require_lexing_ignores_comments_and_preserves_dynamic_edges(self):
        modules, dynamic = dependencies(b'-- require("Wrong")\nrequire [[Right]]\nrequire(variable)\nrequire("prefix" .. variable)')
        self.assertEqual(modules, {"right"})
        self.assertEqual(dynamic, [3, 4])


if __name__ == "__main__":
    unittest.main()
