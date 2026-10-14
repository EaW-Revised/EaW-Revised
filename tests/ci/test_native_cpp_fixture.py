"""Fixture compiler selection must not mix independent Windows installations."""

from pathlib import Path
import json
import os
import time
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import native_cpp_fixture as fixture


class NativeFixtureTests(unittest.TestCase):
    def setUp(self):
        fixture._vcvars_environments.clear()

    def installation(self, root, version="17.14"):
        return json.dumps([{"installationPath": str(root), "installationVersion": version}])

    def windows_fixture(self, directory):
        root = Path(directory) / "VS 2022"
        compiler = root / "VC/Tools/MSVC/14.44/bin/Hostx64/x64/cl.exe"
        compiler.parent.mkdir(parents=True)
        compiler.touch()
        vcvars = root / "VC/Auxiliary/Build/vcvars64.bat"
        vcvars.parent.mkdir(parents=True)
        vcvars.touch()
        (vcvars.parent / "Microsoft.VCToolsVersion.default.txt").write_text("14.44")
        where = Path(directory) / "Microsoft Visual Studio/Installer/vswhere.exe"
        where.parent.mkdir(parents=True)
        where.touch()
        tools = root / "VC/Tools/MSVC/14.44"
        environment = {"ProgramFiles(x86)": directory, "INCLUDE": "wrong STL 14.51",
                       "VCToolsInstallDir": "wrong VS", "VSCMD_VER": "18", "CL": "/bad"}
        captured = f"VCToolsInstallDir={tools}\nVCToolsVersion=14.44\nINCLUDE=matching STL\nPath=matching bins\n"
        return root, compiler, environment, captured

    def test_windows_ignores_path_clang_and_reinitializes_matching_environment(self):
        with tempfile.TemporaryDirectory() as directory:
            root, compiler, environment, captured = self.windows_fixture(directory)
            replies = [subprocess.CompletedProcess([], 0, self.installation(root), ""),
                       subprocess.CompletedProcess([], 0, captured, "")]
            with patch.object(fixture, "os", SimpleNamespace(name="nt", environ=environment)), \
                    patch.object(fixture.subprocess, "run", side_effect=replies) as run, \
                    patch.object(fixture.shutil, "which", return_value="old clang++") as which:
                selected = fixture.select_toolchain()
            self.assertEqual(selected.compiler, str(compiler))
            self.assertEqual(selected.environment["INCLUDE"], "matching STL")
            self.assertEqual(selected.environment["PATH"], "matching bins")
            self.assertTrue(selected.msvc)
            self.assertIn("[17.0,18.0)", run.call_args_list[0].args[0])
            self.assertIn(str(root / "VC/Auxiliary/Build/vcvars64.bat"), run.call_args_list[1].args[0])
            for variable in ("INCLUDE", "VCTOOLSINSTALLDIR", "VSCMD_VER", "CL"):
                self.assertNotIn(variable, run.call_args_list[1].kwargs["env"])
            self.assertEqual(environment["INCLUDE"], "wrong STL 14.51")
            which.assert_not_called()

    def test_slow_vcvars_is_cached_and_invalidated_by_installation_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            root, _, environment, captured = self.windows_fixture(directory)
            version = "17.14"
            probes = []

            def fake_run(command, **kwargs):
                if isinstance(command, list):
                    return subprocess.CompletedProcess([], 0, self.installation(root, version), "")
                probes.append(command)
                self.assertEqual(kwargs["timeout"], 120)
                time.sleep(0.02)  # A slow fake vcvars; cache hits must not call it.
                return subprocess.CompletedProcess([], 0, captured, "")

            with patch.object(fixture, "os", SimpleNamespace(name="nt", environ=environment)), \
                    patch.object(fixture.subprocess, "run", side_effect=fake_run):
                first = fixture.select_toolchain()
                first.environment["INCLUDE"] = "caller modification"
                self.assertEqual(fixture.select_toolchain().environment["INCLUDE"], "matching STL")
                self.assertEqual(len(probes), 1)
                version = "17.15"
                fixture.select_toolchain()
                self.assertEqual(len(probes), 2)
                vcvars = root / "VC/Auxiliary/Build/vcvars64.bat"
                stamp = vcvars.stat().st_mtime_ns + 1_000_000_000
                os.utime(vcvars, ns=(stamp, stamp))
                fixture.select_toolchain()
                self.assertEqual(len(probes), 3)
                (vcvars.parent / "Microsoft.VCToolsVersion.default.txt").write_text("14.45")
                fixture.select_toolchain()
                self.assertEqual(len(probes), 4)
                root, _, _, captured = self.windows_fixture(str(Path(directory) / "other"))
                fixture.select_toolchain()
                self.assertEqual(len(probes), 5)

    def test_failed_vcvars_is_not_cached(self):
        with tempfile.TemporaryDirectory() as directory:
            root, _, environment, captured = self.windows_fixture(directory)
            with patch.object(fixture, "os", SimpleNamespace(name="nt", environ=environment)), \
                    patch.object(fixture.subprocess, "run", side_effect=[
                        subprocess.CompletedProcess([], 0, self.installation(root), ""),
                        subprocess.CompletedProcess([], 1, "", "failed setup"),
                        subprocess.CompletedProcess([], 0, self.installation(root), ""),
                        subprocess.CompletedProcess([], 0, captured, "")]):
                with self.assertRaisesRegex(RuntimeError, "failed setup"):
                    fixture.select_toolchain()
                self.assertTrue(fixture.select_toolchain().msvc)

    def test_windows_environment_names_are_case_insensitive(self):
        with tempfile.TemporaryDirectory() as directory:
            root, compiler, environment, captured = self.windows_fixture(directory)
            environment = {key.upper(): value for key, value in environment.items()}
            with patch.object(fixture, "os", SimpleNamespace(name="nt", environ=environment)), \
                    patch.object(fixture.subprocess, "run", side_effect=[
                        subprocess.CompletedProcess([], 0, self.installation(root), ""),
                        subprocess.CompletedProcess([], 0, captured, "")]):
                self.assertEqual(fixture.select_toolchain().compiler, str(compiler))

    def test_windows_fails_if_no_complete_vs_instead_of_skipping_contract(self):
        with tempfile.TemporaryDirectory() as directory:
            _, _, environment, _ = self.windows_fixture(directory)
            with patch.object(fixture, "os", SimpleNamespace(name="nt", environ=environment)), \
                    patch.object(fixture.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "", "")):
                with self.assertRaisesRegex(RuntimeError, "complete VS 2022"):
                    fixture.select_toolchain()

    def test_windows_rejects_foreign_instance_environment(self):
        with tempfile.TemporaryDirectory() as directory:
            root, _, environment, captured = self.windows_fixture(directory)
            captured = captured.replace(str(root), str(Path(directory) / "wrong VS"))
            with patch.object(fixture, "os", SimpleNamespace(name="nt", environ=environment)), \
                    patch.object(fixture.subprocess, "run", side_effect=[
                        subprocess.CompletedProcess([], 0, self.installation(root), ""),
                        subprocess.CompletedProcess([], 0, captured, "")]):
                with self.assertRaisesRegex(RuntimeError, "invalid compiler"):
                    fixture.select_toolchain()

    def test_windows_rejects_unrelated_override_with_stl_version(self):
        with tempfile.TemporaryDirectory() as directory:
            root, _, environment, captured = self.windows_fixture(directory)
            environment["EAWR_CLANGXX"] = "unrelated-clang++"
            with patch.object(fixture, "os", SimpleNamespace(name="nt", environ=environment)), \
                    patch.object(fixture.shutil, "which", return_value=None), \
                    patch.object(fixture.subprocess, "run", side_effect=[
                        subprocess.CompletedProcess([], 0, self.installation(root), ""),
                        subprocess.CompletedProcess([], 0, captured, "")]):
                with self.assertRaisesRegex(RuntimeError, "unrelated-clang.*STL 14.44"):
                    fixture.select_toolchain()

    def test_posix_preserves_override_and_falls_back_to_gcc(self):
        with patch.object(fixture, "os", SimpleNamespace(name="posix", environ={"EAWR_CLANGXX": "/my/clang++"})):
            self.assertEqual(fixture.select_toolchain().compiler, "/my/clang++")
        with patch.object(fixture, "os", SimpleNamespace(name="posix", environ={})), \
                patch.object(fixture.shutil, "which", side_effect=[None, "/my/g++"]):
            self.assertEqual(fixture.select_toolchain().compiler, "/my/g++")
        with patch.object(fixture, "os", SimpleNamespace(name="posix", environ={})), \
                patch.object(fixture.shutil, "which", return_value=None):
            self.assertIsNone(fixture.select_toolchain())

    def test_compile_uses_matching_environment_and_keeps_objects_in_temporary_directory(self):
        source, executable = Path("/temporary/contract.cpp"), Path("/temporary/contract.exe")
        selected = fixture.Toolchain("cl.exe", {"INCLUDE": "matching"}, True, "14.44")
        with patch.object(fixture.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "", "")) as run:
            fixture.compile_fixture(selected, source, executable)
        self.assertEqual(run.call_args.kwargs["env"], selected.environment)
        self.assertEqual(run.call_args.kwargs["cwd"], source.parent)
        self.assertIn(f"/Fo:{source.with_suffix('.obj')}", run.call_args.args[0])
        self.assertIn("/std:c++20", run.call_args.args[0])
        self.assertIn("/EHsc", run.call_args.args[0])
        self.assertEqual(run.call_args.kwargs["timeout"], fixture.COMPILE_TIMEOUT)

    def test_compile_failure_names_both_versions_and_original_stl_diagnostic(self):
        selected = fixture.Toolchain("clang-cl.exe", {}, True, "14.51")
        with patch.object(fixture.subprocess, "run", side_effect=[
                subprocess.CompletedProcess([], 1, "", "STL1000: requires Clang 20 or newer"),
                subprocess.CompletedProcess([], 0, "clang version 19.1.5", "")]):
            with self.assertRaisesRegex(RuntimeError, "STL 14.51.*compatible") as caught:
                fixture.compile_fixture(selected, Path("/tmp/test.cpp"), Path("/tmp/test.exe"))
        self.assertIn("clang version 19.1.5", str(caught.exception))
        self.assertIn("STL1000", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
