"""Offline integrity, closure, relocation and notice controls for the optional package."""
from __future__ import annotations

import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools.shader_runtime_package import package


class PackageTests(unittest.TestCase):
    def fixture(self, root):
        folder = root / "package"
        folder.mkdir()
        payloads = {"compiler.dll": b"invented boundary fixture", "SOURCE-OFFER.md": b"source available"}
        files = []
        for name, data in payloads.items():
            (folder / name).write_bytes(data)
            files.append({"path": name, "sha256": package.digest(folder / name), "size": len(data),
                          "role": "dll" if name.endswith(".dll") else "notice", "imports": ["kernel32.dll"]})
        lock = {"schema": "eawr-shader-runtime/1", "platform": "windows-x64", "files": files,
                "system_dlls": ["kernel32.dll"]}
        lock_path = root / "lock.json"
        lock_path.write_text(json.dumps(lock), encoding="utf-8")
        shutil.copyfile(lock_path, folder / "manifest.json")
        return folder, lock_path

    def test_shipping_lock_and_generated_header_and_publish_pin(self):
        lock = package.load_lock()
        self.assertEqual(lock["activation"], "disabled")
        self.assertEqual(lock["abi"]["spirv_cross"], [0, 61, 0])
        self.assertEqual({item["path"] for item in lock["files"] if item["role"] == "dll"},
                         {"libgcc_s_seh-1.dll", "libvkd3d-shader-1.dll", "libspirv-cross-c-shared.dll"})
        for source in lock["sources"][:2]:
            self.assertFalse(source["modified"])
        names = {item["path"] for item in lock["files"]}
        self.assertIn("sources/vkd3d-2.1.tar.xz", names)
        for name in ["SOURCE-OFFER.md", "ATTRIBUTION.md", "licenses/vkd3d-LICENSE",
                     "licenses/SPIRV-Cross-LICENSE", "licenses/GCC-MinGW-copyright"]:
            item = next(item for item in lock["files"] if item["path"] == name)
            self.assertEqual(package.digest(package.LOCK.parent / name), item["sha256"])
        config = json.loads((package.ROOT / "tools/publish_scan/config.json").read_text())
        self.assertEqual(config["assets"]["external_package_manifests"]["third_party/shader_runtime/lock.json"],
                         package.digest(package.LOCK))
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "identities.hpp"
            package.write_header(package.LOCK, path)
            self.assertEqual(path.read_bytes(), (package.LOCK.parent / "identities.hpp").read_bytes())

    def test_relocation_and_exact_staging(self):
        with tempfile.TemporaryDirectory() as temporary, patch.object(package, "pe_imports", return_value=["kernel32.dll"]):
            root = Path(temporary)
            source, lock = self.fixture(root)
            target = root / "new install" / "shader-runtime"
            package.stage(source, target, lock)
            shutil.rmtree(source)
            package.verify(target, lock)
            with self.assertRaisesRegex(ValueError, "already exists"):
                package.stage(target, target, lock)

    def test_missing_replaced_notice_and_dll(self):
        with tempfile.TemporaryDirectory() as temporary, patch.object(package, "pe_imports", return_value=["kernel32.dll"]):
            root = Path(temporary)
            source, lock = self.fixture(root)
            for name in ["compiler.dll", "SOURCE-OFFER.md"]:
                original = (source / name).read_bytes()
                (source / name).write_bytes(b"replaced")
                with self.assertRaisesRegex(ValueError, "integrity failure"):
                    package.verify(source, lock)
                (source / name).unlink()
                with self.assertRaisesRegex(ValueError, "missing or unexpected"):
                    package.verify(source, lock)
                (source / name).write_bytes(original)

    def test_package_manifest_cannot_replace_trust_policy(self):
        with tempfile.TemporaryDirectory() as temporary:
            source, lock = self.fixture(Path(temporary))
            (source / "manifest.json").write_text("{}")
            with self.assertRaisesRegex(ValueError, "manifest differs"):
                package.verify(source, lock)

    def test_extra_files_and_dependencies_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            source, lock = self.fixture(Path(temporary))
            (source / "surprise.dll").write_bytes(b"unreviewed")
            with self.assertRaisesRegex(ValueError, "missing or unexpected"):
                package.verify(source, lock)
            (source / "surprise.dll").unlink()
            with patch.object(package, "pe_imports", return_value=["surprise.dll"]):
                with self.assertRaisesRegex(ValueError, "import table differs"):
                    package.verify(source, lock)

    def test_bounded_paths_and_malformed_pe(self):
        for name in ["../outside.dll", "/absolute.dll", "folder/../outside.dll", "folder\\a.dll", "a:a.dll",
                     "a//b.dll", "./a.dll", "~leftover.dll", "private.pdb", "tool.exe"]:
            with self.subTest(name=name), self.assertRaises(ValueError):
                package.logical_path(name)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "invalid.dll"
            path.write_bytes(b"short")
            with self.assertRaises(ValueError):
                package.pe_imports(path)

    @unittest.skipUnless(os.name == "nt" and os.environ.get("EAWR_SHADER_RUNTIME_TEST_PACKAGE"),
                         "optional offline package absent")
    def test_real_shared_c_abi_and_relocated_package(self):
        source = Path(os.environ["EAWR_SHADER_RUNTIME_TEST_PACKAGE"])
        with tempfile.TemporaryDirectory() as temporary:
            relocated = Path(temporary) / "relocated install" / "shader-runtime"
            package.stage(source, relocated)
            subprocess.run([sys.executable, "-m", "tools.shader_runtime_package.abi_probe", str(relocated)],
                           cwd=package.ROOT, check=True, timeout=60)


if __name__ == "__main__":
    unittest.main()
