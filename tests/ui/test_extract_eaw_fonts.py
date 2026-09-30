"""tools/fonts/extract_eaw_fonts.py: validated sfnt scan, pinned builds, ignored cache (#191).

The synthetic tests build their fonts in code (sfnt_fixture.py); no retail font
byte is committed. The repository checks make sure no font is tracked and the
cache stays ignored. The opt-in checks read the real executable under
EAWR_EAW_GAME_ROOT and pin the extracted sizes and SHA-256 digests only.
"""

import contextlib
import hashlib
import io
import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import uuid

ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "fonts" / "extract_eaw_fonts.py"
sys.path.insert(0, str(TOOL.parent))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import extract_eaw_fonts as tool  # noqa: E402
import sfnt_fixture as fixture  # noqa: E402

GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")
FACES = ("EmpireAtWar-Bold", "EmpireAtWar-Light", "EmpireAtWar-Medium", "EmpireAtWar-Stencil")
# The pinned FoC build and its faces: sizes and digests only.
PINNED_EXE = "07daeeaec9d1e751383b1db3f5847b6c66f68785440a6c52c87b85f723d018a4"
PINNED = {
    "EmpireAtWar-Bold": (65684, "3df279cf0063098dcd54156878bc7dc0a3c1010a30c790ffc55695cd799c5aec"),
    "EmpireAtWar-Light": (67148, "6ea18fea12c6536f7ce151ea42bda0a03a03451919fa239af2bb8a31012ce0aa"),
    "EmpireAtWar-Medium": (66580, "24a5c0bad551c136f2d0ef045ae319104cee0bc9df411acb088ad3c25459ae3c"),
    "EmpireAtWar-Stencil": (67632, "3086766b3daee145a7d5c3f9f39ec9dbd49ac66bfe739201424af919553251fc"),
}
FONT_SUFFIXES = (".ttf", ".otf", ".ttc", ".woff", ".woff2", ".fon", ".fnt")


def synthetic_faces():
    return {name: fixture.build_font(name, family=name.replace("-", " ")) for name in FACES}


def run_main(*arguments):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = tool.main([str(argument) for argument in arguments])
    return code, out.getvalue(), err.getvalue()


def table(font: bytes, tag: bytes):
    count = struct.unpack_from(">H", font, 4)[0]
    for index in range(count):
        name, _, offset, length = struct.unpack_from(">4sIII", font, 12 + 16 * index)
        if name == tag:
            return offset, length
    raise KeyError(tag)


def tables_of(font: bytes, drop=()):
    count = struct.unpack_from(">H", font, 4)[0]
    records = [struct.unpack_from(">4sIII", font, 12 + 16 * index) for index in range(count)]
    return {tag: font[offset:offset + length] for tag, _, offset, length in records if tag not in drop}


class Patched:
    """Temporarily pins a synthetic build: KNOWN_BUILDS and PINNED_FACES."""

    def __init__(self, digest=None, faces=None):
        self.digest, self.faces = digest, faces

    def __enter__(self):
        self.saved = dict(tool.KNOWN_BUILDS), dict(tool.PINNED_FACES)
        if self.digest:
            tool.KNOWN_BUILDS[self.digest] = "synthetic test build"
        if self.faces:
            tool.PINNED_FACES.update(self.faces)
        return self

    def __exit__(self, *_):
        tool.KNOWN_BUILDS.clear()
        tool.KNOWN_BUILDS.update(self.saved[0])
        tool.PINNED_FACES.clear()
        tool.PINNED_FACES.update(self.saved[1])


class SfntValidationTests(unittest.TestCase):
    def test_synthetic_font_validates_and_is_named(self):
        font = fixture.build_font("EmpireAtWar-Medium", family="Empire At War Medium")
        face = tool.validate_face(font)
        self.assertEqual(face.full_name, "EmpireAtWar-Medium")
        self.assertEqual(face.family, "Empire At War Medium")
        self.assertEqual(face.postscript_name, "EmpireAtWar-Medium")
        self.assertEqual(face.data, font)

    def assert_rejected(self, font: bytes, reason: str):
        with self.assertRaises(tool.SfntError) as caught:
            tool.validate_face(font)
        self.assertIn(reason, str(caught.exception))

    def test_a_changed_table_byte_fails_its_checksum(self):
        font = bytearray(fixture.build_font("EmpireAtWar-Bold"))
        offset, _ = table(bytes(font), b"glyf")
        font[offset + 3] ^= 0x40
        self.assert_rejected(bytes(font), "table 'glyf' checksum mismatch")

    def test_the_whole_font_adjustment_is_checked(self):
        font = bytearray(fixture.build_font("EmpireAtWar-Bold"))
        offset, _ = table(bytes(font), b"head")
        struct.pack_into(">I", font, offset + 8, struct.unpack_from(">I", font, offset + 8)[0] ^ 1)
        self.assert_rejected(bytes(font), "whole-font checksum adjustment mismatch")

    def test_a_font_without_required_tables_is_rejected(self):
        tables = tables_of(fixture.build_font("EmpireAtWar-Bold"), drop=(b"cmap", b"glyf"))
        self.assert_rejected(fixture.assemble(tables), "required tables missing: cmap, glyf")

    def test_a_head_without_its_magic_is_rejected(self):
        tables = tables_of(fixture.build_font("EmpireAtWar-Bold"))
        head = bytearray(tables[b"head"])
        struct.pack_into(">I", head, 12, 0)
        tables[b"head"] = bytes(head)
        self.assert_rejected(fixture.assemble(tables), "head table has no magic number")

    def test_a_truncated_font_is_rejected(self):
        font = fixture.build_font("EmpireAtWar-Bold")
        self.assert_rejected(font[:-64], "runs past the end of the file")

    def test_overlapping_tables_are_rejected(self):
        font = bytearray(fixture.build_font("EmpireAtWar-Bold"))
        # Point the second record at the first table's data.
        first = struct.unpack_from(">4sIII", font, 12)
        second = struct.unpack_from(">4sIII", font, 28)
        struct.pack_into(">4sIII", font, 28, second[0], second[1], first[2], second[3])
        self.assert_rejected(bytes(font), "overlap")

    def test_a_font_without_a_full_name_is_rejected(self):
        tables = tables_of(fixture.build_font("EmpireAtWar-Bold"))
        tables[b"name"] = fixture._name({1: "Empire At War Bold"})
        self.assert_rejected(fixture.assemble(tables), "no full name")

    def test_unsorted_or_unprintable_directories_are_not_fonts(self):
        font = bytearray(fixture.build_font("EmpireAtWar-Bold"))
        font[12:16] = b"\x01xyz"
        self.assertEqual(tool.scan(bytes(font)).faces, [])


class ScanTests(unittest.TestCase):
    def test_the_scan_finds_every_embedded_face_between_decoys(self):
        faces = synthetic_faces()
        found = tool.scan(fixture.fake_executable(list(faces.values())))
        self.assertEqual(sorted(face.full_name for face in found.faces), sorted(FACES))
        self.assertEqual({face.full_name: face.data for face in found.faces}, faces)

    def test_a_corrupt_copy_is_rejected_with_its_reason(self):
        faces = synthetic_faces()
        broken = bytearray(faces["EmpireAtWar-Stencil"])
        offset, _ = table(bytes(broken), b"loca")
        broken[offset] ^= 0x01
        found = tool.scan(fixture.fake_executable([faces["EmpireAtWar-Bold"], bytes(broken)]))
        self.assertEqual([face.full_name for face in found.faces], ["EmpireAtWar-Bold"])
        self.assertEqual(found.rejected, ["table 'loca' checksum mismatch"])


class CommandLineTests(unittest.TestCase):
    def setUp(self):
        self.work = pathlib.Path(tempfile.mkdtemp(prefix="eawr-fonts-"))
        self.faces = synthetic_faces()

    def tearDown(self):
        shutil.rmtree(self.work, ignore_errors=True)

    def install(self, fonts, name="StarWarsG.exe") -> pathlib.Path:
        corruption = self.work / "game" / "corruption"
        corruption.mkdir(parents=True, exist_ok=True)
        executable = corruption / name
        executable.write_bytes(fixture.fake_executable(fonts))
        return executable

    def test_an_unknown_executable_fails_clearly_and_writes_nothing(self):
        executable = self.install(list(self.faces.values()))
        out = self.work / "cache"
        code, stdout, stderr = run_main("--game-root", self.work / "game", "--out", out)
        self.assertEqual(code, tool.EXIT_UNKNOWN_BUILD)
        digest = hashlib.sha256(executable.read_bytes()).hexdigest()
        self.assertIn(f"unknown executable {executable}", stderr)
        self.assertIn(digest, stderr)
        self.assertIn("--allow-unknown-build", stderr)
        self.assertEqual(stdout, "")
        self.assertFalse(out.exists())

    def test_an_allowed_unknown_build_extracts_all_four_faces(self):
        self.install(list(self.faces.values()))
        out = self.work / "cache"
        code, stdout, stderr = run_main("--game-root", self.work / "game", "--out", out, "--allow-unknown-build")
        self.assertEqual(code, 0, stderr)
        for name, font in self.faces.items():
            self.assertEqual((out / f"{name}.ttf").read_bytes(), font)
            self.assertIn(f"{name} ({len(font)} bytes, sha256 {hashlib.sha256(font).hexdigest()}) differs "
                          "from the pinned face", stderr)
        manifest = json.loads((out / "fonts.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["schema"], "eawr-font-cache/1")
        self.assertIsNone(manifest["source"]["build"])
        self.assertEqual([face["face"] for face in manifest["faces"]], list(FACES))
        self.assertEqual([face["file"] for face in manifest["faces"]], [f"{name}.ttf" for name in FACES])
        self.assertFalse(any(face["pinned"] for face in manifest["faces"]))
        self.assertIn("wrote 4 faces", stdout)

    def test_a_planted_temporary_symlink_is_never_written_through(self):
        # The old writer used "<face>.ttf.tmp" and followed a symlink planted there.
        self.install(list(self.faces.values()))
        out = self.work / "cache"
        out.mkdir()
        victim = self.work / "victim.txt"
        victim.write_bytes(b"keep")
        planted = []
        for name in [f"{face}.ttf" for face in FACES] + ["fonts.json"]:
            link = out / f"{name}.tmp"
            try:
                link.symlink_to(victim)
            except (OSError, NotImplementedError):
                self.skipTest("this account cannot create symlinks")
            planted.append(link)
        code, _, stderr = run_main("--game-root", self.work / "game", "--out", out, "--allow-unknown-build")
        self.assertEqual(code, 0, stderr)
        self.assertEqual(victim.read_bytes(), b"keep")
        for name, font in self.faces.items():
            self.assertEqual((out / f"{name}.ttf").read_bytes(), font)
        self.assertEqual([link for link in planted if not link.is_symlink()], [])
        self.assertEqual(sorted(p.name for p in out.glob(".*.tmp")), [])

    def test_a_pinned_build_extracts_without_the_escape(self):
        executable = self.install(list(self.faces.values()))
        digest = hashlib.sha256(executable.read_bytes()).hexdigest()
        pins = {name: (len(font), hashlib.sha256(font).hexdigest()) for name, font in self.faces.items()}
        out = self.work / "cache"
        with Patched(digest, pins):
            code, stdout, stderr = run_main("--exe", executable, "--out", out, "--json")
        self.assertEqual(code, 0, stderr)
        self.assertEqual(stderr, "")
        summary = json.loads(stdout)
        self.assertEqual(summary["source"], {"file": "StarWarsG.exe", "sha256": digest,
                                             "bytes": executable.stat().st_size, "build": "synthetic test build"})
        self.assertTrue(all(face["pinned"] for face in summary["faces"]))
        self.assertEqual(json.loads((out / "fonts.json").read_text(encoding="utf-8")), summary)

    def test_a_pinned_build_whose_face_differs_fails(self):
        executable = self.install(list(self.faces.values()))
        digest = hashlib.sha256(executable.read_bytes()).hexdigest()
        with Patched(digest):
            code, _, stderr = run_main("--exe", executable, "--out", self.work / "cache")
        self.assertEqual(code, tool.EXIT_FACES)
        self.assertIn("differs from the pinned face", stderr)
        self.assertFalse((self.work / "cache").exists())

    def test_a_missing_face_is_named(self):
        self.install([self.faces[name] for name in FACES[:3]])
        code, _, stderr = run_main("--game-root", self.work / "game", "--out", self.work / "cache",
                                   "--allow-unknown-build")
        self.assertEqual(code, tool.EXIT_FACES)
        self.assertIn("missing faces: EmpireAtWar-Stencil", stderr)
        self.assertIn("valid fonts found: EmpireAtWar-Bold, EmpireAtWar-Light, EmpireAtWar-Medium", stderr)

    def test_two_different_fonts_with_one_name_are_ambiguous(self):
        other = fixture.build_font("EmpireAtWar-Light", family="Other")
        self.install(list(self.faces.values()) + [other])
        code, _, stderr = run_main("--game-root", self.work / "game", "--out", self.work / "cache",
                                   "--allow-unknown-build")
        self.assertEqual(code, tool.EXIT_FACES)
        self.assertIn("2 different fonts are named EmpireAtWar-Light", stderr)

    def test_an_identical_second_copy_is_not_ambiguous(self):
        self.install(list(self.faces.values()) + [self.faces["EmpireAtWar-Light"]])
        code, _, stderr = run_main("--game-root", self.work / "game", "--out", self.work / "cache",
                                   "--allow-unknown-build", "--dry-run")
        self.assertEqual(code, 0, stderr)
        self.assertFalse((self.work / "cache").exists())

    def test_inputs_that_are_not_a_foc_executable(self):
        text = self.work / "notes.txt"
        text.write_text("not an executable", encoding="utf-8")
        code, _, stderr = run_main("--exe", text)
        self.assertEqual(code, tool.EXIT_USAGE)
        self.assertIn("is not a Windows executable", stderr)
        (self.work / "eaw" / "GameData").mkdir(parents=True)
        code, _, stderr = run_main("--game-root", self.work / "eaw")
        self.assertEqual(code, tool.EXIT_USAGE)
        self.assertIn("no corruption/StarWarsG.exe", stderr)
        self.assertIn("Forces of Corruption only", stderr)
        code, _, stderr = run_main("--exe", self.work / "missing.exe")
        self.assertEqual(code, tool.EXIT_USAGE)
        self.assertIn("cannot read", stderr)

    def test_the_corruption_folder_is_found_in_any_case(self):
        corruption = self.work / "game" / "CORRUPTION"
        corruption.mkdir(parents=True)
        (corruption / "starwarsg.exe").write_bytes(fixture.fake_executable(list(self.faces.values())))
        self.assertEqual(tool.locate_executable(self.work / "game").name.lower(), "starwarsg.exe")
        self.assertEqual(tool.locate_executable(corruption).name.lower(), "starwarsg.exe")

    def test_a_tracked_output_directory_in_the_repository_is_refused(self):
        if not (ROOT / ".git").exists() or shutil.which("git") is None:
            self.skipTest("needs a git checkout")
        self.install(list(self.faces.values()))
        out = ROOT / "tools" / "fonts" / f"refused-{uuid.uuid4().hex[:8]}"
        code, _, stderr = run_main("--game-root", self.work / "game", "--out", out, "--allow-unknown-build")
        self.assertEqual(code, tool.EXIT_OUTPUT)
        self.assertIn("inside this repository but not git-ignored", stderr)
        self.assertFalse(out.exists())

    def test_the_ignored_out_tree_is_accepted(self):
        self.install(list(self.faces.values()))
        out = ROOT / "out" / "test-fonts" / uuid.uuid4().hex[:8]
        try:
            code, _, stderr = run_main("--game-root", self.work / "game", "--out", out, "--allow-unknown-build")
            self.assertEqual(code, 0, stderr)
            self.assertEqual(sorted(path.name for path in out.iterdir()),
                             sorted([f"{name}.ttf" for name in FACES] + ["fonts.json"]))
        finally:
            shutil.rmtree(out, ignore_errors=True)
            with contextlib.suppress(OSError):
                out.parent.rmdir()

    def test_output_never_carries_font_bytes(self):
        self.install(list(self.faces.values()))
        result = subprocess.run([sys.executable, str(TOOL), "--game-root", str(self.work / "game"), "--out",
                                 str(self.work / "cache"), "--allow-unknown-build"],
                                capture_output=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr)
        printed = result.stdout + result.stderr
        self.assertTrue(printed.isascii())
        for font in self.faces.values():
            offset, length = table(font, b"glyf")
            sample = font[offset:offset + min(length, 32)]
            self.assertNotIn(sample, printed)
            self.assertNotIn(sample.hex().encode(), printed.lower())


class RepositoryTests(unittest.TestCase):
    """No font is tracked, and the cache (or any font file) stays ignored."""

    def setUp(self):
        if not (ROOT / ".git").exists() or shutil.which("git") is None:
            self.skipTest("needs a git checkout")

    def git(self, *arguments, stdin=None):
        return subprocess.run(["git", "-C", str(ROOT), *arguments], input=stdin, capture_output=True, timeout=120)

    def test_gitignore_covers_the_cache_and_font_files(self):
        paths = [f"out/fonts/{name}.ttf" for name in FACES] + ["out/fonts/fonts.json",
                                                              "apps/viewer/project/fonts/EmpireAtWar-Bold.ttf",
                                                              "tools/fonts/EmpireAtWar-Medium.otf"]
        result = self.git("check-ignore", "--no-index", "-z", "--stdin", stdin="\0".join(paths).encode() + b"\0")
        self.assertIn(result.returncode, (0, 1), result.stderr)
        self.assertEqual(sorted(filter(None, result.stdout.decode().split("\0"))), sorted(paths))

    def test_no_font_file_is_tracked(self):
        result = self.git("ls-files", "-z")
        self.assertEqual(result.returncode, 0, result.stderr)
        tracked = [name for name in result.stdout.decode("utf-8").split("\0") if name]
        self.assertGreater(len(tracked), 100)
        self.assertEqual([name for name in tracked if name.lower().endswith(FONT_SUFFIXES)], [])
        fonts = []
        for name in tracked:
            path = ROOT / name
            if not path.is_file():
                continue
            with path.open("rb") as handle:
                start = handle.read(4096)
            # A TrueType directory, a collection or a web font at the start of any tracked file.
            if tool._directory(start, 0) is not None or start[:4] in (b"ttcf", b"wOFF", b"wOF2", b"OTTO"):
                fonts.append(name)
        self.assertEqual(fonts, [])


@unittest.skipUnless(GAME_ROOT, "set EAWR_EAW_GAME_ROOT to check the real FoC executable")
class RealExecutableTests(unittest.TestCase):
    def setUp(self):
        self.executable = tool.locate_executable(pathlib.Path(GAME_ROOT))
        self.digest = hashlib.sha256(self.executable.read_bytes()).hexdigest()

    def summary(self, *extra):
        code, stdout, stderr = run_main("--exe", self.executable, "--dry-run", "--json", *extra)
        return code, (json.loads(stdout) if code == 0 else None), stderr

    def assert_pinned_faces(self, summary):
        self.assertEqual({face["face"]: (face["bytes"], face["sha256"]) for face in summary["faces"]}, PINNED)

    def test_the_foc_executable_yields_the_four_pinned_faces(self):
        if self.digest != PINNED_EXE:
            code, _, stderr = self.summary()
            self.assertEqual(code, tool.EXIT_UNKNOWN_BUILD, stderr)
            code, summary, stderr = self.summary("--allow-unknown-build")
            self.assertEqual(code, 0, stderr)
            self.assertEqual([face["face"] for face in summary["faces"]], list(FACES))
            return
        code, summary, stderr = self.summary()
        self.assertEqual(code, 0, stderr)
        self.assertEqual(summary["source"]["sha256"], PINNED_EXE)
        self.assert_pinned_faces(summary)

    def test_extracted_files_validate_again(self):
        with tempfile.TemporaryDirectory(prefix="eawr-fonts-") as temporary:
            code, _, stderr = run_main("--exe", self.executable, "--out", temporary, "--allow-unknown-build")
            self.assertEqual(code, 0, stderr)
            for name in FACES:
                face = tool.validate_face((pathlib.Path(temporary) / f"{name}.ttf").read_bytes())
                self.assertEqual(face.full_name, name)
                self.assertEqual(face.family, name.replace("EmpireAtWar-", "Empire At War "))

    def test_the_base_game_executable_is_unknown(self):
        base = tool._child(pathlib.Path(GAME_ROOT), "GameData")
        executable = tool._child(base, "StarWarsG.exe") if base else None
        if executable is None:
            self.skipTest("no GameData/StarWarsG.exe")
        code, _, stderr = run_main("--exe", executable, "--dry-run")
        self.assertEqual(code, tool.EXIT_UNKNOWN_BUILD)
        self.assertIn("unknown executable", stderr)


if __name__ == "__main__":
    unittest.main()
