"""Public Markdown policy on code, links, tracking groups and export exclusions."""

from pathlib import Path
import json
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/publish_scan"))
import doc_refs


class ReferenceTests(unittest.TestCase):
    def test_tracking_groups_are_explicit_and_bare_numbers_keep_offsets(self):
        text = "Work (legacy #540, #984).\nBare #7 and #1234567."
        self.assertEqual([(text.count('\n', 0, offset) + 1, number)
                          for offset, number in doc_refs.bare_refs(text)], [(2, '#7'), (2, '#1234567')])
        for text in ["legacy #7", "(#7 legacy)", "(legacy #7 and #8)", "(legacy #7, #8 pending)"]:
            with self.subTest(text=text):
                self.assertTrue(doc_refs.bare_refs(text))

    def test_fences_inline_code_and_unclosed_fence(self):
        text = ("`#1` ``literal `#2` here`` and `multi\n#3`\n"
                "~~~~python\n#4\n~~~\n#5\n~~~~\n"
                "````\n```\n#6\n````\n#7\n```\n#8\n")
        self.assertEqual([number for _, number in doc_refs.bare_refs(text)], ["#7"])

    def test_unmatched_inline_backtick_does_not_hide_prose(self):
        self.assertEqual([number for _, number in doc_refs.bare_refs("a ` then #7")], ["#7"])

    def test_slash_separated_numbers_all_need_descriptions(self):
        self.assertEqual([number for _, number in doc_refs.bare_refs("#7/#8/#9")], ["#7", "#8", "#9"])

    def test_rule_ids_colours_shas_versions_and_link_fragments(self):
        text = ("WPR-22 v1.2 123abc #ff0000 #12ab34 &#123; "
                "[anchor](topic.md#123) https://example.test/a#123\n"
                "[ref]: topic.md#123\n[issue #7](https://example.test/issues/7)")
        self.assertEqual([number for _, number in doc_refs.bare_refs(text)], ["#7"])

    def test_rewrite_changes_only_prose_legacy_groups(self):
        text = "Work (legacy #7, #8). `keep (legacy #9)`\n~~~\n(legacy #10)\n~~~\n"
        rewritten, count = doc_refs.rewrite_legacy(text, "EAWR")
        self.assertEqual(count, 2)
        self.assertEqual(rewritten, text.replace("(legacy #7, #8)", "(legacy EAWR-7, EAWR-8)"))

    def test_public_tree_check_reports_file_and_line(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'a.md').write_text("# Title\nWork #7\n(legacy #8)", encoding='utf-8')
            self.assertEqual(len(doc_refs.scan(root)), 1)
            self.assertIn("a.md:2:", doc_refs.scan(root)[0])

    def test_private_exclusions_and_stripped_blocks_keep_source_line_numbers(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = root / 'tools/oss/export.json'
            config.parent.mkdir(parents=True)
            config.write_text(json.dumps({'exclude': [{'path': 'private/'}],
                                          'always_strip_blocks': ['maintainer']}), encoding='utf-8')
            (root / 'private').mkdir()
            (root / 'private/a.md').write_text('#1', encoding='utf-8')
            (root / 'a.md').write_text('<!-- eawr-export: maintainer begin -->\n#2\n'
                                      '<!-- eawr-export: maintainer end -->\n#3', encoding='utf-8')
            hits = doc_refs.scan(root)
            self.assertEqual(len(hits), 1)
            self.assertIn('a.md:4:', hits[0])

    def test_overlay_replaces_private_doc_and_excluded_index_entries_are_hidden(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = root / 'tools/oss/export.json'
            config.parent.mkdir(parents=True)
            config.write_text(json.dumps({'exclude': [{'path': 'tools/oss/'}, {'path': 'private/'}],
                                          'overlay': 'tools/oss/public'}), encoding='utf-8')
            overlay = root / 'tools/oss/public'
            overlay.mkdir()
            (root / 'README.md').write_text('Private #1', encoding='utf-8')
            (overlay / 'README.md').write_text('- [private #2](private/a.md)\nPublic #3', encoding='utf-8')
            hits = doc_refs.scan(root)
            self.assertEqual(len(hits), 1)
            self.assertIn('tools/oss/public/README.md:2:', hits[0])

    def test_decisions_and_rules_need_public_definitions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            docs = root / 'docs'
            (docs / 'behaviour').mkdir(parents=True)
            (docs / 'architecture-decisions.md').write_text(
                '## ADR-011: Godot presentation\nStatus: accepted.\n', encoding='utf-8')
            (docs / 'behaviour/rules.md').write_text(
                '- **WSU-10** Picking.\n| Rule | Behaviour | Source |\n|---|---|---|\n'
                '| PU-01 | Credits | data |\n'
                '- WBP-01. Presentation.\n- WU-43 (project). Overlay.\n', encoding='utf-8')
            (root / 'README.md').write_text(
                'ADR-011 WSU-10 PU-01 WBP-01 WU-43\nADR-012 SK-99 WSU-99', encoding='utf-8')
            hits = doc_refs.scan(root)
            self.assertEqual(len(hits), 3)
            for number in ('ADR-012', 'SK-99', 'WSU-99'):
                self.assertTrue(any('README.md:2: ' + number in hit for hit in hits))

    def test_remake_summary_row_cannot_replace_rule_definition(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            rules = root / 'docs/behaviour/rules.md'
            rules.parent.mkdir(parents=True)
            rules.write_text('| Rule | Ours | Verdict |\n|---|---|---|\n'
                             '| WAB-12 | Countdown | Same |', encoding='utf-8')
            self.assertEqual(len(doc_refs.scan(root)), 1)

    def test_prose_mentions_and_summary_tables_do_not_define_decisions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'architecture-decisions.md').write_text(
                'ADR-011 selects Godot.\n| ADR-012 | Proposed |\n## ADR-013\n', encoding='utf-8')
            self.assertEqual(len(doc_refs.scan(root)), 3)

    def test_private_definition_cannot_resolve_public_reference(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = root / 'tools/oss/export.json'
            config.parent.mkdir(parents=True)
            config.write_text(json.dumps({'always_strip_blocks': ['private']}), encoding='utf-8')
            docs = root / 'docs/behaviour'
            docs.mkdir(parents=True)
            (docs / 'rules.md').write_text(
                '<!-- eawr-export: private begin -->\n- **WSU-10** Picking.\n'
                '<!-- eawr-export: private end -->\nWSU-10', encoding='utf-8')
            self.assertEqual(len(doc_refs.scan(root)), 1)
            self.assertIn('rules.md:4: WSU-10', doc_refs.scan(root)[0])

    def test_code_paths_and_private_evidence_are_not_public_rule_references(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'README.md').write_text(
                '`ADR-999`\n```\nWSU-99\n```\n'
                '[capture contract](contracts/P1-12-review.md)\n'
                'Evidence AU-99 ESU-99 stays private. docs/P0-04-contract.md', encoding='utf-8')
            self.assertEqual(doc_refs.scan(root), [])

    def test_work_package_definitions_stay_in_plan_and_other_prose_describes_work(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            plan = root / 'plan'
            plan.mkdir()
            (plan / 'backlog.md').write_text(
                '- [x] **P0-04 Deterministic simulation harness**\n', encoding='utf-8')
            (root / 'README.md').write_text('P0-04 supplies a world.\nP2-99', encoding='utf-8')
            hits = doc_refs.scan(root)
            self.assertEqual(len(hits), 2)
            self.assertIn('describe the work', hits[0])
            self.assertIn('no public', hits[1])

    def test_decision_link_checks_fragment_and_overlay_public_location(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            docs = root / 'docs'
            docs.mkdir()
            (docs / 'architecture-decisions.md').write_text(
                '## ADR-011: Godot presentation\nAccepted.\n', encoding='utf-8')
            config = root / 'tools/oss/export.json'
            config.parent.mkdir(parents=True)
            config.write_text(json.dumps({'overlay': 'tools/oss/public',
                                          'exclude': [{'path': 'tools/oss/'}]}), encoding='utf-8')
            overlay = root / 'tools/oss/public'
            overlay.mkdir()
            readme = overlay / 'README.md'
            readme.write_text('[Godot presentation](docs/architecture-decisions.md#adr-011-godot-presentation)',
                              encoding='utf-8')
            self.assertEqual(doc_refs.scan(root), [])
            readme.write_text('[Godot presentation](docs/architecture-decisions.md#adr-011-gone)',
                              encoding='utf-8')
            self.assertIn('unresolved decision anchor', doc_refs.scan(root)[0])
            readme.write_text('`[example](docs/architecture-decisions.md#adr-099-gone)`', encoding='utf-8')
            self.assertEqual(doc_refs.scan(root), [])


if __name__ == '__main__':
    unittest.main()
