"""The user-side HUD movie conversion (tools/ui/convert_hud_movie.py, #237).

Synthetic only: hud_movie_prepare and FFmpeg are replaced by stand-ins, so the
orchestration, diagnostics and cache rules are checked without game data. When
an FFmpeg with libtheora is on PATH, one test also packs a generated alpha
clip for real (no Bink is needed: FFmpeg has no Bink encoder).
"""
import contextlib
import importlib.util
import io
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("convert_hud_movie", ROOT / "tools/ui/convert_hud_movie.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

ALPHA_KEY = "a" * 64 + "-hud-v1-alpha"
RGB_KEY = "b" * 64 + "-hud-v1-rgb"
CAPABLE = " V....D binkvideo            Bink video\n V....D libtheora            libtheora Theora\n"


def completed(args, stdout="", returncode=0, stderr=""):
    return subprocess.CompletedProcess(args, returncode, stdout=stdout, stderr=stderr)


class FakeTools:
    """Stands in for hud_movie_prepare and FFmpeg; records every command."""

    def __init__(self, cache, rows, capabilities=CAPABLE, encode=b"OggS encoded", fail_on=None):
        self.cache, self.rows, self.capabilities = cache, rows, capabilities
        self.encode, self.fail_on, self.calls = encode, fail_on, []

    def __call__(self, args, **kwargs):
        self.calls.append(list(args))
        program = pathlib.Path(args[0]).name
        if program == "prepare":
            lines = []
            for name, key, state in self.rows:
                if state == "extracted":
                    (self.cache / f"{key}.bik").write_bytes(b"BIKi " + name.encode())
                lines.append(f"{name}\t{key}\t{state}")
            return completed(args, "\n".join(lines) + "\n")
        if "-decoders" in args or "-encoders" in args:
            return completed(args, self.capabilities)
        if self.fail_on and any(self.fail_on in str(arg) for arg in args):
            raise subprocess.CalledProcessError(1, args, stderr="Invalid data found when processing input\n")
        if "-y" in args:
            pathlib.Path(args[-1]).write_bytes(self.encode)
        return completed(args)


class Conversion(unittest.TestCase):
    def setUp(self):
        self.directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-movie-convert-"))
        self.cache = self.directory / "cache"
        self.cache.mkdir()

    def tearDown(self):
        shutil.rmtree(self.directory, ignore_errors=True)

    def main(self, fake, *names, ffmpeg="ffmpeg"):
        argv = ["--prepare", "prepare", "--game-root", "game", "--cache", str(self.cache)]
        if ffmpeg:
            argv += ["--ffmpeg", ffmpeg]
        for name in names:
            argv += ["--name", name]
        errors = io.StringIO()
        with mock.patch.object(MODULE.subprocess, "run", side_effect=fake), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(errors):
            code = MODULE.main(argv)
        return code, errors.getvalue()

    def test_alpha_movies_are_packed_side_by_side_and_verified(self):
        fake = FakeTools(self.cache, [("Portrait_Loop", ALPHA_KEY, "extracted"), ("Plain", RGB_KEY, "extracted")])
        code, _ = self.main(fake, "Portrait_Loop", "Plain")
        self.assertEqual(code, 0)
        encodes = [call for call in fake.calls if "-y" in call]
        self.assertEqual(len(encodes), 2)
        alpha, plain = encodes
        self.assertIn("hstack", " ".join(alpha))
        self.assertIn("alphaextract", " ".join(alpha))
        self.assertNotIn("-filter_complex", plain)
        for command in encodes:
            self.assertIn("libtheora", command)
            self.assertIn("-an", command)
        self.assertEqual(sum("-xerror" in call for call in fake.calls), 2)
        self.assertEqual((self.cache / f"{ALPHA_KEY}.ogv").read_bytes(), b"OggS encoded")
        self.assertEqual(sorted(path.name for path in self.cache.iterdir()), [f"{ALPHA_KEY}.ogv", f"{RGB_KEY}.ogv"])

    def test_cached_and_shared_entries_are_not_converted_again(self):
        (self.cache / f"{RGB_KEY}.ogv").write_bytes(b"OggS kept")
        fake = FakeTools(self.cache, [("Plain", RGB_KEY, "cached"), ("Tyber_Loop", ALPHA_KEY, "extracted"),
                                      ("Tyber_loop", ALPHA_KEY, "extracted")])
        code, _ = self.main(fake, "Plain", "Tyber_Loop", "Tyber_loop")
        self.assertEqual(code, 0)
        self.assertEqual(sum("-y" in call for call in fake.calls), 1)
        self.assertEqual((self.cache / f"{RGB_KEY}.ogv").read_bytes(), b"OggS kept")

    def test_without_ffmpeg_the_diagnostic_says_how_to_supply_it(self):
        with mock.patch.object(MODULE.shutil, "which", return_value=None), mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("EAWR_FFMPEG", None)
            code, text = self.main(FakeTools(self.cache, []), "Portrait_Loop", ffmpeg=None)
        self.assertEqual(code, 1)
        self.assertIn("EAWR-UI-0706", text)
        self.assertIn("--ffmpeg", text)

    def test_an_ffmpeg_without_libtheora_or_bink_is_refused(self):
        for capabilities, missing in ((" V....D binkvideo  Bink video\n", "libtheora"),
                                      (" V....D libtheora  libtheora Theora\n", "Bink")):
            with self.subTest(missing):
                code, text = self.main(FakeTools(self.cache, [], capabilities=capabilities), "Portrait_Loop")
                self.assertEqual(code, 1)
                self.assertIn("EAWR-UI-0706", text)
                self.assertIn(missing, text)

    def test_a_failed_conversion_keeps_the_cache_and_removes_the_copies(self):
        (self.cache / f"{ALPHA_KEY}.ogv").write_bytes(b"OggS previous")
        fake = FakeTools(self.cache, [("Plain", RGB_KEY, "extracted"), ("Other", "c" * 64 + "-hud-v1-rgb",
                                                                      "extracted")], fail_on=RGB_KEY)
        code, text = self.main(fake, "Plain", "Other")
        self.assertEqual(code, 1)
        self.assertIn("EAWR-UI-0707", text)
        self.assertIn("Invalid data", text)
        self.assertEqual(sorted(path.name for path in self.cache.iterdir()), [f"{ALPHA_KEY}.ogv"])

    def test_output_that_is_not_ogg_never_enters_the_cache(self):
        fake = FakeTools(self.cache, [("Plain", RGB_KEY, "extracted")], encode=b"RIFF")
        code, text = self.main(fake, "Plain")
        self.assertEqual(code, 1)
        self.assertIn("EAWR-UI-0707", text)
        self.assertEqual(list(self.cache.iterdir()), [])

    def test_a_resolution_failure_passes_the_prepare_diagnostic_through(self):
        def fake(args, **kwargs):
            if pathlib.Path(args[0]).name == "prepare":
                return completed(args, returncode=1, stderr='EAWR-UI-0702 [error] data/xml/movies.xml: movie "X" is '
                                                            'not declared\n')
            return completed(args, CAPABLE)
        code, text = self.main(fake, "X")
        self.assertEqual(code, 1)
        self.assertTrue(text.startswith("EAWR-UI-0702"), text)


def capable_ffmpeg():
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        return None
    try:
        return MODULE.find_ffmpeg(ffmpeg)
    except MODULE.ConversionError:
        return None


@unittest.skipUnless(capable_ffmpeg(), "no FFmpeg with the Bink decoder and libtheora on PATH")
class RealFfmpeg(unittest.TestCase):
    def test_the_alpha_packing_doubles_the_width(self):
        ffmpeg = capable_ffmpeg()
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "clip.mkv"
            # A generated 24x16 clip with a real alpha plane (FFV1 keeps it).
            subprocess.run([ffmpeg, "-nostdin", "-v", "error", "-f", "lavfi", "-i",
                            "color=c=red@0.5:s=24x16:r=10:d=0.3,format=yuva420p", "-c:v", "ffv1",
                            str(source)], check=True, timeout=60)
            output = pathlib.Path(directory) / "clip.ogv"
            MODULE.convert(source, output, ffmpeg, True)
            probe = subprocess.run([ffmpeg, "-hide_banner", "-i", str(output)], capture_output=True, text=True)
            self.assertIn("theora", probe.stderr)
            self.assertIn("48x16", probe.stderr)


if __name__ == "__main__":
    unittest.main()
