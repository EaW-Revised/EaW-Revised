"""The XML literal gate (tools/inventory/xml_literal_check.py): what it flags, what it accepts, how the allowlist shrinks."""

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "inventory"))

import xml_literal_check as gate  # noqa: E402

# Merge-base size of the allowlist: fallback pin when git or origin/main is missing (the gate compares against the base).
# Raising it takes a reviewed growth_override in the allowlist plus an edit here.
ALLOWLIST_PIN = 28
_DIRS = []
TAGS = ["Volume_Saturation_Distance", "Laser_Beam_Z_Scale_Factor", "Encyclopedia_Delay"]


class Repo:
    """A throwaway tree with a registry of three tags and one source file."""

    def __init__(self, source: str, entries=None, max_entries=None):
        self.dir = tempfile.TemporaryDirectory()
        _DIRS.append(self.dir)
        self.root = pathlib.Path(self.dir.name)
        (self.root / "docs/tag-coverage").mkdir(parents=True)
        rows = [{"tag": tag, "classes": ["SpaceUnit"], "status": "todo"} for tag in TAGS + ["*"]]
        (self.root / gate.STATUSES_PATH).write_text(json.dumps({"schema_version": 2, "rows": rows}), encoding="utf-8")
        (self.root / "src").mkdir()
        (self.root / "src/a.cpp").write_text(source, encoding="utf-8")
        entries = entries or []
        self.allowlist = self.root / "allow.json"
        self.allowlist.write_text(
            json.dumps({"max_entries": len(entries) if max_entries is None else max_entries, "entries": entries}),
            encoding="utf-8",
        )

    def check(self, base=None):
        return gate.check(self.root, self.allowlist, base)


def entry(text, ticket=1, **extra):
    return {"file": "src/a.cpp", "text": text, "ticket": ticket, **extra}


def tearDownModule():
    for directory in _DIRS:
        directory.cleanup()


class LiteralGateTests(unittest.TestCase):
    def test_local_reader_component_counts_are_not_authored_values(self):
        for name, declaration in (("components", "std::string_view tag"), ("numbers", "const Node* node")):
            reader = f"std::vector<float> {name}({declaration}, std::size_t count) {{ if (values.size() != count) fail(); return values; }}\n"
            argument = '"Volume_Saturation_Distance"' if name == "components" else 'child(root, "Volume_Saturation_Distance")'
            call = f"auto values = {name}({argument}, 8);"
            self.assertEqual(Repo(reader + call + "\n").check(), [])
            self.assertEqual(len(Repo(reader + call + " double copy = 300.0;\n").check()), 1)
        self.assertEqual(len(Repo('auto value = components("Volume_Saturation_Distance", 300);\n').check()), 1)
        for reader in (
            '[[nodiscard]] std::vector<float> components(const char* tag, std::size_t count) const { if (values.size() != count) fail(); return values; }',
            'const auto components = [&](const char* tag, std::size_t count) -> std::optional<std::vector<float>> { return values.size() == count ? values : fail(); };',
        ):
            call = '\nauto values = components("Volume_Saturation_Distance", 4);'
            self.assertEqual(Repo(reader + call).check(), [])
            self.assertEqual(len(Repo(reader + call + ' double copy = 300.0;').check()), 1)

    def test_literal_tag_in_area_shard_is_still_flagged(self):
        repo = Repo("double d{300.0};  // Volume_Saturation_Distance\n")
        root = repo.root / gate.STATUSES_PATH
        registry = json.loads(root.read_text(encoding="utf-8"))
        shard = root.with_name("statuses-presentation.json")
        shard.write_text(json.dumps(registry), encoding="utf-8")
        root.write_text(json.dumps({"schema_version": 2, "rows": [], "shards": [shard.name]}), encoding="utf-8")
        self.assertEqual(gate.load_tags(repo.root), set(TAGS))
        problems = repo.check()
        self.assertEqual(len(problems), 1)
        self.assertIn("Volume_Saturation_Distance", problems[0])

    def test_tag_in_trailing_comment_with_literal_is_flagged(self):
        repo = Repo("double d{300.0};  // Volume_Saturation_Distance\n")
        problems = repo.check()
        self.assertEqual(len(problems), 1)
        self.assertIn("Volume_Saturation_Distance", problems[0])
        self.assertIn("src/a.cpp:1", problems[0])

    def test_tag_in_string_with_default_argument_is_flagged(self):
        self.assertEqual(len(Repo('auto v = number(node, "Encyclopedia_Delay", 750);\n').check()), 1)

    def test_comment_block_above_declaration_is_flagged(self):
        repo = Repo("// GAMECONSTANTS.XML Laser_Beam_Z_Scale_Factor\n// and more prose.\nconstexpr float z = 8.0F;\n")
        self.assertEqual(len(repo.check()), 1)

    def test_comment_above_does_not_leak_past_a_blank_line_or_one_declaration(self):
        repo = Repo("// Laser_Beam_Z_Scale_Factor\nconstexpr bool on = true;\nconstexpr float z = 8.0F;\n")
        self.assertEqual(repo.check(), [])

    def test_tag_without_literal_is_fine(self):
        self.assertEqual(Repo('auto v = read(node, "Encyclopedia_Delay");\n').check(), [])

    def test_trivial_rule_ids_subscripts_and_template_arguments_are_not_literals(self):
        source = (
            "std::array<double, 3> a{};  // Volume_Saturation_Distance\n"
            "auto x = table[5];  // Encyclopedia_Delay (BP-04, #822)\n"
            'auto y = read("Encyclopedia_Delay", 0);\n'
        )
        self.assertEqual(Repo(source).check(), [])

    def test_xml_fallback_marker_accepts_the_literal(self):
        load = 'if (!node.has("Volume_Saturation_Distance")) { log_warning("Volume_Saturation_Distance missing"); }\n'
        self.assertEqual(Repo("double d{300.0};  // xml-fallback: Volume_Saturation_Distance\n" + load).check(), [])
        above = "// xml-fallback: Encyclopedia_Delay\nconstexpr double d = 0.75;\n"
        above += 'if (!node.has("Encyclopedia_Delay")) { report_error("Encyclopedia_Delay missing"); }\n'
        self.assertEqual(Repo(above).check(), [])

    def test_xml_fallback_marker_needs_a_load_of_the_tag_and_a_diagnostic(self):
        backed = (
            "double saturation() {\n"
            "    double d{300.0};  // xml-fallback: Volume_Saturation_Distance\n"
            '    if (!node.has("Volume_Saturation_Distance")) {\n'
            '        log_warning("Volume_Saturation_Distance missing");\n'
            "    }\n"
            "    return d;\n"
            "}\n"
        )
        self.assertEqual(Repo(backed).check(), [])
        no_load = "double d{300.0};  // xml-fallback: Volume_Saturation_Distance\n"
        self.assertEqual(len(Repo(no_load).check()), 1)
        wrong_tag = backed.replace("// xml-fallback: Volume_Saturation_Distance", "// xml-fallback: Encyclopedia_Delay")
        self.assertEqual(len(Repo(wrong_tag).check()), 1)
        not_a_tag = (
            'double d = number(node, "Volume_Saturation_Distance", 300.0);  // xml-fallback: Anything\n'
            'if (!ok) { log_warning("Volume_Saturation_Distance missing"); }\n'
        )
        problems = Repo(not_a_tag).check()
        self.assertEqual(len(problems), 1)
        self.assertIn("not backed", problems[0])
        no_diagnostic = backed.replace('        log_warning("Volume_Saturation_Distance missing");\n', "")
        self.assertEqual(len(Repo(no_diagnostic).check()), 1)

    def test_explicit_loader_reference_accepts_direct_and_helper_diagnostics(self):
        marker = "double d{300.0}; // xml-fallback: Volume_Saturation_Distance; load: src/loader.cpp#load\n"
        direct = 'void load() { if (!node.has("Volume_Saturation_Distance")) log_warning("missing"); }\n'
        helper = ('void fallback() { report_error("missing"); }\n'
                  'void load() { if (!node.has("Volume_Saturation_Distance")) fallback(); }\n')
        for body in (direct, helper):
            repo = Repo(marker)
            (repo.root / "src/loader.cpp").write_text(body, encoding="utf-8")
            self.assertEqual(repo.check(), [])
        repo = Repo("// xml-fallback: Volume_Saturation_Distance; load: src/loader.cpp#load\ndouble d{300.0};\n")
        (repo.root / "src/loader.cpp").write_text(direct, encoding="utf-8")
        self.assertEqual(repo.check(), [])

    def test_nearby_window_rejects_issue_2596_unrelated_diagnostic(self):
        repo = Repo(
            'void load() {\n'
            '    double d{300.0}; // xml-fallback: Volume_Saturation_Distance\n'
            '    node.read("Volume_Saturation_Distance");\n'
            '    log_warning("unrelated");\n'
            '}\n'
        )
        problems = repo.check()
        self.assertEqual(len(problems), 1)
        self.assertIn('not backed', problems[0])

    def test_nearby_window_tracks_the_load_key_and_missing_branch(self):
        marker = 'double d{300.0}; // xml-fallback: Volume_Saturation_Distance\n'
        probes = [
            ('if (node.has("Volume_Saturation_Distance")) log_warning("present");', False),
            ('if (!node.has("Volume_Saturation_Distance")) log_warning("missing");', True),
            ('if (node.has("Volume_Saturation_Distance")) {} else log_warning("missing");', True),
            ('auto value = node.read("Volume_Saturation_Distance"); if (!value) log_warning("missing");', True),
            ('auto value = node.read("Volume_Saturation_Distance"); if (!other) log_warning("other missing");', False),
            ('if (!node.has("Volume_Saturation_Distance")) {} log_warning("unrelated");', False),
            ('if (!node.has("Volume_Saturation_Distance")) out.diagnostics.clear();', False),
            ('log_warning("Volume_Saturation_Distance is fine");', False),
            ('/* if (!node.has("Volume_Saturation_Distance")) log_warning("missing"); */', False),
            ('if (!node.has("Volume_Saturation_DistanceExtra")) log_warning("missing");', False),
        ]
        for body, backed in probes:
            with self.subTest(body=body):
                self.assertEqual(len(Repo(marker + body).check()), 0 if backed else 1)

    def test_nearby_window_accepts_local_helper_and_lambda_diagnostics(self):
        marker = 'double d{300.0}; // xml-fallback: Volume_Saturation_Distance\n'
        for body in (
            'void fallback() { report_error("missing"); }\n'
            'void load() { if (!node.has("Volume_Saturation_Distance")) fallback(); }\n',
            'auto fallback = [](const char* tag) { if (!node.has(tag)) log_warning("missing"); };\n'
            'fallback("Volume_Saturation_Distance");\n',
        ):
            with self.subTest(body=body):
                self.assertEqual(Repo(marker + body).check(), [])

    def test_nearby_window_keeps_its_twenty_line_limit(self):
        marker = 'double d{300.0}; // xml-fallback: Volume_Saturation_Distance\n'
        diagnostic = 'if (!node.has("Volume_Saturation_Distance")) log_warning("missing");\n'
        for padding, backed in ((gate.FALLBACK_WINDOW - 1, True), (gate.FALLBACK_WINDOW, False)):
            with self.subTest(padding=padding):
                self.assertEqual(len(Repo(marker + '\n' * padding + diagnostic).check()), 0 if backed else 1)

    def test_templated_loader_calls_preserve_the_missing_key_connection(self):
        marker = 'double d{300.0}; // xml-fallback: Volume_Saturation_Distance; load: src/loader.cpp#load\n'
        helper = ('template<int N, typename T>\n'
                  'T read(const char* tag) { auto value = node.last_child(tag); '
                  'if (!value) log_warning("missing"); return {}; }\n')
        for key, backed in (('Volume_Saturation_Distance', True), ('Encyclopedia_Delay', False)):
            with self.subTest(key=key):
                body = helper + 'void load() { read<1, double>("' + key + '"); }\n'
                repo = Repo(marker)
                (repo.root / 'src/loader.cpp').write_text(body, encoding='utf-8')
                self.assertEqual(len(repo.check()), 0 if backed else 1)
                self.assertEqual(len(Repo(marker.split('; load:')[0] + '\n' + body).check()), 0 if backed else 1)

    def test_every_shipped_fallback_marker_is_backed(self):
        tags = gate.load_tags(ROOT)
        checked = 0
        for root in gate.ROOTS:
            for path in (ROOT / root).rglob('*'):
                if path.suffix not in gate.SUFFIXES:
                    continue
                lines = path.read_text(encoding='utf-8').splitlines()
                for index, line in enumerate(lines):
                    marked = set(gate.MARKER.findall(line))
                    if not marked:
                        continue
                    reference = gate.LOAD_REFERENCE.search(line)
                    with self.subTest(path=path.relative_to(ROOT), line=index + 1):
                        self.assertTrue(gate.marker_is_backed(lines, index, marked, tags)
                                        or (reference is not None and gate.reference_is_backed(
                                            ROOT, reference.groups(), marked, tags)))
                    checked += 1
        self.assertGreater(checked, 0)

    def test_explicit_loader_reference_rejects_invalid_or_undiagnosed_loads(self):
        marker = "double d{300.0}; // xml-fallback: Volume_Saturation_Distance; load: src/loader.cpp#load\n"
        valid = 'void load() { if (!node.has("Volume_Saturation_Distance")) log_warning("missing"); }\n'
        cases = [
            (marker.replace("src/loader.cpp", "src/absent.cpp"), valid),
            (marker.replace("#load", "#absent"), valid),
            (marker, valid.replace("Volume_Saturation_Distance", "Encyclopedia_Delay")),
            (marker, 'void load() { node.read("Volume_Saturation_Distance"); }\n'),
            (marker, 'void load() { /* Volume_Saturation_Distance */ log_warning("missing"); }\n'),
            (marker, 'void load() { node.read("Volume_Saturation_DistanceExtra"); log_warning("missing"); }\n'),
            (marker, 'void load() { node.read("Volume_Saturation_Distance"); /* log_warning("missing"); */ }\n'),
            (marker, 'void fallback() {}\nvoid load() { node.read("Volume_Saturation_Distance"); fallback(); }\n'),
            (marker, 'void other() { log_warning("unrelated"); }\nvoid load() { node.read("Volume_Saturation_Distance"); }\n'),
            (marker.replace("src/loader.cpp", "src/../loader.cpp"), valid),
        ]
        for source, body in cases:
            with self.subTest(source=source, body=body):
                repo = Repo(source)
                (repo.root / "src/loader.cpp").write_text(body, encoding="utf-8")
                self.assertEqual(len(repo.check()), 1)

        # Review probes A-E: a diagnostic elsewhere in the function is insufficient.
        probes = [
            ('A', 'void load() { node.read("Volume_Saturation_Distance"); log_warning("unrelated"); }', 1),
            ('B', 'void load() { log_warning("Volume_Saturation_Distance is fine"); }', 1),
            ('C', 'void warn_other() { log_warning("x"); } void load() { node.read("Volume_Saturation_Distance"); warn_other(); }', 1),
            ('D', 'void load() { node.read("Volume_Saturation_Distance"); out.diagnostics.clear(); }', 1),
            ('E', valid, 0),
        ]
        for name, body, expected in probes:
            with self.subTest(probe=name):
                repo = Repo(marker)
                (repo.root / "src/loader.cpp").write_text(body, encoding="utf-8")
                self.assertEqual(len(repo.check()), expected)

    def test_diagnostic_pattern_matches_calls_only(self):
        for text in ('out.diagnostics.clear()', 'out.diagnostics', 'diagnostic.message', 'std::cerr'):
            with self.subTest(text=text):
                self.assertIsNone(gate._DIAGNOSTIC.search(text))
        for text in ('log_warning("x")', 'report_error("x")', 'out.diagnostics.push_back(d)',
                     'out.diagnostics.emplace_back(d)'):
            with self.subTest(text=text):
                self.assertIsNotNone(gate._DIAGNOSTIC.search(text))

    def test_shipped_loader_references_are_backed_for_every_marked_tag(self):
        tags = gate.load_tags(ROOT)
        checked = set()
        for path in (ROOT / "include/eawr/presentation").rglob('*.hpp'):
            for line in path.read_text(encoding="utf-8").splitlines():
                reference = gate.LOAD_REFERENCE.search(line)
                if reference:
                    marked = set(gate.MARKER.findall(line))
                    with self.subTest(reference=reference.groups(), marked=marked):
                        self.assertTrue(gate.reference_is_backed(ROOT, reference.groups(), marked, tags))
                    checked.add(reference.group(2))
        self.assertEqual(checked, {'fog_looks', 'battle_message_looks', 'shell_bar', 'minimap_settings'})

    def test_explicit_reference_tracks_the_missing_arm_and_load_result(self):
        marker = 'double d{300.0}; // xml-fallback: Volume_Saturation_Distance; load: src/loader.cpp#load\n'
        probes = [
            ('if (node.has("Volume_Saturation_Distance")) log_warning("present");', False),
            ('if (node.has("Volume_Saturation_Distance")) {} else log_warning("missing");', True),
            ('auto value = node.read("Volume_Saturation_Distance"); if (!value) log_warning("missing");', True),
            ('auto value = node.read("Volume_Saturation_Distance"); if (!other) log_warning("other missing");', False),
            ('if (!node.has("Volume_Saturation_Distance")) {} log_warning("unrelated");', False),
            ('if (!node.has("Volume_Saturation_Distance")) out.diagnostics.clear();', False),
        ]
        for body, backed in probes:
            with self.subTest(body=body):
                repo = Repo(marker)
                (repo.root / 'src/loader.cpp').write_text('void load() { ' + body + ' }', encoding='utf-8')
                self.assertEqual(len(repo.check()), 0 if backed else 1)

    def test_block_comment_at_line_start_is_scanned(self):
        self.assertEqual(len(Repo("/* Volume_Saturation_Distance */ double d{300.0};\n").check()), 1)
        self.assertEqual(Repo("/* Volume_Saturation_Distance is read elsewhere */\n").check(), [])

    def test_allowlist_growth_needs_a_reviewed_override(self):
        text = "double d{300.0}; // Volume_Saturation_Distance"
        other = "double e{500.0}; // Encyclopedia_Delay"
        repo = Repo(text + "\n" + other + "\n", [entry(text), entry(other)], max_entries=2)
        base = [entry(text)]
        self.assertTrue(any("grew" in p for p in repo.check(base)))
        allow = json.loads(repo.allowlist.read_text(encoding="utf-8"))
        allow["growth_override"] = {"ticket": 7, "reason": "reviewed"}
        repo.allowlist.write_text(json.dumps(allow), encoding="utf-8")
        self.assertEqual(repo.check(base), [])
        self.assertTrue(any("did not grow" in p for p in repo.check([entry(text), entry(other)])))
        allow["growth_override"] = {"ticket": "x"}
        repo.allowlist.write_text(json.dumps(allow), encoding="utf-8")
        self.assertTrue(any("growth_override needs" in p for p in repo.check(base)))

    def test_base_ref_reads_the_allowlist_at_the_merge_base(self):
        text = 'double d{300.0}; // Volume_Saturation_Distance'
        repo = Repo(text + '\n', [entry(text)])
        self.assertIsNone(gate.base_entries_from_git(repo.root, "main"))  # not a git repository

        def git(*args):
            subprocess.run(["git", "-C", str(repo.root), *args], check=True, capture_output=True)

        git("init", "-q", "-b", "main")
        (repo.root / "config").mkdir()
        allow = {"max_entries": 1, "entries": [entry(text)],
                 "growth_override": {"ticket": 7, "reason": "reviewed"}}
        (repo.root / gate.ALLOWLIST_PATH).write_text(json.dumps(allow), encoding="utf-8")
        git("add", gate.ALLOWLIST_PATH)
        git("-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "base")
        git("checkout", "-qb", "pr")
        self.assertEqual(gate.base_entries_from_git(repo.root, "main"), allow)
        repo.allowlist.write_text(json.dumps(allow), encoding='utf-8')
        self.assertEqual(repo.check(gate.base_entries_from_git(repo.root, 'main')), [])

    def test_allowlist_rename_preserves_tag_divisor_and_metadata(self):
        old = 'std::tuple{"Encyclopedia_Delay", &result.delay, 1000.0},'
        new = old.replace('result.delay', 'timing.delay')
        metadata = {"ticket": "false-positive", "reason": "milliseconds-to-seconds divisor"}
        repo = Repo(new + "\n", [entry(new, **metadata)])
        self.assertEqual(repo.check([entry(old, **metadata)]), [])
        for changed in (
            entry(old.replace('1000.0', '500.0'), **metadata),
            entry(old.replace('Encyclopedia_Delay', 'Volume_Saturation_Distance'), **metadata),
            entry(old, ticket=7, reason=metadata['reason']),
            entry(old, ticket='false-positive', reason='different exception'),
            {**entry(old, **metadata), 'file': 'src/b.cpp'},
        ):
            with self.subTest(base=changed):
                self.assertTrue(any('grew' in p for p in repo.check([changed])))

    def test_allowlist_rename_cannot_cover_an_additional_entry(self):
        old = 'double old_delay{750.0}; // Encyclopedia_Delay'
        new = old.replace('old_delay', 'new_delay')
        extra = old.replace('old_delay', 'extra_delay')
        repo = Repo(new + '\n' + extra + '\n', [entry(new), entry(extra)])
        self.assertEqual(sum('grew' in p for p in repo.check([entry(old)])), 1)
        # An entry still present in the current list cannot also fund a rename.
        repo = Repo(old + '\n' + new + '\n', [entry(old), entry(new)])
        self.assertEqual(sum('grew' in p for p in repo.check([entry(old)])), 1)

    def test_allowlist_rename_and_shrink_pass_together(self):
        old = 'double old_delay{750.0}; // Encyclopedia_Delay'
        new = old.replace('old_delay', 'new_delay')
        removed = 'double d{300.0}; // Volume_Saturation_Distance'
        repo = Repo(new + '\n', [entry(new)])
        self.assertEqual(repo.check([entry(old), entry(removed)]), [])

    def test_inherited_override_passes_but_cannot_approve_new_growth(self):
        text = 'double d{300.0}; // Volume_Saturation_Distance'
        other = 'double e{500.0}; // Encyclopedia_Delay'
        override = {"ticket": 7, "reason": "reviewed"}
        base = {"entries": [entry(text)], "growth_override": override}
        for source, entries, grows in (
            (text, [entry(text)], False),
            (text.replace('double d', 'double renamed'),
             [entry(text.replace('double d', 'double renamed'))], False),
            ('int x = 5;', [], False),
            (text + '\n' + other, [entry(text), entry(other)], True),
        ):
            with self.subTest(entries=entries):
                repo = Repo(source + '\n', entries)
                allow = json.loads(repo.allowlist.read_text(encoding='utf-8'))
                allow['growth_override'] = override
                repo.allowlist.write_text(json.dumps(allow), encoding='utf-8')
                base_path = repo.root / 'base.json'
                base_path.write_text(json.dumps(base), encoding='utf-8')
                problems = repo.check(base_path)
                if grows:
                    self.assertTrue(any('grew' in p for p in problems))
                    allow['growth_override'] = {"ticket": 8, "reason": "new review"}
                    repo.allowlist.write_text(json.dumps(allow), encoding='utf-8')
                    self.assertEqual(repo.check(base_path), [])
                else:
                    self.assertEqual(problems, [])

    def test_new_override_is_unused_for_a_rename(self):
        old = 'double d{300.0}; // Volume_Saturation_Distance'
        new = old.replace('double d', 'double renamed')
        repo = Repo(new + '\n', [entry(new)])
        allow = json.loads(repo.allowlist.read_text(encoding='utf-8'))
        allow['growth_override'] = {"ticket": 7, "reason": "reviewed"}
        repo.allowlist.write_text(json.dumps(allow), encoding='utf-8')
        self.assertTrue(any('did not grow' in p for p in repo.check([entry(old)])))

    def test_the_committed_allowlist_does_not_exceed_its_pin(self):
        allow = json.loads((ROOT / gate.ALLOWLIST_PATH).read_text(encoding="utf-8"))
        self.assertLessEqual(allow["max_entries"], ALLOWLIST_PIN)
        self.assertLessEqual(len(allow["entries"]), ALLOWLIST_PIN)

    def test_allowlisted_finding_passes_and_text_change_fails(self):
        text = "double d{300.0}; // Volume_Saturation_Distance"
        self.assertEqual(Repo(text + "\n", [entry(text)]).check(), [])
        problems = Repo("double d{301.0}; // Volume_Saturation_Distance\n", [entry(text)]).check()
        self.assertTrue(any("stale allowlist entry" in p for p in problems))
        self.assertTrue(any("names XML tag" in p for p in problems))

    def test_stale_entry_fails(self):
        problems = Repo("int x = 5;\n", [entry("gone")]).check()
        self.assertEqual(len(problems), 1)
        self.assertIn("stale", problems[0])

    def test_allowlist_may_not_exceed_max_entries(self):
        text = "double d{300.0}; // Volume_Saturation_Distance"
        problems = Repo(text + "\n", [entry(text)], max_entries=0).check()
        self.assertTrue(any("max_entries" in p for p in problems))

    def test_entries_need_a_ticket_or_a_reasoned_false_positive(self):
        text = "double d{300.0}; // Volume_Saturation_Distance"
        self.assertTrue(Repo(text + "\n", [entry(text, ticket=None)]).check())
        self.assertTrue(Repo(text + "\n", [entry(text, ticket="false-positive")]).check())
        self.assertEqual(Repo(text + "\n", [entry(text, ticket="false-positive", reason="engine formula")]).check(), [])

    def test_allowlist_may_not_grow_against_a_base(self):
        text = "double d{300.0}; // Volume_Saturation_Distance"
        repo = Repo(text + "\n", [entry(text)])
        base = repo.root / "base.json"
        base.write_text(json.dumps({"max_entries": 0, "entries": []}), encoding="utf-8")
        self.assertTrue(any("grew" in p for p in repo.check(base)))
        base.write_text(json.dumps({"max_entries": 1, "entries": [entry(text)]}), encoding="utf-8")
        self.assertEqual(repo.check(base), [])

    def test_tests_and_vendored_code_are_not_scanned(self):
        repo = Repo("int x = 5;\n")
        for part in ("src/tests", "src/third_party"):
            (repo.root / part).mkdir(parents=True)
            (repo.root / part / "b.cpp").write_text("double d{300.0};  // Volume_Saturation_Distance\n", encoding="utf-8")
        self.assertEqual(repo.check(), [])


class RepoStateTests(unittest.TestCase):
    def test_the_repository_passes_its_own_gate(self):
        self.assertEqual(gate.check(ROOT, ROOT / gate.ALLOWLIST_PATH, None), [])


if __name__ == "__main__":
    unittest.main()
