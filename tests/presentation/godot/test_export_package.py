import configparser
import fnmatch
import json
import importlib.util
import pathlib
import subprocess
import unittest
from tempfile import TemporaryDirectory
from unittest.mock import patch


ROOT = pathlib.Path(__file__).resolve().parents[3]
MODULE_PATH = ROOT / "apps/viewer/tools/export_package.py"
SPEC = importlib.util.spec_from_file_location("export_package", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ExportPackageGuardTests(unittest.TestCase):
    def test_pck_allowlist_covers_the_presets_public_config_filters(self):
        presets = configparser.ConfigParser(interpolation=None)
        presets.read(ROOT / "apps/viewer/project/export_presets.cfg")
        tracked = subprocess.check_output(
            ["git", "ls-files", "--", "apps/viewer/project/config"], cwd=ROOT, text=True
        ).splitlines()
        config_entries = {path.removeprefix("apps/viewer/project/") for path in tracked}
        for section in presets.sections():
            if not presets.has_option(section, "include_filter"):
                continue
            filters = json.loads(presets[section]["include_filter"]).split(",")
            included = {name for name in config_entries
                        if any(fnmatch.fnmatchcase(name, pattern) for pattern in filters)}
            self.assertEqual(included, {name for name in MODULE.REQUIRED_PCK_ENTRIES
                                        if name.startswith("config/")})

    def valid_pck_entries(self):
        return sorted(MODULE.REQUIRED_PCK_ENTRIES) + [".godot/exported/test-main.scn"]

    def test_public_config_package_passes_the_exact_allowlist(self):
        MODULE.validate_pck_entries(self.valid_pck_entries())

    def test_pck_rejects_missing_extra_and_duplicate_entries(self):
        entries = self.valid_pck_entries()
        for invalid in (entries[1:], entries + ["config/private.json"], entries + [entries[0]]):
            with self.subTest(entries=invalid), self.assertRaises(ValueError):
                MODULE.validate_pck_entries(invalid)

    def test_forbidden_content_stays_rejected_even_if_allowlisted(self):
        for suffix in MODULE.FORBIDDEN_PCK_SUFFIXES:
            forbidden = "config/private" + suffix
            with self.subTest(suffix=suffix), patch.object(
                MODULE, "REQUIRED_PCK_ENTRIES", MODULE.REQUIRED_PCK_ENTRIES | {forbidden}
            ), self.assertRaisesRegex(ValueError, "forbidden generated/private"):
                MODULE.validate_pck_entries(self.valid_pck_entries())

    def test_pck_requires_exactly_one_exported_main_scene(self):
        entries = self.valid_pck_entries()
        for invalid in (entries[:-1], entries + [".godot/exported/other-main.scn"]):
            with self.assertRaisesRegex(ValueError, "expected one exported main scene"):
                MODULE.validate_pck_entries(invalid)

    def test_pinned_engine_identity_is_returned_from_probe(self):
        completed = subprocess.CompletedProcess(
            [str(MODULE_PATH), "--version"],
            0,
            stdout=f"{MODULE.PINNED_GODOT_IDENTITY}\n",
            stderr="",
        )
        with patch.object(MODULE.subprocess, "run", return_value=completed):
            identity, version = MODULE.validate_godot_identity(MODULE_PATH)
        self.assertEqual(identity, MODULE.PINNED_GODOT_IDENTITY)
        self.assertEqual(version, MODULE.PINNED_GODOT_VERSION)

    def test_wrong_engine_identity_is_rejected_by_bounded_probe(self):
        wrong_output = "4.7.1.stable.official.deadbeef0\n"
        completed = subprocess.CompletedProcess(
            [str(MODULE_PATH), "--version"], 0, stdout=wrong_output, stderr=""
        )
        with patch.object(MODULE.subprocess, "run", return_value=completed) as run:
            with self.assertRaisesRegex(ValueError, "does not report pinned identity"):
                MODULE.validate_godot_identity(MODULE_PATH)
        self.assertEqual(run.call_args.kwargs["timeout"], MODULE.GODOT_VERSION_TIMEOUT_SECONDS)

    def test_out_of_tree_output_is_rejected_before_creation(self):
        with TemporaryDirectory() as temporary:
            outside = pathlib.Path(temporary) / "package"
            with self.assertRaisesRegex(ValueError, "within the repository out/ tree"):
                MODULE.resolve_output_path(outside, "package-root")
            self.assertFalse(outside.exists())

    def test_smoke_engine_version_mismatch_is_rejected(self):
        smoke = {"versions": {"godot": "4.7.1-stable"}}
        with self.assertRaisesRegex(ValueError, "smoke receipt versions.godot"):
            MODULE.verify_smoke_engine_version(smoke, MODULE.PINNED_GODOT_VERSION)

    def test_package_carries_the_pinned_licence_files(self):
        with TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            cache = root / "cache"
            cache.mkdir()
            items = []
            for name in ("godot-LICENSE.txt", "godot-cpp-LICENSE.md"):
                data = f"licence text of {name}\n".encode("utf-8")
                (cache / name).write_bytes(data)
                items.append({"name": name, "url": "https://invalid.example/" + name,
                              "sha256": MODULE.hashlib.sha256(data).hexdigest()})
            package = root / "package"
            package.mkdir()
            with patch.object(MODULE, "LICENSE_CACHE", cache), \
                    patch.object(MODULE, "pinned_license_files", return_value=items):
                MODULE.bundle_licenses(package)
                names = {path.name for path in (package / MODULE.LICENSES_DIR).iterdir()}
                self.assertEqual(names, MODULE.expected_license_names())
                self.assertEqual(names, {"LICENSE", "THIRD_PARTY_NOTICES.md", "godot-LICENSE.txt",
                                         "godot-cpp-LICENSE.md"})
                (cache / "godot-cpp-LICENSE.md").write_bytes(b"tampered\n")
                with patch.object(MODULE.urllib.request, "urlopen", side_effect=OSError("offline")):
                    with self.assertRaises(OSError):
                        MODULE.bundle_licenses(package)

    def test_every_pinned_licence_file_names_a_url_and_hash(self):
        items = MODULE.pinned_license_files()
        self.assertEqual({item["name"] for item in items},
                         {"godot-LICENSE.txt", "godot-COPYRIGHT.txt", "godot-cpp-LICENSE.md"})
        for item in items:
            self.assertTrue(item["url"].startswith("https://raw.githubusercontent.com/godotengine/"))
            self.assertRegex(item["sha256"], "^[0-9a-f]{64}$")

    def test_pinned_smoke_engine_versions_are_accepted(self):
        smoke = {
            "backend": {"engine": "Godot 4.7.2-stable"},
            "versions": {"godot": "4.7.2-stable"},
        }
        MODULE.verify_smoke_engine_version(smoke, MODULE.PINNED_GODOT_VERSION)


if __name__ == "__main__":
    unittest.main(argv=[__file__])
