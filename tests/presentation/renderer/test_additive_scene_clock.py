"""A-03/A-12: the installed satellite ribbon scrolls on the held scene clock."""

import json
import os
import pathlib
import struct
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from space_map_test_support import ROOT, SpaceMapRunner, decode_png, fixture
sys.path.insert(0, str(ROOT))
from tools.megx import read_index


def _chunks(blob):
    offset = 0
    while offset < len(blob):
        kind, size = struct.unpack_from("<II", blob, offset)
        length = size & 0x7fffffff
        yield kind, bool(size & 0x80000000), blob[offset + 8:offset + 8 + length], offset
        offset += 8 + length


def _stock_model(root, logical):
    """Read private installed bytes in manifest/layer order; never commit assets."""
    model = None
    for layer in ("GameData", "corruption"):
        for node in ET.parse(root / layer / "Data/MegaFiles.xml").iter():
            if node.tag.lower() != "file" or not node.text:
                continue
            archive = root / layer / node.text.strip().replace("\\", "/")
            if not archive.is_file():
                continue
            for entry in read_index(archive):
                if entry.name.replace("\\", "/").lower() == logical:
                    with archive.open("rb") as stream:
                        stream.seek(entry.offset)
                        model = stream.read(entry.size)
        loose = root / layer / logical
        if loose.is_file():
            model = loose.read_bytes()
    assert model is not None, logical
    return model


def _additive_only(model):
    """Hide other meshes without changing bones, object indices or ribbon geometry."""
    result = []
    additive_meshes = 0
    for kind, group, body, _ in _chunks(model):
        if kind == 0x400:
            children = list(_chunks(body))
            shaders = [next(_chunks(payload))[2].rstrip(b"\0").decode("ascii")
                       for child, _, payload, _ in children if child == 0x10100]
            if "MeshAdditive.fx" in shaders:
                assert shaders == ["MeshAdditive.fx"], "the ribbon mask must contain only the additive pass"
                additive_meshes += 1
            else:
                body = bytearray(body)
                metadata = next(offset for child, _, _, offset in children if child == 0x402)
                # Mesh metadata: count, min/max bounds, reserved word, hidden word.
                struct.pack_into("<I", body, metadata + 8 + 32, 1)
        result.append(fixture._chunk(kind, bytes(body), group))
    assert additive_meshes == 1, "one authored ribbon mesh must define this mask"
    return b"".join(result)


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST")
                     and os.environ.get("EAWR_EAW_GAME_ROOT"), "requires the installed-game viewer GPU lane")
class AdditiveSceneClock(SpaceMapRunner, unittest.TestCase):
    def test_satellite_ribbon_moves_and_hull_stays_still(self):
        # A synthetic map places the installed SpaceBuildable through normal map
        # population. It carries no other objects or sky models, and no asset copy.
        # Seventy held ticks give 2.1 effect seconds; an exact 2 s at the authored
        # one-wrap/second rate would alias and could look motionless.
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-additive-scene-clock-"))
        root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
        overlay = directory / "map-overlay"
        maps = overlay / "Data/Art/Maps"
        maps.mkdir(parents=True)
        # Like the other loose-only fixtures, declare an absent synthetic archive
        # to establish ordering while every fixture file remains loose.
        (overlay / "Data/MegaFiles.xml").write_text(
            "<Mega_Files><File>EawrClockFixture.meg</File></Mega_Files>", encoding="utf-8")
        header = fixture._mini(0, struct.pack("<I", 0x0201)) + fixture._mini(1, struct.pack("<I", 2))
        environment = fixture._chunk(4, fixture._chunk(6, fixture._mini(0x14, fixture._cstring("CLOCK_TEST"))), True)
        placement = (fixture._mini(0, struct.pack("<I", 1))
                     + fixture._mini(1, struct.pack("<I", fixture.type_crc("Empire_Defense_Satellite_Laser")))
                     + fixture._mini(4, struct.pack("<3f", 0.0, 0.0, 0.0))
                     + fixture._mini(5, struct.pack("<3f", 0.0, 0.0, 0.0)))
        objects = fixture._chunk(1100, fixture._chunk(1113, fixture._chunk(1200, placement), True), True)
        body = fixture._chunk(256, environment, True) + fixture._chunk(258, fixture._chunk(1, objects, True), True)
        logical_map = "data/art/maps/eawr_additive_clock.ted"
        (maps / "eawr_additive_clock.ted").write_bytes(struct.pack("<II", 0, len(header)) + header
                                                    + fixture._chunk(1, body, True))
        options = ("--eawr-populate", "--eawr-mod-root", str(overlay),
                   "--eawr-space-camera", "0,350,450,0,0,0,0,1,0,45,1,1500",
                   "--eawr-map-particle-frames", "1", "--eawr-map-effects", "off",
                   "--eawr-environment", "default", "--eawr-lighting", "sh", "--eawr-shadows", "off",
                   "--eawr-bloom", "off")
        frames = {}
        captures = {}
        for name, offset in (("first", 0), ("later", 70), ("repeat", 0), ("ribbon_first", 0), ("ribbon_later", 70)):
            if name == "ribbon_first":
                models = overlay / "Data/Art/Models"
                models.mkdir()
                (models / "nv_lds.alo").write_bytes(_additive_only(_stock_model(root, "data/art/models/nv_lds.alo")))
            code, report, capture = self._run(root, logical_map, directory,
                                              name, options + ("--eawr-map-idle-offset", str(offset)))
            self.assertEqual(code, 0, report["failure"])
            self.assertEqual(report["populate"]["composed"], 1)
            self.assertEqual(report["space"]["drawn_surfaces"], 0, "the fixture has no sky, planet or nebula")
            self.assertEqual(report["space"]["effect_clock"]["surfaces"], 0)
            self.assertAlmostEqual(report["space"]["effect_clock"]["time_seconds"], offset * 0.03, places=5)
            if name.startswith("ribbon_"):
                self.assertEqual(report["populate"]["surfaces_uploaded"], 1, "only the authored additive mesh is drawn")
            else:
                self.assertGreater(report["populate"]["surfaces_uploaded"], 1, "the hull must also be drawn")
            animation = report["populate"]["unit_animation"]
            self.assertEqual((animation["clock"], animation["idle_offset"]), ("held", offset))
            self.assertEqual(report["populate"]["unit_animation"]["idle_sample_failures"], 0)
            frames[name] = decode_png(capture.read_bytes())
            captures[name] = capture
        self.assertEqual(captures["first"].read_bytes(), captures["repeat"].read_bytes(),
                         "a held offset must repeat exactly")
        width, height, before = frames["first"]
        self.assertEqual((width, height), (1280, 720))
        self.assertEqual(frames["later"][:2], (width, height))
        after = frames["later"][2]

        background = before[0][0]
        def visible(rgb):
            return max(abs(a - b) for a, b in zip(rgb, background)) > 3
        ribbon_frames = (frames["ribbon_first"][2], frames["ribbon_later"][2])
        # The mask comes from the exact authored additive mesh rendered at both
        # clocks, with all other meshes hidden. It assumes nothing about the ribbon's colour.
        ribbon = {(x, y) for y in range(height) for x in range(width)
                  if any(visible(frame[y][x]) for frame in ribbon_frames)}
        self.assertGreater(len(ribbon), 100, "the authored ribbon mesh must be visible")
        mask = {(x + dx, y + dy) for x, y in ribbon for dx in range(-2, 3) for dy in range(-2, 3)}
        changed = {(x, y) for y in range(height) for x in range(width)
                   if max(abs(a - b) for a, b in zip(before[y][x], after[y][x])) > 3}
        hull = {(x, y) for y in range(height) for x in range(width)
                if (x, y) not in mask and (visible(before[y][x]) or visible(after[y][x]))}
        metrics = {"ribbon_pixels": len(ribbon), "changed_ribbon_pixels": len(changed & mask),
                   "changed_hull_pixels": len(changed & hull), "stationary_hull_pixels": len(hull - changed),
                   "effect_seconds": [0.0, 2.1], "offset_ticks": [0, 70]}
        (directory / "clock-metrics.json").write_text(json.dumps(metrics, indent=2), encoding="utf-8")
        self.assertGreater(len(changed & mask), 50, metrics)
        self.assertGreater(len(hull), 500, "the stationary model must also be visible")
        self.assertEqual(len(changed & hull), 0, "the rest of the model must stay still: " + str(metrics))


if __name__ == "__main__":
    unittest.main()
