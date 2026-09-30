"""The boundary checks' result cache (tools/boundary_cache.py) reuses a result only for unchanged inputs."""

import os
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from boundary_cache import BoundaryCache, parse_depfile  # noqa: E402


class DepfileTests(unittest.TestCase):
    def test_windows_and_posix_depfiles(self):
        windows = ("fog.o: src\\sim\\fog.cpp include\\eawr\\sim\\fog.hpp \\\r\n"
                   "  C:\\Program\\ Files\\ (x86)\\VC\\include\\cstdint \\\r\n"
                   "  C:\\Program\\ Files\\LLVM\\lib\\clang\\23\\include\\yvals_core.h\r\n")
        self.assertEqual(parse_depfile(windows), [
            "src\\sim\\fog.cpp", "include\\eawr\\sim\\fog.hpp",
            "C:\\Program Files (x86)\\VC\\include\\cstdint",
            "C:\\Program Files\\LLVM\\lib\\clang\\23\\include\\yvals_core.h"])
        posix = "unit.o: /src/a\\ b.cpp /usr/include/c++/14/vector \\\n /cost$$.h\n"
        self.assertEqual(parse_depfile(posix), ["/src/a b.cpp", "/usr/include/c++/14/vector", "/cost$.h"])
        self.assertEqual(parse_depfile(""), [])


class CacheTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(self.tmp.name).resolve()
        self.root = base / "repo"
        (self.root / "include" / "eawr").mkdir(parents=True)
        (self.root / "src").mkdir()
        self.unit = self.root / "src" / "unit.cpp"
        self.header = self.root / "include" / "eawr" / "unit.hpp"
        self.unit.write_text('#include "eawr/unit.hpp"\n', encoding="utf-8")
        self.header.write_text("int value;\n", encoding="utf-8")
        self.generated = base / "generated"
        self.generated.mkdir()
        (self.generated / "table.inc").write_text("1,\n", encoding="utf-8")
        self.cache_dir = base / "cache"
        self.depfile = base / "unit.d"
        self.depfile.write_text(
            f"unit.o: {self.unit} {self.header} \\\n {self.generated / 'table.inc'}\n".replace(" (", "\\ ("),
            encoding="utf-8")

    def tearDown(self):
        self.tmp.cleanup()

    def cache(self, identity=None, generated=None):
        generated = generated or self.generated
        return BoundaryCache(self.cache_dir, self.root, identity or {"clang": "x"},
                             include_dirs=(self.root / "include", generated), aliases={generated: "<generated>"})

    def stored(self):
        cache = self.cache()
        cache.store({"file": "src/unit.cpp"}, self.depfile, [["src/unit.cpp", 1, 1, "CODE", "message"]])
        return cache

    def test_unchanged_inputs_reuse_the_result(self):
        self.assertIsNone(self.cache().lookup({"file": "src/unit.cpp"}))
        self.stored()
        cache = self.cache()
        self.assertEqual(cache.lookup({"file": "src/unit.cpp"}), [["src/unit.cpp", 1, 1, "CODE", "message"]])
        self.assertEqual((cache.hits, cache.misses), (1, 0))

    def test_a_changed_header_unit_or_generated_file_is_a_miss(self):
        for path in (self.header, self.unit, self.generated / "table.inc"):
            with self.subTest(path=path.name):
                self.stored()
                original = path.read_text(encoding="utf-8")
                path.write_text(original + "// edit\n", encoding="utf-8")
                self.assertIsNone(self.cache().lookup({"file": "src/unit.cpp"}))
                path.write_text(original, encoding="utf-8")

    def test_the_generated_directory_may_move(self):
        self.stored()
        moved = pathlib.Path(self.tmp.name).resolve() / "generated-2"
        os.rename(self.generated, moved)
        self.assertIsNotNone(self.cache(generated=moved).lookup({"file": "src/unit.cpp"}))

    def test_a_new_file_that_could_shadow_an_include_is_a_miss(self):
        for directory in (self.root / "src", self.root / "include", self.root / "include" / "eawr"):
            with self.subTest(directory=directory.name):
                self.stored()
                shadow = directory / "shadow.hpp"
                shadow.write_text("", encoding="utf-8")
                self.assertIsNone(self.cache().lookup({"file": "src/unit.cpp"}))
                shadow.unlink()

    def test_new_intermediate_directory_in_earlier_include_root_is_a_miss(self):
        earlier = self.root / "first"
        later = self.root / "second"
        (earlier / "pkg").mkdir(parents=True)
        (later / "pkg" / "deep").mkdir(parents=True)
        selected = later / "pkg" / "deep" / "header.hpp"
        selected.write_text("int selected;\n", encoding="utf-8")
        self.depfile.write_text(f"unit.o: {self.unit} {selected}\n", encoding="utf-8")

        def cache():
            return BoundaryCache(self.cache_dir, self.root, {"clang": "shadow"},
                                 include_dirs=(earlier, later))

        key = {"file": "src/unit.cpp", "case": "intermediate-shadow"}
        cache().store(key, self.depfile, "pass")
        self.assertEqual(cache().lookup(key), "pass")
        (earlier / "pkg" / "deep").mkdir()
        (earlier / "pkg" / "deep" / "header.hpp").write_text("float shadow;\n", encoding="utf-8")
        self.assertIsNone(cache().lookup(key))

    def test_files_no_include_could_name_do_not_matter(self):
        # A test's __pycache__ (a build host's git clean removes it again) or a script next to a header.
        self.stored()
        (self.root / "src" / "__pycache__").mkdir()
        (self.root / "include" / "eawr" / "tool.py").write_text("", encoding="utf-8")
        self.assertIsNotNone(self.cache().lookup({"file": "src/unit.cpp"}))
        (self.root / "include" / "eawr" / "vector").write_text("", encoding="utf-8")
        self.assertIsNone(self.cache().lookup({"file": "src/unit.cpp"}))

    def test_a_deleted_dependency_another_key_or_identity_is_a_miss(self):
        self.stored()
        self.assertIsNone(self.cache().lookup({"file": "src/other.cpp"}))
        self.assertIsNone(self.cache({"clang": "y"}).lookup({"file": "src/unit.cpp"}))
        self.header.unlink()
        self.assertIsNone(self.cache().lookup({"file": "src/unit.cpp"}))

    def test_a_damaged_entry_is_a_miss_and_no_directory_means_no_cache(self):
        self.stored()
        for entry in self.cache_dir.iterdir():
            entry.write_text("{", encoding="utf-8")
        self.assertIsNone(self.cache().lookup({"file": "src/unit.cpp"}))
        off = BoundaryCache(None, self.root, {})
        off.store({"file": "src/unit.cpp"}, self.depfile, [])
        self.assertIsNone(off.lookup({"file": "src/unit.cpp"}))


if __name__ == "__main__":
    unittest.main()
