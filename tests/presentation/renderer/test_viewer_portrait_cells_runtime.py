"""Compare shared GC portrait cells with native texture children in a paused scene."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_planet_hud_runtime as support


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class PortraitCellsRuntime(unittest.TestCase):
    def compare(self, fixture, names, size):
        from PIL import Image
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-portrait-cells-"))
        driver = directory / "compare.gd"
        driver.write_text('''extends SceneTree
var elapsed = 0.0
var frame = 0
var targets = []
var names = ''' + json.dumps(names) + '''
var output = ''' + json.dumps(directory.as_posix()) + '''
var requested_size = Vector2i''' + str(size) + '''
func _initialize():
    # Pin the render viewport even when Windows clamps the physical window.
    root.borderless = true
    root.position = Vector2i.ZERO
    root.size = requested_size
    root.content_scale_factor = 1.0
    root.content_scale_aspect = Window.CONTENT_SCALE_ASPECT_KEEP
    root.content_scale_size = requested_size
    root.content_scale_mode = Window.CONTENT_SCALE_MODE_VIEWPORT
    var instance = load("res://main.tscn").instantiate()
    root.add_child(instance)
    current_scene = instance
func _process(delta):
    elapsed += delta
    if elapsed < 2.5: return false
    frame += 1
    if frame == 1:
        for name in names:
            var button = root.find_child(name, true, false)
            assert(button != null and button.is_visible_in_tree(), "missing portrait consumer " + name)
            assert(button.has_method("set_portrait_cell_enabled"))
            targets.append(button)
    if frame in [20, 40, 60]:
        root.get_texture().get_image().save_png(output + "/" + str(frame) + ".png")
    if frame in [21, 41]:
        for button in targets: button.set_portrait_cell_enabled(frame == 41)
    if frame == 60:
        var controls = []
        for button in targets:
            var rect = button.get_global_rect()
            var art = false
            for child in button.get_children():
                if child is TextureRect and child.texture != null: art = true
            controls.append({"name": button.name, "rect": [rect.position.x, rect.position.y, rect.size.x, rect.size.y], "art": art})
        var file = FileAccess.open(output + "/controls.json", FileAccess.WRITE)
        file.store_string(JSON.stringify(controls))
        file.close()
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''', encoding="utf-8")
        args = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", f"{size[0]}x{size[1]}",
                "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
                "--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", fixture,
                "--eawr-galactic-paused", "yes", "--eawr-game-root", support.GAME_ROOT,
                "--eawr-report", str(directory / "report.json")]
        if fixture == "ground-contents":
            args += ["--eawr-galactic-focus", "101", "--eawr-galactic-flight-ms", "750"]
        result = subprocess.run(args, cwd=support.ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        controls = json.loads((directory / "controls.json").read_text(encoding="utf-8"))
        self.assertTrue(any(row["art"] for row in controls), "comparison never drew a portrait")
        images = []
        for frame in (20, 40, 60):
            with Image.open(directory / f"{frame}.png") as image:
                self.assertEqual(image.size, size)
                images.append(image.convert("RGB"))
        for row in controls:
            x, y, width, height = row["rect"]
            box = (round(x), round(y), round(x + width), round(y + height))
            self.assertGreater(box[2] - box[0], 0, row)
            self.assertGreater(box[3] - box[1], 0, row)
            self.assertGreaterEqual(box[0], 0, row)
            self.assertGreaterEqual(box[1], 0, row)
            self.assertLessEqual(box[2], size[0], row)
            self.assertLessEqual(box[3], size[1], row)
            pixels = [image.crop(box).tobytes() for image in images]
            self.assertEqual(pixels[0], pixels[1], f"shared portrait differs from native: {row}")
            self.assertEqual(pixels[1], pixels[2], f"restoring shared portrait changes pixels: {row}")

    def test_ground_portraits_match_native_at_both_viewports(self):
        for size in ((1280, 720), (1920, 1080)):
            with self.subTest(size=size):
                self.compare("ground-contents", ["Pad_00", "Pad_01", "Pad_09", "Slot_00"], size)

    def test_hero_portraits_match_native_enabled_and_disabled(self):
        for fixture, names in (("theft", ["bm_0000", "bm_0001"]),
                               ("neutralization", ["h_0000", "h_0001", "h_0100"])):
            with self.subTest(fixture=fixture):
                self.compare(fixture, names, (1280, 720))
