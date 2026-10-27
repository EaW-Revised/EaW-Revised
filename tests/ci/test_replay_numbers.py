"""The replay number registry agrees with docs/replay-format.md and the code constants."""

from pathlib import Path
import json
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import check_replay_numbers as checker

FILES = (checker.REGISTRY, checker.DOC, checker.REPLAY_HEADER, checker.OPCODE_HEADER)


def copy_tree(destination):
    for relative in FILES:
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(ROOT / relative, target)
    for directory in checker.BLOCK_DIRS:
        shutil.copytree(ROOT / directory, destination / directory, dirs_exist_ok=True)


class ReplayNumbersTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        copy_tree(self.root)

    def edit_registry(self, edit):
        path = self.root / checker.REGISTRY
        registry = json.loads(path.read_text(encoding="utf-8"))
        edit(registry)
        path.write_text(json.dumps(registry), encoding="utf-8")

    def test_repository_is_consistent(self):
        self.assertEqual(checker.check(ROOT), [])

    def test_unregistered_code_opcode_is_reported(self):
        header = self.root / checker.OPCODE_HEADER
        text = header.read_text(encoding="utf-8")
        header.write_text(text + "\ninline constexpr std::uint8_t opcode_probe = 99;\n", encoding="utf-8")
        self.assertTrue(any("opcode_probe = 99" in problem for problem in checker.check(self.root)))

    def test_wrong_registry_value_is_reported(self):
        def edit(registry):
            registry["tactical_replay"]["header_extensions"][0]["tag"] = 40
        self.edit_registry(edit)
        problems = checker.check(self.root)
        self.assertTrue(any("registry 40" in problem for problem in problems))

    def test_documented_flag_must_match_doc(self):
        def edit(registry):
            for row in registry["tactical_replay"]["opcodes"]:
                if row["value"] == 31:
                    row["documented"] = True
        self.edit_registry(edit)
        self.assertEqual([problem for problem in checker.check(self.root) if "opcode 31" in problem],
                         ["opcode 31: documented=True, the doc table says False"])

    def test_unregistered_state_block_is_reported(self):
        header = self.root / "src/sim/tactical/session_encoding.hpp"
        text = header.read_text(encoding="utf-8")
        header.write_text(text + "\ninline constexpr std::array<std::uint8_t, 4> probe_tag{'P', 'R', 'B', 'E'};\n",
                          encoding="utf-8")
        self.assertTrue(any("state block PRBE" in problem for problem in checker.check(self.root)))

    def test_doc_only_opcode_row_is_reported(self):
        doc = self.root / checker.DOC
        text = doc.read_text(encoding="utf-8")
        marker = "| 26 | Reinforce with explicit facing"
        self.assertIn(marker, text)
        doc.write_text(text.replace(marker, "| 40 | Probe | none | 32 |\n" + marker), encoding="utf-8")
        self.assertIn("opcode 40 is in the doc's command table but not in the registry", checker.check(self.root))

    def test_doc_only_extension_tag_is_reported(self):
        doc = self.root / checker.DOC
        doc.write_text(doc.read_text(encoding="utf-8") + "\nTag 40 is a probe.\n", encoding="utf-8")
        self.assertIn("header extension tag 40 is mentioned in the doc but not in the registry", checker.check(self.root))

    def test_absent_tag_phrase_is_not_a_mention(self):
        self.assertIsNone(__import__("re").search(checker.TAG_MENTION.format(6), "Absent tag 6 preserves"))
        self.assertIsNone(__import__("re").search(checker.TAG_MENTION.format(6), "absent tag 6 preserves"))

    def test_literal_max_opcode_is_still_checked(self):
        header = self.root / checker.OPCODE_HEADER
        text = header.read_text(encoding="utf-8")
        header.write_text(text.replace("max_command_opcode = opcode_battle_control", "max_command_opcode = 40"), encoding="utf-8")
        self.assertTrue(any("max_command_opcode is 40" in problem for problem in checker.check(self.root)))


if __name__ == "__main__":
    unittest.main()
