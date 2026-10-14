"""Exercise the installed-data launcher and configure-time registration gate."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
LAUNCHER = ROOT / "tests/ci/run_game_data_test.py"
# These NONE-language projects never build. Supply a placeholder make program
# so the fixtures need neither a compiler nor a separately installed build tool.
CMAKE_GENERATOR = ["-G", "Unix Makefiles", f"-DCMAKE_MAKE_PROGRAM={sys.executable}"]


class GameDataTests(unittest.TestCase):
    def run_launcher(self, root):
        environment = dict(os.environ)
        environment.pop("EAWR_EAW_GAME_ROOT", None)
        if root is not None:
            environment["EAWR_EAW_GAME_ROOT"] = str(root)
        return subprocess.run(
            [sys.executable, str(LAUNCHER), sys.executable, "-c",
             "import os,sys; print('CHILD', os.environ['EAWR_EAW_GAME_ROOT'], sys.argv[1]); sys.exit(3)",
             "argument with spaces"], env=environment, capture_output=True, text=True)

    def test_missing_empty_and_nonexistent_roots_skip_without_running_child(self):
        with tempfile.TemporaryDirectory() as directory:
            for root in (None, "", Path(directory) / "missing"):
                with self.subTest(root=root):
                    result = self.run_launcher(root)
                    self.assertEqual(result.returncode, 77, result.stderr)
                    self.assertIn("SKIPPED:", result.stdout)
                    self.assertNotIn("CHILD", result.stdout)

    def test_supplied_root_preserves_arguments_environment_and_failure(self):
        with tempfile.TemporaryDirectory(prefix="game data ") as directory:
            result = self.run_launcher(directory)
            self.assertEqual(result.returncode, 3, result.stderr)
            self.assertIn(f"CHILD {directory} argument with spaces", result.stdout)
            self.assertNotIn("SKIPPED:", result.stdout)

    def test_configure_checks_nested_registrations_and_both_skip_properties(self):
        for property_text, accepted in (("", False), ("SKIP_RETURN_CODE 77", True),
                                        ('SKIP_REGULAR_EXPRESSION "SKIPPED:"', True)):
            with self.subTest(property_text=property_text), tempfile.TemporaryDirectory() as directory:
                source = Path(directory)
                (source / "nested").mkdir()
                (source / "CMakeLists.txt").write_text(
                    'cmake_minimum_required(VERSION 3.25)\nproject(SkipGate NONE)\n'
                    'enable_testing()\n'
                    f'include("{ROOT.as_posix()}/cmake/EawrGameDataTests.cmake")\n'
                    'add_subdirectory(nested)\neawr_check_game_data_tests("${CMAKE_SOURCE_DIR}")\n')
                (source / "nested/CMakeLists.txt").write_text(
                    'add_test(NAME installed COMMAND "${CMAKE_COMMAND}" -E true)\n'
                    f'set_tests_properties(installed PROPERTIES LABELS "slow;game-data" {property_text})\n')
                result = subprocess.run(["cmake", *CMAKE_GENERATOR, "-S", str(source), "-B", str(source / "build")],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode == 0, accepted, result.stdout + result.stderr)
                if not accepted:
                    self.assertIn("game-data test installed has no skip property", result.stderr)

    def test_registration_resolves_executable_target_and_ctest_reports_skip(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)
            (source / "child.py").write_text("print('CHILD executed')\nraise SystemExit(3)\n")
            (source / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.25)\nproject(Launcher NONE)\nenable_testing()\n'
                f'include("{ROOT.as_posix()}/cmake/EawrGameDataTests.cmake")\n'
                'add_executable(child IMPORTED)\n'
                f'set_property(TARGET child PROPERTY IMPORTED_LOCATION "{Path(sys.executable).as_posix()}")\n'
                'eawr_add_game_data_test(NAME installed COMMAND child "${CMAKE_SOURCE_DIR}/child.py")\n'
                'eawr_check_game_data_tests("${CMAKE_SOURCE_DIR}")\n')
            build = source / "build"
            configured = subprocess.run(["cmake", *CMAKE_GENERATOR, "-S", str(source), "-B", str(build)],
                                        capture_output=True, text=True)
            self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
            environment = dict(os.environ)
            environment.pop("EAWR_EAW_GAME_ROOT", None)
            skipped = subprocess.run(["ctest", "--test-dir", str(build), "-L", "game-data", "-V"],
                                     env=environment, capture_output=True, text=True)
            self.assertEqual(skipped.returncode, 0, skipped.stdout + skipped.stderr)
            self.assertIn("***Skipped", skipped.stdout)
            self.assertNotIn("CHILD executed", skipped.stdout)
            environment["EAWR_EAW_GAME_ROOT"] = directory
            failed = subprocess.run(["ctest", "--test-dir", str(build), "-L", "game-data", "-V"],
                                    env=environment, capture_output=True, text=True)
            self.assertNotEqual(failed.returncode, 0, failed.stdout)
            self.assertIn("CHILD executed", failed.stdout)


if __name__ == "__main__":
    unittest.main()
