"""Run the real CMake compiler-selection logic with a recording compiler stub."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class BoundaryConfigureTests(unittest.TestCase):
    def configure(self, windows=True, reject=False):
        source = (ROOT / "tests/core/CMakeLists.txt").read_text()
        block = source.split("if(EAWR_ENABLE_SIM_BOUNDARY_CHECK)", 1)[1].split(
            "add_test(NAME presentation_session_boundary", 1)[0]
        # Include the actual selection/registration block, without compiling or
        # configuring any production targets. Only external commands are stubbed.
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            toolset = directory / "VC/Tools/MSVC/test"
            (toolset / "include").mkdir(parents=True)
            (toolset / "include/yvals_core.h").touch()
            script = directory / "probe.cmake"
            script.write_text('''
cmake_minimum_required(VERSION 3.24)
function(execute_process)
    cmake_parse_arguments(CALL "" "RESULT_VARIABLE;OUTPUT_VARIABLE;ERROR_VARIABLE" "COMMAND" ${ARGV})
    file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/calls.txt" "${CALL_COMMAND}\\n")
    if(CALL_RESULT_VARIABLE)
        set(${CALL_RESULT_VARIABLE} ${REJECT} PARENT_SCOPE)
    endif()
    if(CALL_OUTPUT_VARIABLE)
        set(${CALL_OUTPUT_VARIABLE} "clang version 20.0.0" PARENT_SCOPE)
    endif()
endfunction()
function(add_test)
    file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/tests.txt" "${ARGV}\\n")
endfunction()
function(set_tests_properties)
    file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/properties.txt" "${ARGV}\\n")
endfunction()
set(WIN32 "${TEST_WINDOWS}")
set(EAWR_CLANGXX recording-clang)
set(Python3_EXECUTABLE recording-python)
set(CMAKE_CXX_COMPILER "${TOOLSET}/bin/Hostx64/x64/cl.exe")
set(PROJECT_SOURCE_DIR "${ROOT}")
set(CMAKE_CURRENT_SOURCE_DIR "${ROOT}/tests/core")
set(EAWR_ENABLE_SIM_BOUNDARY_CHECK ON)
if(EAWR_ENABLE_SIM_BOUNDARY_CHECK)
''' + block)
            environment = dict(os.environ)
            environment.pop("VCToolsInstallDir", None)
            result = subprocess.run([
                shutil.which("cmake") or "cmake", f"-DTEST_WINDOWS={'ON' if windows else 'OFF'}",
                f"-DREJECT={1 if reject else 0}", f"-DTOOLSET={toolset.as_posix()}",
                f"-DROOT={ROOT.as_posix()}", "-P", str(script),
            ], cwd=directory, env=environment, capture_output=True, text=True)
            return (result, (directory / "calls.txt").read_text(),
                    (directory / "tests.txt").read_text() if (directory / "tests.txt").exists() else "",
                    (directory / "properties.txt").read_text() if (directory / "properties.txt").exists() else "")

    def test_windows_probe_is_bounded_and_ctest_keeps_full_gate(self):
        result, calls, tests, properties = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        probe = calls.splitlines()[0]
        self.assertIn("VCToolsInstallDir=", probe)
        self.assertIn("--files;", probe)
        self.assertIn("sim_boundary_clang_probe.cpp;src/sim/world.cpp", probe)
        project = next(line for line in tests.splitlines() if "NAME;sim_boundary_project;" in line)
        self.assertIn("check_sim_boundary.py", project)
        self.assertNotIn("--files", project)
        self.assertIn("--jobs;4", project)
        self.assertIn("ENVIRONMENT;VCToolsInstallDir=", properties)

    def test_linux_retains_small_compiler_probe(self):
        result, calls, tests, _ = self.configure(windows=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("recording-clang;-std=c++20;-fsyntax-only", calls)
        self.assertNotIn("check_sim_boundary.py", calls)
        self.assertIn("NAME;sim_boundary_project;", tests)

    def test_incompatible_compiler_still_fails_configure(self):
        result, _, tests, _ = self.configure(reject=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("found no Clang", result.stderr)
        self.assertEqual(tests, "")


if __name__ == "__main__":
    unittest.main()
