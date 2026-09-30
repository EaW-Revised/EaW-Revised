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
