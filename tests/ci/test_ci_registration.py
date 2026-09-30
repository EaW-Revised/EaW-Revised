"""Every tests/ci test file runs in CTest (and so in CI and the offload builds).

The tests of tools/offload, tools/common and tools/rig live here, not next to the tools,
so `unittest discover -s tools/offload` finds none; CTest runs them through
tests/ci/CMakeLists.txt, one add_test per file. This guard fails when a test file is
not registered there, or when a registered file would pass without running a test
(no unittest.main(), or no test case at all)."""

import pathlib
import re
import sys
import unittest

HERE = pathlib.Path(__file__).resolve().parent
# Files with their own entry point instead of unittest.main().
SCRIPTS = {"test_provision_linux_runner.py"}


def registered() -> set[str]:
    text = (HERE / "CMakeLists.txt").read_text(encoding="utf-8")
    return set(re.findall(r"\$\{CMAKE_CURRENT_SOURCE_DIR\}/(test_[A-Za-z0-9_]+\.py)", text))


# The adapter's tests live beside the tools they test; tests/ci/CMakeLists.txt runs them as modules.
OUTSIDE = (("tests/presentation/acceptance", "test_p1_12_*.py"), ("tests/validation/p1_capture", "test_foc_*.py"))


class RegistrationTests(unittest.TestCase):
    def test_every_adapter_test_file_is_registered_with_ctest(self):
        text = (HERE / "CMakeLists.txt").read_text(encoding="utf-8")
        for directory, pattern in OUTSIDE:
            for path in sorted((HERE.parent.parent / directory).glob(pattern)):
                module = f"{directory.replace('/', '.')}.{path.stem}"
                with self.subTest(module=module):
                    self.assertRegex(text, rf"eawr_add_(sharded_)?adapter_test\([A-Za-z0-9_]+ {re.escape(module)}[ )]",
                                     "add it to the adapter tests in tests/ci/CMakeLists.txt")

    def test_every_test_file_is_registered_with_ctest(self):
        files = {p.name for p in HERE.glob("test_*.py")}
        self.assertEqual(sorted(files - registered()), [], "add an add_test for these to tests/ci/CMakeLists.txt")
        self.assertEqual(sorted(registered() - files), [], "tests/ci/CMakeLists.txt names files that do not exist")

    def test_every_registered_file_runs_tests(self):
        sys.path.insert(0, str(HERE))
        loader = unittest.TestLoader()
        for name in sorted(registered() - SCRIPTS):
            with self.subTest(file=name):
                self.assertIn("unittest.main(", (HERE / name).read_text(encoding="utf-8"),
                              "run as a script by CTest, it must call unittest.main()")
                module = __import__(name[:-3])
                self.assertGreater(loader.loadTestsFromModule(module).countTestCases(), 0)


if __name__ == "__main__":
    unittest.main()
