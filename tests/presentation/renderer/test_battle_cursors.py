"""UI-09 GPU contracts: real battle events resolve install-backed cursor art (CU-01..11).

Run on the GPU lane. EAWR_CURSOR_CAPTURE_DIR retains lit images, reports and logs for eye review.
"""
import os
import pathlib
import re
import shutil
import struct
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
import test_battle_input as battle_input  # noqa: E402
from test_battle_input import CAMERA, CORVETTE, NEBULON, TARTAN, ACCLAMATOR, Y_WING_SQUADRON, TIE_SQUADRON  # noqa: E402


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") == "1", "GPU lane opt-in")
class BattleCursorGraphical(unittest.TestCase):
    _run = battle_input.BattleInputGraphical._run

    def test_missing_art_clears_the_action_cursor_and_overlay(self):
        configured = os.environ.get("EAWR_CURSOR_CAPTURE_DIR")
        with tempfile.TemporaryDirectory(prefix="eawr-cursor-missing-") as temporary:
            directory = pathlib.Path(configured or temporary).resolve() / "missing-art"
            directory.mkdir(parents=True, exist_ok=True)
            camera = directory / "camera.xml"
            camera.write_text(re.sub(r"<initial [^>]*/>",
                '<initial target_x="-4850" target_y="4400" target_height="0" zoom="0.65" yaw_degrees="0"/>',
                CAMERA.read_text(encoding="utf-8")), encoding="utf-8")
            shutil.copy(CAMERA.parent / "space-live-camera-bindings.json", directory)
            mod = directory / "synthetic-mod"
            xml, textures = mod / "Data/XML", mod / "Data/Art/Textures"
            xml.mkdir(parents=True, exist_ok=True)
            textures.mkdir(parents=True, exist_ok=True)
            (mod / "Data/MegaFiles.xml").write_text(
                '<Mega_Files><File>Missing.meg</File></Mega_Files>', encoding="utf-8")
            (xml / "MousePointerFiles.xml").write_text(
                '<MousePointerFiles><File>MousePointers.xml</File></MousePointerFiles>', encoding="utf-8")
            # Entirely synthetic art: the fixture never copies a retail texture.
            (textures / "synthetic_cursor.tga").write_bytes(
                struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, 16, 16, 32, 0x28)
                + bytes((0, 255, 0, 255)) * 256)
            (textures / "normal_cursor.tga").write_bytes(
                struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, 16, 16, 32, 0x28)
                + bytes((255, 0, 0, 255)) * 256)
            (textures / "corrupt_cursor.tga").write_bytes(b"unusable synthetic image")
            common = ["--eawr-mod-root", str(mod), "--eawr-live-ai", "off", "--eawr-live-reveal", "on",
                      "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on",
                      "--eawr-live-step", "2", "--eawr-live-input", f"10:click:unit={CORVETTE}",
                      "--eawr-live-input", "12:hover:screen=800,180"]
            for normal in ("missing_cursor.tga", "corrupt_cursor.tga", "normal_cursor.tga"):
                with self.subTest(normal=normal):
                    (xml / "MousePointers.xml").write_text(
                        '<MousePointers><MousePointer Name="POINTER_MOVE">'
                        '<Base_Texture>synthetic_cursor.tga</Base_Texture></MousePointer>'
                        '<MousePointer Name="POINTER_NORMAL">'
                        f'<Base_Texture>{normal}</Base_Texture></MousePointer></MousePointers>', encoding="utf-8")
                    usable_normal = normal == "normal_cursor.tga"
                    name = normal.removesuffix(".tga")
                    options = [*common, "--eawr-live-input", "60:key:A",
                               "--eawr-live-capture-ticks", "18,68"]
                    if not usable_normal:
                        options += ["--eawr-live-input", "80:hover:hud=pause"]
                    code, result = self._run(directory, name, options, camera=camera, end_tick=100)
                    self.assertEqual(code, 0, result.get("failure"))
                    battle = result["battle_input"]
                    samples = {(sample["id"], sample["hover"]) for sample in battle["cursor_samples"]}
                    self.assertIn(("POINTER_MOVE", "empty"), samples)
                    self.assertIn(("POINTER_ATTACK_ONLY_MODE_NO_UNIT_TARGETED", "empty"), samples)
                    if not usable_normal:
                        self.assertIn(("POINTER_NORMAL", "hud"), samples)
                    cursor = battle["cursor"]
                    self.assertEqual(cursor["hardware"], usable_normal, cursor)
                    self.assertEqual(cursor["overlay_visible"], usable_normal, cursor)
                    self.assertEqual(cursor["system_restores"], 0 if usable_normal else 1, cursor)
                    if usable_normal:
                        self.assertEqual(cursor["art_id"], "POINTER_NORMAL", cursor)
                    else:
                        self.assertNotIn("art_id", cursor)
                        self.assertGreater(cursor["problems"], 0, cursor)
                    self.assertEqual(battle["orders"], 0)
                    self.assertTrue(result["live_session"]["headless_hashes_equal"])
            # Recover custom art after the default arrow was restored.
            (xml / "MousePointers.xml").write_text(
                '<MousePointers><MousePointer Name="POINTER_MOVE">'
                '<Base_Texture>synthetic_cursor.tga</Base_Texture></MousePointer></MousePointers>', encoding="utf-8")
            code, result = self._run(directory, "recover", [*common, "--eawr-live-input", "60:key:A",
                "--eawr-live-input", "80:key:A", "--eawr-live-capture-ticks", "18,68,88"],
                camera=camera, end_tick=100)
            self.assertEqual(code, 0, result.get("failure"))
            self.assertEqual(result["battle_input"]["cursor"]["art_id"], "POINTER_MOVE")
            self.assertTrue(result["battle_input"]["cursor"]["hardware"])
            self.assertEqual(result["battle_input"]["cursor"]["system_restores"], 1)
            self.assertEqual(result["battle_input"]["orders"], 0)
            self.assertTrue(result["live_session"]["headless_hashes_equal"])

    def test_hover_and_targeting_cursors(self):
        configured = os.environ.get("EAWR_CURSOR_CAPTURE_DIR")
        with tempfile.TemporaryDirectory(prefix="eawr-cursor-") as temporary:
            directory = pathlib.Path(configured or temporary).resolve()
            directory.mkdir(parents=True, exist_ok=True)
            shutil.copy(CAMERA.parent / "space-live-camera-bindings.json", directory)
            common = ["--eawr-live-ai", "off", "--eawr-live-reveal", "on",
                      "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on"]

            def camera_file(name, x, y, zoom):
                camera = directory / f"{name}-camera.xml"
                camera.write_text(re.sub(r"<initial [^>]*/>",
                    f'<initial target_x="{x}" target_y="{y}" target_height="0" zoom="{zoom}" yaw_degrees="0"/>',
                    CAMERA.read_text(encoding="utf-8")), encoding="utf-8")
                return camera

            def run(name, gestures, extra, expected, camera, end_tick):
                options = [*common, *extra]
                for gesture in gestures:
                    options += ["--eawr-live-input", gesture]
                code, result = self._run(directory, name, options, camera=camera, end_tick=end_tick)
                self.assertEqual(code, 0, result.get("failure"))
                battle = result["battle_input"]
                self.assertEqual(battle["scripted_fired"], len(gestures), battle["log"])
                samples = {(sample["id"], sample["hover"]) for sample in battle["cursor_samples"]}
                for pair in expected:
                    self.assertIn(pair, samples, battle)
                cursor = battle["cursor"]
                self.assertTrue(cursor["hardware"], cursor)
                self.assertEqual(cursor["definitions"], 53, cursor)
                self.assertEqual(cursor["problems"], 0, cursor)
                self.assertGreater(cursor["frames"], 0, cursor)
                self.assertTrue(result["live_session"]["headless_hashes_equal"])
                self.assertEqual(battle["orders"], 0, battle["log"])
                self.assertEqual(battle["ability_bar"]["targeted"], 0, battle["log"])
                self.assertTrue((directory / f"{name}.png").is_file())
                return battle

            # Select at the friendly spawn, then frame the idle enemy spawn. No combat can
            # kill the selection or target before the ship, reticle and icon hovers.
            run("cursor-world", (f"10:click:unit={CORVETTE}", "12:hover:screen=800,180",
                f"200:hover:unit={ACCLAMATOR}", f"220:hover:reticle={ACCLAMATOR}:first",
                f"420:hover:icon={TIE_SQUADRON}", "440:hover:hud=pause"),
                ["--eawr-live-step", "2", "--eawr-live-follow-group", f"20:{ACCLAMATOR}",
                 "--eawr-live-follow-group", f"240:{TIE_SQUADRON}",
                 "--eawr-live-capture-ticks", "18,208,228,428,448"],
                (("POINTER_MOVE", "empty"), ("POINTER_ATTACK", "unit"),
                 ("POINTER_ATTACK", "hardpoint"), ("POINTER_ATTACK", "squadron_icon"),
                 ("POINTER_NORMAL", "hud")), camera_file("cursor-world", -4850, 4400, 0.65), 460)

            # The existing ion-shot layout, checked while its last craft is still alive.
            # After selecting and arming, frame the target so HUD chrome cannot cover it.
            # Track the moving ship through the capture's render latency.
            battle = run("cursor-ability", (f"2100:click:icon={Y_WING_SQUADRON}", "2110:key:I+shift",
                "2120:hover:screen=800,180",
                *(f"{tick}:hover:unit={TARTAN}" for tick in range(2180, 2240, 10)),
                "2240:hover:screen=800,180"),
                ["--eawr-live-step", "10", "--eawr-live-order", f"1:move:{NEBULON}@-2000,1800,0",
                 "--eawr-live-order", f"1:move:{TARTAN}@-1192,1054,0",
                 "--eawr-live-order", f"1:move:{Y_WING_SQUADRON}@-1900,1750,0",
                 "--eawr-live-follow-group", f"1:{Y_WING_SQUADRON}",
                 "--eawr-live-follow-group", f"2130:{TARTAN}",
                 "--eawr-live-capture-ticks", "2170,2220,2270"],
                (("POINTER_TARGET_SPECIAL_ABILITY_TO_ENEMY_OBJECT", "unit"),
                 ("POINTER_TARGET_SPECIAL_ABILITY_TO_ENEMY_OBJECT_INVALID", "empty")),
                camera_file("cursor-ability", -1900, 1750, 0.85), 2270)
            self.assertTrue(battle["ability_bar"]["targeting"], battle["ability_bar"])
            self.assertEqual(battle["cursor"]["id"], "POINTER_TARGET_SPECIAL_ABILITY_TO_ENEMY_OBJECT_INVALID")


if __name__ == "__main__":
    unittest.main()
