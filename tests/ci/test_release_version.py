"""The one release version: config/release-version.json, its CMake reader and the derived version strings."""
import importlib.util
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/release"))
import release_version as rv  # noqa: E402
import update_manifest  # noqa: E402

CMAKE = os.environ.get("EAWR_CMAKE_COMMAND") or shutil.which("cmake")


def configure_version(tree: pathlib.Path) -> str:
    """Configure a stand-in project that takes its VERSION from the tree's release version include."""
    (tree / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.25)\n'
        'include("${CMAKE_CURRENT_LIST_DIR}/cmake/EawrReleaseVersion.cmake")\n'
        'project(Stand VERSION ${EAWR_RELEASE_VERSION} LANGUAGES NONE)\n'
        'message(STATUS "PROJECT_VERSION=${PROJECT_VERSION}")\n', encoding="utf-8")
    result = subprocess.run([CMAKE, "-S", str(tree), "-B", str(tree / "b")], capture_output=True, text=True, check=False)
    if result.returncode:
        raise ValueError(result.stdout + result.stderr)
    return re.search(r"PROJECT_VERSION=(\S+)", result.stdout).group(1)


class ReleaseVersionTests(unittest.TestCase):
    def stand_in(self, text: str) -> pathlib.Path:
        tree = pathlib.Path(tempfile.mkdtemp(prefix="eawr-version-"))
        self.addCleanup(shutil.rmtree, tree, True)
        (tree / "cmake").mkdir()
        (tree / "config").mkdir()
        shutil.copyfile(ROOT / "cmake/EawrReleaseVersion.cmake", tree / "cmake/EawrReleaseVersion.cmake")
        (tree / "config/release-version.json").write_text(text, encoding="utf-8")
        return tree

    def test_committed_file_is_valid(self):
        self.assertRegex(rv.read(), r"^\d+\.\d+\.\d+$")

    def test_cmake_project_version_follows_the_file(self):
        self.assertTrue(CMAKE, "cmake is needed to prove the CMake reader")
        self.assertEqual(configure_version(self.stand_in('{"version": "4.5.6"}')), "4.5.6")
        self.assertEqual(configure_version(self.stand_in('{\r\n  "version": "0.3.3"\r\n}\r\n')), "0.3.3")

    def test_cmake_refuses_a_bad_file(self):
        self.assertTrue(CMAKE)
        for text in ('{"version": "0.3"}', '{"version": "0.3.3c"}', '{"release": "0.3.3"}', '{"version": 3}', 'not json'):
            with self.subTest(text=text), self.assertRaisesRegex(ValueError, "release-version.json|JSON"):
                configure_version(self.stand_in(text))

    def test_byte_order_mark_is_accepted_by_both_readers(self):
        self.assertTrue(CMAKE)
        text = chr(0xFEFF) + '{"version": "1.2.3"}'
        self.assertEqual(configure_version(self.stand_in(text)), "1.2.3")
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "release-version.json"
            path.write_bytes(text.encode("utf-8"))
            self.assertEqual(rv.read(path), "1.2.3")

    def test_missing_file_is_refused(self):
        self.assertTrue(CMAKE)
        tree = self.stand_in('{"version": "1.2.3"}')
        (tree / "config/release-version.json").unlink()
        with self.assertRaisesRegex(ValueError, "release-version.json"):
            configure_version(tree)
        with self.assertRaises(OSError):
            rv.read(tree / "config/release-version.json")

    def test_launcher_resources_follow_the_release_version(self):
        self.assertTrue(CMAKE)
        launcher = ROOT / "apps/launcher"
        cmake_text = (launcher / "CMakeLists.txt").read_text(encoding="utf-8")
        comma = 'set(EAWR_VERSION_COMMA "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")'
        dotted = 'set(EAWR_VERSION_DOTTED "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.0")'
        self.assertIn(comma, cmake_text)
        self.assertIn(dotted, cmake_text)
        for name in ("eawr_launcher.rc", "mods_start.rc", "eawr_launcher.manifest"):
            self.assertFalse((launcher / "src" / name).exists(), f"{name} must be generated, not committed")
            self.assertIn(f"configure_file(src/{name}.in", cmake_text)
        tree = self.stand_in('{"version": "4.5.6"}')
        for name in ("eawr_launcher.rc.in", "mods_start.rc.in", "eawr_launcher.manifest.in"):
            shutil.copyfile(launcher / "src" / name, tree / name)
        lines = ['cmake_minimum_required(VERSION 3.25)',
                 'include("${CMAKE_CURRENT_LIST_DIR}/cmake/EawrReleaseVersion.cmake")',
                 'project(Stand VERSION ${EAWR_RELEASE_VERSION} LANGUAGES NONE)', comma, dotted,
                 'foreach(name eawr_launcher.rc mods_start.rc eawr_launcher.manifest)',
                 '    configure_file(${name}.in ${CMAKE_CURRENT_BINARY_DIR}/out/${name} @ONLY)',
                 'endforeach()']
        (tree / "CMakeLists.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
        result = subprocess.run([CMAKE, "-S", str(tree), "-B", str(tree / "b")], capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for name in ("eawr_launcher.rc", "mods_start.rc"):
            text = (tree / "b/out" / name).read_text(encoding="utf-8")
            self.assertIn("FILEVERSION 4,5,6,0", text)
            self.assertIn("PRODUCTVERSION 4,5,6,0", text)
            self.assertIn('VALUE "FileVersion", "4.5.6"', text)
            self.assertIn('VALUE "ProductVersion", "4.5.6"', text)
            self.assertNotIn("@", text)
        manifest = (tree / "b/out/eawr_launcher.manifest").read_text(encoding="utf-8")
        self.assertIn('version="4.5.6.0"', manifest)
        self.assertNotIn("@", manifest)

    def test_both_projects_read_the_file_before_project(self):
        for path in ("CMakeLists.txt", "apps/viewer/CMakeLists.txt"):
            with self.subTest(path=path):
                text = (ROOT / path).read_text(encoding="utf-8")
                include = text.index("EawrReleaseVersion.cmake")
                project = re.search(r"^project\(", text, re.M).start()
                self.assertLess(include, project)
                self.assertIn("VERSION ${EAWR_RELEASE_VERSION}", text[project:project + 200])
                self.assertIsNone(re.search(r"project\([^)]*VERSION\s+\d", text, re.S))

    def test_python_reader_rejects_other_shapes(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "release-version.json"
            for text in ('{"version": "0.3"}', '{"version": "0.3.3", "extra": 1}', '[]', '{"version": 3}'):
                with self.subTest(text=text):
                    path.write_text(text, encoding="utf-8")
                    with self.assertRaises(ValueError):
                        rv.read(path)
            path.write_text(json.dumps({"version": "1.2.3"}), encoding="utf-8")
            self.assertEqual(rv.read(path), "1.2.3")

    def test_kit_label_is_version_plus_suffix(self):
        self.assertEqual(rv.kit_label("", "0.3.3"), "0.3.3")
        self.assertEqual(rv.kit_label("c-clang", "0.3.3"), "0.3.3c-clang")
        for bad in ("a b", "x" * 25, "../x", "é"):
            with self.subTest(suffix=bad), self.assertRaises(ValueError):
                rv.kit_label(bad, "0.3.3")

    def test_label_must_start_with_the_version_without_extending_it(self):
        self.assertEqual(rv.check_label("0.3.3c", "0.3.3"), "c")
        self.assertEqual(rv.check_label("0.3.3", "0.3.3"), "")
        for bad in ("0.3.2c", "10.3.3c", "0.3.33", "0.3.3 c", "", "0.3.3" + "c" * 30):
            with self.subTest(label=bad), self.assertRaises(ValueError):
                rv.check_label(bad, "0.3.3")

    def test_kit_version_is_a_valid_manifest_version(self):
        commit = "4c7035bb" + "0" * 32
        for label in ("0.3.3", "0.3.3c-clang", "0.3.3a"):
            version = rv.kit_version(label, commit, "0.3.3")
            with self.subTest(version=version):
                self.assertEqual(version, f"{label}-4c7035bb")
                update_manifest.split_version(version)
        with self.assertRaises(ValueError):
            rv.kit_version("0.3.3c", "4c7035bb", "0.3.3")

    def test_tag_must_equal_v_plus_version(self):
        rv.check_tag("v0.3.3", "0.3.3")
        for bad in ("0.3.3", "v0.3.2", "v0.3.3-rc1", "V0.3.3"):
            with self.subTest(tag=bad), self.assertRaises(ValueError):
                rv.check_tag(bad, "0.3.3")

    def test_command_line(self):
        run = lambda *args: subprocess.run([sys.executable, str(ROOT / "tools/release/release_version.py"), *args],
                                           capture_output=True, text=True, check=False)
        self.assertEqual(run().stdout.strip(), rv.read())
        self.assertEqual(run("check-tag", "v" + rv.read()).returncode, 0)
        self.assertEqual(run("check-tag", "v0.0.1").returncode, 1)
        self.assertEqual(run("label", "c").stdout.strip(), rv.read() + "c")
        self.assertEqual(run("bogus").returncode, 2)

    def test_release_docs_point_at_the_file(self):
        text = (ROOT / "docs/releasing.md").read_text(encoding="utf-8")
        self.assertIn("config/release-version.json", text)
        self.assertNotIn("the next candidate is v0.3.2", text)


if __name__ == "__main__":
    unittest.main()
