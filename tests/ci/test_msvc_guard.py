"""The MSVC shift-guard probe (cmake/EawrMsvcGuard.cmake, legacy #3930 and #1287).

MSVC 19.51 at /O2 drops the guard of `n == -1 ? 0 : (1 << n)`; the module compiles and runs a probe of that
pattern at configure time. By default a compiler that fails it gets /Od on the affected Lua units only
(eawr_msvc_guard_apply), `EAWR_MSVC_GUARD_MODE=refuse` stops the configure instead, `off` skips the probe, and a
compiler that passes it changes no flag. These cases configure a tiny project that includes the module and reports
the compile options it put on a stand-in source."""

import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
CMAKE = shutil.which("cmake")
PROJECT = f"""cmake_minimum_required(VERSION 3.25)
project(guard_probe LANGUAGES CXX)
list(APPEND CMAKE_MODULE_PATH "{(ROOT / 'cmake').as_posix()}")
if(EAWR_TEST_CROSS)
    set(CMAKE_CROSSCOMPILING ON)
endif()
include(EawrMsvcGuard)
add_library(stand_in STATIC stand_in.cpp)
set(EAWR_TEST_APPLY_SOURCE "${{CMAKE_CURRENT_SOURCE_DIR}}/stand_in.cpp" CACHE FILEPATH "Source given to eawr_msvc_guard_apply")
eawr_msvc_guard_apply(stand_in "${{EAWR_TEST_APPLY_SOURCE}}")
get_property(_applied SOURCE stand_in.cpp TARGET_DIRECTORY stand_in PROPERTY COMPILE_OPTIONS)
message(STATUS "applied=[${{_applied}}]")
"""
MISCOMPILE_MESSAGE = "miscompiles `n == -1 ? 0 : (1 << n)` at /O2"


@unittest.skipUnless(CMAKE, "CMake is not available")
class MsvcGuardTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.source = pathlib.Path(self.tmp.name, "src")
        self.source.mkdir()
        (self.source / "CMakeLists.txt").write_text(PROJECT, encoding="utf-8")
        (self.source / "stand_in.cpp").write_text("int stand_in() { return 0; }\n", encoding="utf-8")
        self.failing_probe = self.source / "failing_probe.cpp"
        self.failing_probe.write_text(
            '#include <cstdio>\nint main() { std::puts("shift guard dropped (test probe)"); return 1; }\n',
            encoding="utf-8",
        )
        self.passing_probe = self.source / "passing_probe.cpp"
        self.passing_probe.write_text("int main() { return 0; }\n", encoding="utf-8")
        self.unbuildable_probe = self.source / "unbuildable_probe.cpp"
        self.unbuildable_probe.write_text("int main() { this is not C++ }\n", encoding="utf-8")
        self.count = 0

    def configure(self, *args):
        self.count += 1
        build = pathlib.Path(self.tmp.name, f"build{self.count}")
        done = subprocess.run(
            [CMAKE, "-S", str(self.source), "-B", str(build), *args],
            capture_output=True, text=True, timeout=600,
        )
        return done, " ".join((done.stdout + done.stderr).split())  # CMake wraps its text

    def test_a_failing_probe_gets_the_workaround_by_default(self):
        done, output = self.configure("-DEAWR_MSVC_GUARD_FORCE=ON", f"-DEAWR_MSVC_GUARD_PROBE_SOURCE={self.failing_probe}")
        self.assertEqual(done.returncode, 0, output)
        self.assertIn(MISCOMPILE_MESSAGE, output)
        self.assertIn("shift guard dropped (test probe)", output)
        self.assertIn("compiling the affected Lua units with /Od", output)
        self.assertIn("applied=[/Od]", output)

    def test_refuse_mode_still_refuses_with_the_documented_message(self):
        done, output = self.configure("-DEAWR_MSVC_GUARD_FORCE=ON", "-DEAWR_MSVC_GUARD_MODE=refuse",
                                      f"-DEAWR_MSVC_GUARD_PROBE_SOURCE={self.failing_probe}")
        self.assertNotEqual(done.returncode, 0, output)
        self.assertIn(MISCOMPILE_MESSAGE, output)
        self.assertIn("shift guard dropped (test probe)", output)
        self.assertIn("-DEAWR_MSVC_GUARD_MODE=refuse", output)
        self.assertNotIn("applied=", output)

    def test_a_fixed_compiler_is_left_untouched(self):
        for mode in ("workaround", "refuse"):
            with self.subTest(mode=mode):
                done, output = self.configure("-DEAWR_MSVC_GUARD_FORCE=ON", f"-DEAWR_MSVC_GUARD_MODE={mode}",
                                              f"-DEAWR_MSVC_GUARD_PROBE_SOURCE={self.passing_probe}")
                self.assertEqual(done.returncode, 0, output)
                self.assertNotIn(MISCOMPILE_MESSAGE, output)
                self.assertIn("applied=[]", output)

    def test_a_probe_that_cannot_be_built_fails_closed(self):
        probe = f"-DEAWR_MSVC_GUARD_PROBE_SOURCE={self.unbuildable_probe}"
        done, output = self.configure("-DEAWR_MSVC_GUARD_FORCE=ON", probe)
        self.assertEqual(done.returncode, 0, output)
        self.assertIn("did not build or run", output)
        self.assertIn("compile with /Od to be safe", output)
        self.assertIn("applied=[/Od]", output)
        done, output = self.configure("-DEAWR_MSVC_GUARD_FORCE=ON", "-DEAWR_MSVC_GUARD_MODE=refuse", probe)
        self.assertNotEqual(done.returncode, 0, output)
        self.assertIn("could not be checked", output)
        done, output = self.configure("-DEAWR_MSVC_GUARD_FORCE=ON", "-DEAWR_MSVC_GUARD_MODE=off", probe)
        self.assertEqual(done.returncode, 0, output)
        self.assertIn("applied=[]", output)

    def test_cross_builds_are_not_probed(self):
        done, output = self.configure("-DEAWR_MSVC_GUARD_FORCE=ON", "-DEAWR_TEST_CROSS=ON",
                                      f"-DEAWR_MSVC_GUARD_PROBE_SOURCE={self.failing_probe}")
        self.assertEqual(done.returncode, 0, output)
        self.assertNotIn(MISCOMPILE_MESSAGE, output)
        self.assertIn("applied=[]", output)

    def test_a_source_that_is_not_in_the_target_is_a_configure_error(self):
        for mode in ("workaround", "off"):
            with self.subTest(mode=mode):
                done, output = self.configure("-DEAWR_MSVC_GUARD_FORCE=ON", f"-DEAWR_MSVC_GUARD_MODE={mode}",
                                              f"-DEAWR_MSVC_GUARD_PROBE_SOURCE={self.passing_probe}",
                                              f"-DEAWR_TEST_APPLY_SOURCE={self.source / 'elsewhere.cpp'}")
                self.assertNotEqual(done.returncode, 0, output)
                self.assertIn("is not one of the sources of stand_in", output)

    def test_off_mode_skips_the_probe(self):
        done, output = self.configure("-DEAWR_MSVC_GUARD_FORCE=ON", "-DEAWR_MSVC_GUARD_MODE=off",
                                      f"-DEAWR_MSVC_GUARD_PROBE_SOURCE={self.failing_probe}")
        self.assertEqual(done.returncode, 0, output)
        self.assertNotIn(MISCOMPILE_MESSAGE, output)
        self.assertIn("applied=[]", output)

    def test_an_unknown_mode_is_rejected(self):
        done, output = self.configure("-DEAWR_MSVC_GUARD_MODE=sometimes")
        self.assertNotEqual(done.returncode, 0, output)
        self.assertIn("EAWR_MSVC_GUARD_MODE must be workaround, refuse or off", output)

    def test_the_real_probe_passes_or_gets_the_workaround(self):
        done, output = self.configure("-DEAWR_MSVC_GUARD_FORCE=ON")
        self.assertEqual(done.returncode, 0, output)
        if MISCOMPILE_MESSAGE in output:
            self.assertIn("computesizes narray=", output)
            self.assertIn("applied=[/Od]", output)
        else:
            self.assertIn("applied=[]", output)

    def test_the_real_probe_in_refuse_mode_passes_or_names_the_miscompile(self):
        done, output = self.configure("-DEAWR_MSVC_GUARD_FORCE=ON", "-DEAWR_MSVC_GUARD_MODE=refuse")
        if done.returncode == 0:
            self.assertNotIn(MISCOMPILE_MESSAGE, output)
        else:
            self.assertIn(MISCOMPILE_MESSAGE, output)
            self.assertIn("computesizes narray=", output)

    def test_other_compilers_are_not_probed_without_the_hook(self):
        done, output = self.configure(f"-DEAWR_MSVC_GUARD_PROBE_SOURCE={self.failing_probe}")
        self.assertEqual(done.returncode, 0, output)
        if "The CXX compiler identification is MSVC" in output:
            self.assertIn("applied=[/Od]", output)
        else:
            self.assertIn("applied=[]", output)


if __name__ == "__main__":
    unittest.main()
