"""Render both minimap filter buttons against independently extracted XML/atlas layers."""
import io
import json
import os
import pathlib
import re
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

from tests.presentation.renderer import test_viewer_galactic_command_bar_runtime as support


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticFilterStatesRuntime(unittest.TestCase):
    def test_both_filters_render_authored_normal_hover_pressed_disabled(self):
        from PIL import Image, ImageChops, ImageStat
        from tools.inventory.corpus import Corpus
        from tools.inventory.mtd_inventory import parse_mtd

        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-filter-states-"))
        corpus = Corpus(support.GAME_ROOT)
        xml = corpus.read_effective("foc", "Data/XML/CommandBarComponents.xml")
        components = {node.get("Name"): node for node in ET.fromstring(xml.data)}
        entries = {entry.name.lower(): entry for entry in parse_mtd(corpus.read_effective(
            "foc", "Data/Art/Textures/mt_commandbar.mtd").data)}
        page = Image.open(io.BytesIO(corpus.read_effective(
            "foc", "Data/Art/Textures/mt_commandbar.tga").data)).convert("RGBA")
        fields = {"blank": "Blank_Texture_Name", "icon": "Icon_Texture_Name",
                  "hover": "Mouse_Over_Texture_Name", "pressed": "Selected_Texture_Name",
                  "disabled": "Disabled_Texture_Name"}
        authored = []
        for name in ("b_filters0", "b_filters1"):
            component = components[name]
            self.assertEqual(component.findtext("Swap_Texture").strip().lower(), "false")
            row = {"name": name, "shift": component.findtext("Click_Shift").strip().lower() == "true",
                   "selected_alpha": component.findtext("Selected_Alpha").strip().lower() == "true"}
            for state, field in fields.items():
                token = re.split(r"[\s,]+", component.findtext(field).strip())[0]
                entry = entries[token.lower()]
                image = page.crop((entry.x, entry.y, entry.x + entry.width, entry.y + entry.height))
                if not entry.has_alpha:
                    image.putalpha(255)
                path = directory / (name + "-" + state + ".png")
                image.save(path)
                row[state] = path.as_posix()
                row[state + "_texture"] = token
            authored.append(row)
        (directory / "authored.json").write_text(json.dumps(authored, indent=2), encoding="utf-8")
        driver = directory / "states.gd"
        driver.write_text('''extends SceneTree
var frame = 0
var output = ''' + json.dumps(directory.as_posix()) + '''
var authored = JSON.parse_string(''' + repr(json.dumps(authored)) + ''')
var buttons = []
var reference
var states = ["normal", "hover", "pressed", "disabled"]
func _initialize():
    call_deferred("start")
func start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func image_texture(path, opaque):
    var image = Image.load_from_file(path)
    if opaque: image.convert(Image.FORMAT_RGB8)
    return ImageTexture.create_from_image(image)
func layer(path, offset = Vector2.ZERO, additive = false, opaque = false):
    var rect = TextureRect.new()
    rect.texture = image_texture(path, opaque)
    rect.position = offset
    rect.mouse_filter = Control.MOUSE_FILTER_IGNORE
    if additive:
        var material = CanvasItemMaterial.new()
        material.blend_mode = CanvasItemMaterial.BLEND_MODE_ADD
        rect.material = material
    reference.add_child(rect)
func motion(point):
    var event = InputEventMouseMotion.new()
    event.position = point
    event.global_position = point
    Input.parse_input_event(event)
func _process(_delta):
    frame += 1
    if frame == 60:
        var canvas = CanvasLayer.new()
        canvas.layer = 100
        root.add_child(canvas)
        var back = ColorRect.new()
        back.color = Color.BLACK
        back.size = Vector2(1280, 720)
        back.mouse_filter = Control.MOUSE_FILTER_IGNORE
        canvas.add_child(back)
        for row in authored:
            var button = root.find_child(row.name, true, false)
            assert(button != null, "filter absent: " + row.name)
            print("FILTER initial ", row.name, " pressed=", button.button_pressed)
            assert(not button.button_pressed, "renderer fixture must start both filters unselected")
            button.pressed.connect(func(): print("FILTER action ", button.name, " frame=", frame))
            button.reparent(canvas)
            button.hide()
            button.position = Vector2(100, 100)
            button.size = Vector2(36, 35)
            button.scale = Vector2(3, 3)
            buttons.append(button)
        reference = Control.new()
        reference.position = Vector2(300, 100)
        reference.scale = Vector2(3, 3)
        canvas.add_child(reference)
        current_scene.process_mode = Node.PROCESS_MODE_DISABLED
    if frame >= 70 and frame < 230:
        var index = int((frame - 70) / 20)
        var phase = (frame - 70) % 20
        var button = buttons[int(index / 4)]
        var row = authored[int(index / 4)]
        var state = states[index % 4]
        if phase == 0:
            for target in buttons: target.hide()
            for child in reference.get_children():
                reference.remove_child(child)
                child.queue_free()
            button.show()
            button.disabled = false
            button.set_pressed_no_signal(false)
            button.emit_signal("button_up")
            motion(Vector2(1100, 100))
        if phase == 3:
            if state == "hover": motion(button.get_global_rect().get_center())
            if state == "pressed":
                button.set_pressed_no_signal(true)
                button.emit_signal("button_down")
            if state == "disabled":
                button.disabled = true
                button.emit_signal("mouse_exited")
            layer(row.blank)
            layer(row.icon, Vector2(0, 1) if state == "pressed" and row.shift else Vector2.ZERO)
            if state != "normal": layer(row[state], Vector2.ZERO, state == "hover" or (state == "pressed" and not row.selected_alpha), state == "hover")
        if phase == 10:
            print("FILTER state ", row.name, " ", state, " pressed=", button.button_pressed,
                " disabled=", button.disabled, " hover=", button.is_hovered(), " rect=", button.get_global_rect())
            if state == "hover": assert(button.is_hovered(), "filter did not receive hover")
            root.get_texture().get_image().save_png(output + "/" + row.name + "-" + state + "-render.png")
    if frame == 230:
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''', encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-seat", "3",
            "--eawr-galactic-hud-fixture", "accessible", "--eawr-game-root", support.GAME_ROOT,
            "--eawr-galactic-paused", "yes", "--eawr-report", str(directory / "report.json")],
            cwd=support.ROOT, capture_output=True, text=True, timeout=180)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("Assertion failed", result.stdout + result.stderr)
        for row in authored:
            for state in ("normal", "hover", "pressed", "disabled"):
                with self.subTest(button=row["name"], state=state):
                    capture = Image.open(directory / (row["name"] + "-" + state + "-render.png")).convert("RGB")
                    actual = capture.crop((100, 100, 208, 205))
                    expected = capture.crop((300, 100, 408, 205))
                    error = max(ImageStat.Stat(ImageChops.difference(actual, expected)).mean)
                    self.assertLess(error, 1.0, "render differs from authored layers")
                    if state == "hover":
                        centre = actual.crop((24, 24, 84, 81))
                        self.assertGreater(max(ImageStat.Stat(centre).mean), 35, "hover is near-black")


if __name__ == "__main__":
    unittest.main()
