"""Particle texture origin contracts and a fixed single-particle graphical probe."""

import json
import os
import pathlib
import struct
import subprocess
import tempfile
import unittest

from tests.presentation.renderer.test_effect_mode import ROOT, _chunk, _emitter, _group


LOGICAL_EFFECT = "data/art/models/p_origin_probe.alo"
TEXTURE_NAME = "p_origin_probe.tga"
SIZE = 16


def logical_pixels():
    """Invented asymmetric RGBA image, in top-to-bottom display order."""
    colors = ((250, 24, 16, 255), (20, 242, 28, 255),
              (16, 42, 245, 160), (245, 20, 195, 100))
    return [[colors[2 * (y >= SIZE // 2) + (x >= SIZE // 2)]
             for x in range(SIZE)] for y in range(SIZE)]


def tga(pixels, *, bottom_left=False):
    height, width = len(pixels), len(pixels[0])
    header = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0,
                         width, height, 32, 0x08 if bottom_left else 0x28)
    rows = reversed(pixels) if bottom_left else pixels
    return header + b"".join(bytes((b, g, r, a)) for row in rows
                             for r, g, b, a in row)


def decode_logical_tga(data):
    width, height = struct.unpack_from("<HH", data, 12)
    bottom_left = not bool(data[17] & 0x20)
    channels = [tuple((data[i + 2], data[i + 1], data[i], data[i + 3]))
                for i in range(18, len(data), 4)]
    rows = [channels[y * width:(y + 1) * width] for y in range(height)]
    return list(reversed(rows)) if bottom_left else rows


def effect():
    emitter = _emitter("one-billboard", blend=2, texture=TEXTURE_NAME,
                       position=_group(0), velocity=_group(0), rgb=(1.0, 1.0, 1.0),
                       size=(8.0, 8.0), rate=1, lifetime=5.0)
    return _chunk(0x900, _chunk(0, b"origin-probe\0")
                  + _chunk(1, struct.pack("<I", 1))
                  + _chunk(0x800, emitter, True) + _chunk(2, b"\x01"), True)


def install(root, image):
    data = root / "GameData" / "Data"
    models = data / "Art" / "Models"
    textures = data / "Art" / "Textures"
    models.mkdir(parents=True)
    textures.mkdir(parents=True)
    (models / "P_Origin_Probe.ALO").write_bytes(effect())
    (textures / "P_Origin_Probe.TGA").write_bytes(image)
    (data / "MegaFiles.xml").write_text("<Mega_Files><File>Missing.meg</File></Mega_Files>")


def run_capture(root, output):
    from PIL import Image

    command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--path", str(ROOT / "apps/viewer/project"), "--",
               "--eawr-effect", LOGICAL_EFFECT, "--eawr-game-root", str(root),
               "--eawr-effect-seed", "17", "--eawr-effect-frames", "2", "--eawr-effect-dt", "0.1",
               "--eawr-report", str(output.with_suffix(".json")), "--eawr-capture", str(output)]
    completed = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, check=False)
    report = json.loads(output.with_suffix(".json").read_text(encoding="utf-8"))
    if completed.returncode:
        raise AssertionError(f"{report.get('failure')}\n{completed.stdout}\n{completed.stderr}")
    return report, Image.open(output).convert("RGB")


class TextureOriginGraphical(unittest.TestCase):
    @unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST")
                         and os.environ.get("EAWR_GODOT_EXECUTABLE"),
                         "set graphical viewer and pinned Godot executable")
    def test_equivalent_origins_and_reversed_origin_negative_control(self):
        pixels = logical_pixels()
        top = tga(pixels)
        bottom = tga(pixels, bottom_left=True)
        wrong = bytearray(bottom)
        wrong[17] |= 0x20  # Same reversed rows, falsely declared top-left.
        self.assertEqual(decode_logical_tga(top), decode_logical_tga(bottom))
        self.assertNotEqual(decode_logical_tga(top), decode_logical_tga(wrong))
        captures = []
        with tempfile.TemporaryDirectory(prefix="eawr-particle-origin-") as temporary:
            base = pathlib.Path(temporary)
            for name, encoded in (("top", top), ("bottom", bottom), ("wrong", wrong)):
                root = base / name
                install(root, encoded)
                report, image = run_capture(root, base / f"{name}.png")
                self.assertEqual(report["status"], "effect_render_passed")
                self.assertEqual(report["lifecycle"]["live_rids_after_release"], 0)
                self.assertEqual(report["lifecycle"]["registry_resources_after_release"], 0)
                self.assertEqual(report["frames"][-1]["particles"], 1)
                captures.append((report, image.copy()))
        top_report, top_image = captures[0]
        bottom_report, bottom_image = captures[1]
        wrong_report, wrong_image = captures[2]
        self.assertEqual(top_report["capture_identity"], bottom_report["capture_identity"])
        self.assertEqual(top_report["capture_identity"], wrong_report["capture_identity"])
        self.assertEqual(top_image.tobytes(), bottom_image.tobytes())
        self.assertNotEqual(top_image.tobytes(), wrong_image.tobytes())
        x0, y0, x1, y1 = top_report["evidence"]["projected_bounds_pixels"]
        point = lambda fx, fy: (round(x0 * (1 - fx) + x1 * fx),
                                round(y0 * (1 - fy) + y1 * fy))
        upper_left = top_image.getpixel(point(.25, .25))
        upper_right = top_image.getpixel(point(.75, .25))
        lower_left = top_image.getpixel(point(.25, .75))
        lower_right = top_image.getpixel(point(.75, .75))
        self.assertGreater(upper_left[0], upper_left[2] + 80)
        self.assertGreater(upper_right[1], upper_right[0] + 80)
        self.assertGreater(lower_left[2], lower_left[0] + 60)
        self.assertGreater(lower_right[0], lower_right[1] + 30)
        self.assertGreater(lower_right[2], lower_right[1] + 30)
        self.assertGreater(upper_left[0], lower_right[0] + 40)  # source alpha 255 versus 100
        wrong_upper_left = wrong_image.getpixel(point(.25, .25))
        self.assertLess(wrong_upper_left[0], wrong_upper_left[2])


if __name__ == "__main__":
    unittest.main()
