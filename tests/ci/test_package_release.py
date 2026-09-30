"""The release packager (tools/release/package_release.py) on a stand-in build tree."""

from __future__ import annotations

import hashlib
import pathlib
import sys
import tempfile
import unittest
import zipfile
from unittest.mock import patch

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "release"))
import package_release  # noqa: E402


class PackageReleaseTests(unittest.TestCase):
    def fake_build(self, root: pathlib.Path, suffix: str, vs_layout: bool) -> pathlib.Path:
        build = root / "build"
        for name in package_release.TOOLS:
            folder = build / "apps" / name / ("Release" if vs_layout else "")
            folder.mkdir(parents=True, exist_ok=True)
            (folder / (name + suffix)).write_bytes(b"binary " + name.encode())
        return build

    def fake_project(self, root: pathlib.Path, platform: str) -> pathlib.Path:
        project = root / "project"
        (project / "bin").mkdir(parents=True)
        (project / ".godot").mkdir()
        (project / "project.godot").write_text("config_version=5\n", encoding="utf-8")
        (project / ".godot" / "cache.bin").write_bytes(b"x")
        (project / "bin" / package_release.PLATFORMS[platform]["extension"]).write_bytes(b"ext")
        (project / "bin" / "libeawr_viewer.other.so").write_bytes(b"other")
        return project

    def test_windows_package(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            build = self.fake_build(root, ".exe", vs_layout=True)
            project = self.fake_project(root, "windows-x64")
            with patch.object(package_release, "VIEWER_PROJECT", project):
                files = package_release.package(build, "windows-x64", "v0.1.0", root / "out")
            self.assertEqual([f.name for f in files], ["eaw-revised-v0.1.0-windows-x64-tools.zip",
                                                       "eaw-revised-v0.1.0-windows-x64-viewer.zip"])
            with zipfile.ZipFile(files[0]) as archive:
                names = set(archive.namelist())
            self.assertIn("eaw-revised-v0.1.0-windows-x64-tools/bin/sim_headless.exe", names)
            self.assertIn("eaw-revised-v0.1.0-windows-x64-tools/LICENSE", names)
            self.assertIn("eaw-revised-v0.1.0-windows-x64-tools/THIRD_PARTY_NOTICES.md", names)
            with zipfile.ZipFile(files[1]) as archive:
                names = set(archive.namelist())
            top = "eaw-revised-v0.1.0-windows-x64-viewer/"
            self.assertIn(top + "project/bin/libeawr_viewer.windows.template_release.x86_64.dll", names)
            self.assertIn(top + "project/project.godot", names)
            self.assertNotIn(top + "project/bin/libeawr_viewer.other.so", names)
            self.assertFalse(any("/.godot/" in name for name in names))
            sums = package_release.write_sums(files, root / "out")
            first = sums.read_text(encoding="utf-8").splitlines()[0].split("  ")
            self.assertEqual(first[0], hashlib.sha256(files[0].read_bytes()).hexdigest())

    def test_missing_tool_or_extension_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            build = self.fake_build(root, "", vs_layout=False)
            (build / "apps" / "sky_scan" / "sky_scan").unlink()
            project = self.fake_project(root, "linux-x64")
            with patch.object(package_release, "VIEWER_PROJECT", project):
                with self.assertRaises(FileNotFoundError):
                    package_release.package(build, "linux-x64", "v0.1.0", root / "out")
            self.assertEqual(package_release.main(["--build-dir", str(build), "--platform", "linux-x64",
                                                   "--version", "v1", "--out", str(root / "o2")]), 2)


if __name__ == "__main__":
    unittest.main()
