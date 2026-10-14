"""The publish scan (tools/publish_scan) on small synthetic trees: what it flags and what it lets through."""

from __future__ import annotations

import copy
import pathlib
import shutil
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "publish_scan"))
import publish_scan  # noqa: E402

CONFIG = publish_scan.load_config()


def write(root: pathlib.Path, rel: str, data: str | bytes) -> pathlib.Path:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(data, bytes):
        path.write_bytes(data)
    else:
        path.write_text(data, encoding="utf-8", newline="")
    return path


def tree(temporary: str) -> pathlib.Path:
    """A minimal clean tree: a README and the tree's own clean-room checker."""
    root = pathlib.Path(temporary)
    write(root, "README.md", "# A project\n")
    (root / "tools").mkdir()
    shutil.copyfile(ROOT / "tools" / "cleanroom_check.py", root / "tools" / "cleanroom_check.py")
    return root


class PublishScanTests(unittest.TestCase):
    def test_a_clean_tree_passes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            write(root, "src/a.cpp", "// SDK 10.0.26200.0, version 1.2.3.4, 256.1.1.1\nint m_count = 0;\n")
            results = publish_scan.scan(root, CONFIG)
            self.assertEqual({name: hits for name, hits in results.items() if hits}, {})
            self.assertEqual(publish_scan.main(["--root", str(root)]), 0)

    def test_security_hits(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            # Made-up values of each shape, joined so this file itself stays clean.
            write(root, "a.md", "ssh 10.0." + "0.20\nC:/Users/" + "someone/x\n/home/" + "someone/y\nat 0x14"
                  + "0001234\nFUN_" + "00401000\nkey id_ed25519_" + "build\nbox." + "lan\n")
            hits = publish_scan.security_scan(root, CONFIG, publish_scan.tree_files(root))
            self.assertEqual(sorted(hit.split(": ")[1] for hit in hits),
                             ["binary-address", "decompiler-name", "home-path", "lan-host", "private-ipv4",
                              "ssh-key-name", "user-path"])

    def test_extra_rules_and_terms(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            write(root, "a.md", "runs on HostX and in /srv/private\n")
            hits = publish_scan.security_scan(root, CONFIG, publish_scan.tree_files(root),
                                              {"private-dir": r"/srv/private"}, ["hostx"])
            self.assertEqual(sorted(hit.split(": ")[1] for hit in hits), ["private-dir", "private-name"])

    def test_review_dotted_prefix_hosts_remain_visible_in_allowed_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            for name, text in (
                ('apps/viewer/src/map_mode_ready.cpp', '// reach gw.options.' + 'lan.'),
                ('apps/viewer/src/live_session_view.cpp', 'connect(gw.state.' + 'lan, 1);'),
                ('apps/viewer/src/map_mode_ready.cpp', 'auto x = options.' + 'lan; // gw.options.' + 'lan.')):
                with self.subTest(source=name, line=text):
                    path = write(root, name, text + '\n')
                    hits = publish_scan.security_scan(root, CONFIG, [path])
                    self.assertEqual(len(hits), text.count('.' + 'lan'), hits)
                    self.assertIn('lan-host', hits[0])

    def test_review_private_names_keep_camel_glued_and_filename_matches(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            token = 'Thom' + 'Ernst'
            write(root, 'docs/' + token + 'Notes.md', token + 'Laptop\n')
            hits = publish_scan.security_scan(root, CONFIG, publish_scan.tree_files(root), extra_terms=[token])
            self.assertEqual(len(hits), 2, hits)
            self.assertTrue(all('private-name' in hit for hit in hits))

    def test_review_home_paths_keep_file_uris_and_nested_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            write(root, 'a.md', 'see file:///home/' + 'alice/notes\n'
                  + 'copied from /srv/backup/home/' + 'alice/notes\n')
            hits = publish_scan.security_scan(root, CONFIG, publish_scan.tree_files(root))
            self.assertEqual(len(hits), 2, hits)
            self.assertTrue(all('home-path' in hit for hit in hits))

    def test_review_new_live_session_file_has_no_identifier_allowance(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            path = write(root, 'apps/viewer/src/live_session_scratch.cpp', 'options.' + 'lan.enabled;\n')
            hits = publish_scan.security_scan(root, CONFIG, [path])
            self.assertEqual(len(hits), 1, hits)
            self.assertIn('lan-host', hits[0])

    def test_review_login_hosts_cannot_use_lan_identifier_allowance(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            path = write(root, 'apps/viewer/src/map_mode_ready.cpp', '// ssh root@options.' + 'lan.\n')
            hits = publish_scan.security_scan(root, CONFIG, [path])
            self.assertEqual(len(hits), 1, hits)
            self.assertIn('lan-host', hits[0])

    def test_review_url_and_colon_prefixes_cannot_use_lan_identifier_allowance(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            for text in ('// see http://state.' + 'lan.', '// (http://entry.' + 'lan)',
                         '// service:options.' + 'lan.'):
                with self.subTest(line=text):
                    path = write(root, 'apps/viewer/src/map_mode_ready.cpp', text + '\n')
                    hits = publish_scan.security_scan(root, CONFIG, [path])
                    self.assertEqual(len(hits), 1, hits)
                    self.assertIn('lan-host', hits[0])

    def test_network_fixture_ip_allowance_covers_only_quoted_match(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            path = write(root, 'tests/net/lan_directory_tests.cpp', 'auto s = 10.0.' + '0.1; // "note"\n')
            hits = publish_scan.security_scan(root, CONFIG, [path])
            self.assertEqual(len(hits), 1, hits)
            self.assertIn('private-ipv4', hits[0])

    def test_review_lan_hosts_in_prose_and_strings_with_trailing_punctuation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            for text in ('// connect to options.' + 'lan.',
                         'auto h = " options.' + 'lan.";',
                         '// see http://x.example/?h=state.' + 'lan&p=1',
                         '// ask state.' + 'lan, then retry',
                         '// the lab host (state.' + 'lan).'):
                with self.subTest(line=text):
                    path = write(root, 'apps/viewer/src/map_mode_ready.cpp', text + '\n')
                    hits = publish_scan.security_scan(root, CONFIG, [path])
                    self.assertEqual(len(hits), 1, hits)
                    self.assertIn('lan-host', hits[0])

    def test_review_quote_char_literals_and_comments_do_not_license_fixture_ips(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            for name, text in (
                    ('tests/net/lan_directory_tests.cpp',
                     "auto a = '\"'; auto s = 10.0." + "0.1; auto b = '\"';"),
                    ('tests/presentation/renderer/test_lan_lobby.py',
                     "a = '\"Alpha|'; s = 10.0." + "0.5; b = '\"';"),
                    ('tests/net/lan_directory_tests.cpp', '// gateway "10.0.' + '0.1"')):
                with self.subTest(source=name, line=text):
                    path = write(root, name, text + '\n')
                    hits = publish_scan.security_scan(root, CONFIG, [path])
                    self.assertEqual(len(hits), 1, hits)
                    self.assertIn('private-ipv4', hits[0])

    def test_review_additional_round_one_two_three_negative_probes(self):
        cases = (
            ('apps/viewer/src/map_mode_ready.cpp', '// reach mybox.options.' + 'lan. for the rig'),
            ('apps/viewer/src/map_mode_ready.cpp', 'auto h = "mybox.options.' + 'lan";'),
            ('apps/viewer/src/map_mode_ready.cpp', 'if (options.' + 'lan) { connect(mybox.' + 'lan); }'),
            ('apps/viewer/src/map_mode_ready.cpp', 'auto p = gw->options.' + 'lan;'),
            ('apps/viewer/src/live_session_view.cpp', 'connect("http://state.' + 'lan/x");'),
            ('apps/viewer/src/live_session_view.cpp', 'auto h = "state.' + 'lan";'),
            ('tests/net/lan_directory_tests.cpp', '"10.0.' + '0.1" "10.0.' + '0.9"'),
            ('tests/net/lan_directory_tests.cpp', '"10.0.' + '0.1"; x = 192.168.' + '2.133;'),
            ('tests/net/lan_directory_tests.cpp', 'auto s = "10.0.' + '0.1"; auto t = 10.0.' + '0.2;'),
            ('tests/presentation/renderer/test_lan_lobby.py', '# "Alpha|" 10.0.' + '0.5'),
            ('src/b.cpp', 'auto id = PWD' + '11Worker;'),
            ('docs/PWL' + '12Runbook.md', 'plain text'),
            ('apps/viewer/src/galactic/galactic_hud.cpp', 'Eawr' + 'BuildQueueDial.Thom' + 'Ernst'),
            ('apps/viewer/src/galactic/galactic_hud.cpp', 'Eawr' + 'BuildQueueDial_Thom' + 'Ernst'),
            ('apps/viewer/src/galactic/galactic_hud.cpp', 'Eawr' + 'BuildQueueDial PWD' + '11'),
            ('apps/viewer/src/galactic/galactic_hud.cpp', 'Eawr' + 'BuildQueueDialX'),
            ('apps/viewer/src/other.cpp', 'Eawr' + 'BuildQueueDial'),
            ('apps/viewer/src/galactic/galactic_hud.cpp', 'Eawr' + 'BuildQueueDial_PWD' + '11'),
            ('apps/viewer/src/galactic/galactic_hud.cpp', '9Eawr' + 'BuildQueueDial'),
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            for name, text in cases:
                with self.subTest(source=name, line=text):
                    path = write(root, name, text + '\n')
                    self.assertTrue(publish_scan.security_scan(root, CONFIG, [path],
                                    extra_terms=['Thom' + 'Ernst', 'PWD' + '11', 'PWL' + '12', 'eawr' + 'build']))

    def test_public_identifier_exception_does_not_start_inside_a_digit_run(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            path = write(root, 'src/presentation/godot/ui/build_queue_dial.hpp', '9Eawr' + 'BuildQueueDial;\n')
            hits = publish_scan.security_scan(root, CONFIG, [path], extra_terms=['eawr' + 'build'])
            self.assertEqual(len(hits), 1, hits)
            self.assertIn('private-name', hits[0])

    def test_private_terms_keep_substrings_in_identifiers_paths_and_suffixes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            write(root, "src/HostXBuilder.cpp", "HostXBuilder hostx_value; /srv/HostX/file HOSTX\n")
            hits = publish_scan.security_scan(root, CONFIG, publish_scan.tree_files(root), extra_terms=["hostx"])
            self.assertEqual(len(hits), 5, hits)
            self.assertTrue(all('private-name' in hit for hit in hits))
            self.assertTrue(any(':0:' in hit for hit in hits), hits)

    def test_home_prose_exception_is_scoped_to_its_file_and_match(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            path = write(root, 'docs/behaviour/gc-hud-inventory.md', 'Previous/next/home/' + 'zoom state; '
                         + 'file:///home/' + 'zoom/private; /srv/backup/home/' + 'zoom/private\n')
            hits = publish_scan.security_scan(root, CONFIG, [path])
            self.assertEqual(len(hits), 2, hits)
            self.assertTrue(all('home-path' in hit for hit in hits))
            path = write(root, 'a.md', 'Previous/next/home/' + 'zoom state\n')
            self.assertEqual(len(publish_scan.security_scan(root, CONFIG, [path])), 1)

    def test_public_identifier_exception_is_exact_and_keeps_extra_rules(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            identifier = 'Eawr' + 'BuildQueueDial'
            term = 'eawr' + 'build'
            path = write(root, 'src/presentation/godot/ui/build_queue_dial.hpp', identifier + ';\n')
            self.assertEqual(publish_scan.security_scan(root, CONFIG, [path], extra_terms=[term]), [])
            path.write_text(identifier + 'Backup;\n', encoding='utf-8')
            self.assertEqual(len(publish_scan.security_scan(root, CONFIG, [path], extra_terms=[term])), 1)
            path.write_text(identifier + ';\n', encoding='utf-8')
            hits = publish_scan.security_scan(root, CONFIG, [path], {'private-extra': identifier}, [term])
            self.assertEqual(len(hits), 1, hits)
            self.assertIn('private-extra', hits[0])
            unknown = write(root, 'src/scratch.cpp', identifier + ';\n')
            self.assertEqual(len(publish_scan.security_scan(root, CONFIG, [unknown], extra_terms=[term])), 1)

    def test_same_lan_token_in_a_string_is_not_licensed_by_member_access(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            path = write(root, 'apps/viewer/src/map_mode_ready.cpp',
                         'auto x = options.' + 'lan; const char* host = "options.' + 'lan";\n')
            hits = publish_scan.security_scan(root, CONFIG, [path])
            self.assertEqual(len(hits), 2, hits)
            self.assertIn('lan-host', hits[0])

    def test_new_lan_owner_chain_text_requires_review(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            path = write(root, 'apps/viewer/src/map_mode_ready.cpp',
                         'state.options.' + 'lan.enabled; state.live_options.' + 'lan.enabled;\n')
            self.assertEqual(len(publish_scan.security_scan(root, CONFIG, [path])), 2)

    def test_value_allowlist_does_not_hide_a_second_hostname_on_the_same_line(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            config = copy.deepcopy(CONFIG)
            config['security']['allow'].append({'path': 'src/a.cpp', 'regex': r'options[.]lan',
                'value_regex': r'options[.]lan', 'patterns': ['lan-host']})
            write(root, 'src/a.cpp', 'options.' + 'lan.enabled; // box.' + 'lan HostX\n')
            hits = publish_scan.security_scan(root, config, publish_scan.tree_files(root), extra_terms=['hostx'])
            self.assertEqual(sorted(hit.split(': ')[1] for hit in hits), ['lan-host', 'lan-host', 'private-name'])
            self.assertTrue(any('box.' + 'lan' in hit for hit in hits), hits)

    def test_network_fixture_allowance_keeps_unlisted_ips_visible(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            write(root, 'tests/net/port_mapping_tests.cpp', '"192.168.' + '1.1" "192.168.' + '1.99"\n')
            hits = publish_scan.security_scan(root, CONFIG, publish_scan.tree_files(root))
            self.assertEqual(len(hits), 2, hits)
            self.assertTrue(any('1.99' in hit for hit in hits))

    def test_lan_member_allowance_covers_conditions_and_capture_without_hiding_hosts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            write(root, 'apps/viewer/src/live_session_prepare.cpp',
                  '[lan = options_.' + 'lan](auto tick) {};\n'
                  + 'return options_.' + 'lan != nullptr; // box.' + 'lan\n')
            hits = publish_scan.security_scan(root, CONFIG, publish_scan.tree_files(root))
            self.assertEqual(len(hits), 3, hits)
            self.assertTrue(any('box.' + 'lan' in hit for hit in hits))

    def test_exact_security_lines_require_matching_path_and_unchanged_full_text(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            for pin in CONFIG['security']['exact_line_allow']:
                with self.subTest(source=pin['path'], digest=pin['sha256']):
                    lines = (ROOT / pin['path']).read_text(encoding='utf-8').splitlines()
                    line = next(line for line in lines
                                if publish_scan.hashlib.sha256(line.encode('utf-8')).hexdigest() == pin['sha256'])
                    path = write(root, pin['path'], line + '\n')
                    self.assertEqual(publish_scan.security_scan(root, CONFIG, [path]), [])
                    for changed in (line + ' ', line + ' // changed', ' ' + line):
                        path.write_text(changed + '\n', encoding='utf-8')
                        self.assertTrue(publish_scan.security_scan(root, CONFIG, [path]))
                    other = write(root, 'scratch.cpp', line + '\n')
                    self.assertTrue(publish_scan.security_scan(root, CONFIG, [other]))
                    path.write_text(line + '\n', encoding='utf-8')
                    self.assertTrue(publish_scan.security_scan(root, CONFIG, [path],
                                   extra_regex={name: CONFIG['security']['regex'][name] for name in pin['patterns']}))

    def test_new_campaign_and_installed_replays_have_exact_reviewed_pins(self):
        names = [name for name in CONFIG['assets']['binary_allowlist_origins']
                 if name.startswith(('tests/galactic/', 'tests/skirmish/fixtures/installed/'))]
        self.assertTrue(names)
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            for name in names:
                with self.subTest(fixture=name):
                    data = (ROOT / name).read_bytes()
                    fixture = write(root, name, data)
                    self.assertEqual(publish_scan.asset_scan(root, CONFIG, [fixture]), [])
                    fixture.write_bytes(data + b'changed')
                    self.assertIn('binary file not on the allowlist',
                                  publish_scan.asset_scan(root, CONFIG, [fixture])[0])
                    fixture.write_bytes(data)
                    self.assertIn(f'{name}: byte-identical to a retail file',
                                  publish_scan.asset_scan(root, CONFIG, [fixture],
                                      {publish_scan.hashlib.sha256(data).hexdigest()}))

    def test_checker_applies_cleanroom_rules(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            write(root, "src/a.cpp", "// see Gizmo" + "Class:" + ":Get_X and the icon i_button-m_" + "big_tank.tga\n")
            hits = publish_scan.cleanroom_scan(root, CONFIG, publish_scan.tree_files(root))
            self.assertEqual(len(hits), 1, hits)
            self.assertIn("Gizmo" + "Class", hits[0])

    def test_assets(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            write(root, "art/ship.alo", "x")
            write(root, "img.png", b"\x89PNG\r\n\x1a\n")
            write(root, "blob.bin", b"\x00\x01")
            fixture = next(iter(CONFIG["assets"]["binary_allowlist"]))
            write(root, fixture, (ROOT / fixture).read_bytes())
            write(root, "tests/shaders/fixtures/Synthetic.fx", "float4 main() : COLOR { return 1; }\n")
            hits = publish_scan.asset_scan(root, CONFIG, publish_scan.tree_files(root))
            self.assertEqual(sorted(hit.split(":")[0] for hit in hits), ["art/ship.alo", "blob.bin", "img.png"])
            data = (root / "blob.bin").read_bytes()
            retail = {publish_scan.hashlib.sha256(data).hexdigest()}
            hits = publish_scan.asset_scan(root, CONFIG, publish_scan.tree_files(root), retail)
            self.assertIn("blob.bin: byte-identical to a retail file", hits)

    def test_readme_rule(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            for term in CONFIG["readme"]["forbidden"]:
                write(root, "README.md", f"# About\nRuns {term.upper()} data.\n")
                self.assertEqual(len(publish_scan.readme_scan(root, CONFIG)), 1)

    def test_reviewed_lua_artwork_is_byte_exact_and_still_checked_for_retail(self):
        for name, digest in CONFIG["assets"]["image_allowlist"].items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                root = pathlib.Path(temporary)
                data = (ROOT / name).read_bytes()
                self.assertEqual(publish_scan.hashlib.sha256(data).hexdigest(), digest)
                self.assertIn("Lua 5.0.2", CONFIG["assets"]["image_allowlist_origins"][name])
                image = write(root, name, data)
                self.assertEqual(publish_scan.asset_scan(root, CONFIG, [image]), [])
                self.assertIn(f"{name}: byte-identical to a retail file",
                              publish_scan.asset_scan(root, CONFIG, [image], {digest}))
                image.write_bytes(data + b"changed")
                self.assertIn("image not on the reviewed allowlist",
                              publish_scan.asset_scan(root, CONFIG, [image])[0])

    def test_generated_files_are_scanned_and_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            names = ["pkg/__pycache__/notes.txt", "pkg/cache.pyc", "out/generated.txt", "pkg/result.obj"]
            for name in names:
                write(root, name, "runs on HostX")
            results = publish_scan.scan(root, CONFIG, extra_terms=["hostx"])
            self.assertEqual(len(results["security"]), len(names))
            self.assertEqual(len(results["assets"]), len(names))

    def test_paths_are_checked_even_for_binary_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            write(root, "docs/HostX-notes.bin", b"\x00")
            write(root, "docs/box." + "lan/readme.md", "safe")
            hits = publish_scan.security_scan(root, CONFIG, publish_scan.tree_files(root), extra_terms=["hostx"])
            self.assertEqual(sorted(hit.split(": ")[1] for hit in hits), ["lan-host", "private-name"])

    def test_third_party_has_no_blanket_exemptions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            for name, data in [("blob.bin", b"\x00"), ("bad.txt", b"\xff"), ("program.exe", b"MZ\x00")]:
                write(root, "third_party/new/" + name, data)
            write(root, "third_party/new/api.hpp", "Gizmo" + "Class")
            results = publish_scan.scan(root, CONFIG)
            self.assertEqual(len(results["cleanroom"]), 1)
            self.assertEqual(len(results["assets"]), 4)  # executable is also generated

    def test_binary_fixtures_are_hash_pinned(self):
        name, digest = next(iter(CONFIG["assets"]["binary_allowlist"].items()))
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            fixture = write(root, name, (ROOT / name).read_bytes())
            self.assertEqual(publish_scan.asset_scan(root, CONFIG, [fixture]), [])
            fixture.write_bytes(fixture.read_bytes() + b"\x00changed")
            self.assertIn("binary file not on the allowlist", publish_scan.asset_scan(root, CONFIG, [fixture])[0])

    def test_allow_rules_cannot_mask_other_patterns_or_private_rules(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = tree(temporary)
            name = "third_party/lua/doc/manual.html"
            write(root, name, "/home/" + "roberto HostX box." + "lan")
            hits = publish_scan.security_scan(root, CONFIG, publish_scan.tree_files(root),
                                              {"private-extra": "HostX"}, ["hostx"])
            self.assertEqual(sorted(hit.split(": ")[1] for hit in hits),
                             ["lan-host", "private-extra", "private-name"])

    def test_upstream_non_utf8_sample_has_an_exact_pin(self):
        name = "third_party/lua/test/life.lua"
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            data = (ROOT / name).read_bytes().replace(b"\r\n", b"\n").replace(b"\r", b"\n")
            sample = write(root, name, data)
            self.assertIn("Lua 5.0.2", CONFIG["assets"]["binary_allowlist_origins"][name])
            self.assertEqual(publish_scan.asset_scan(root, CONFIG, [sample]), [])
            # A Windows checkout (core.autocrlf) writes CRLF: still the reviewed file.
            sample.write_bytes(data.replace(b"\n", b"\r\n"))
            self.assertEqual(publish_scan.asset_scan(root, CONFIG, [sample]), [])
            sample.write_bytes(data + b"changed")
            self.assertIn("binary file not on the allowlist", publish_scan.asset_scan(root, CONFIG, [sample])[0])

    def test_gitleaks_pins_every_platform(self):
        assets = CONFIG["gitleaks"]["assets"]
        self.assertEqual(set(assets), {"windows_x64", "linux_x64", "linux_arm64", "darwin_x64", "darwin_arm64"})
        for asset in assets.values():
            self.assertRegex(asset["sha256"], "^[0-9a-f]{64}$")
            self.assertIn(CONFIG["gitleaks"]["version"], asset["file"])


if __name__ == "__main__":
    unittest.main()
