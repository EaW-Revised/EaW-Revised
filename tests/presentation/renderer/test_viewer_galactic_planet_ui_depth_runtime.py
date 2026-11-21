"""#7809: the galaxy map stacks each planet's controls by camera depth and hides them behind nearer planet bodies."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

from tests.presentation.renderer import test_viewer_galactic_command_bar_runtime as support


# Orbit the map (middle button + ctrl rotates) and pan it with the arrow keys; at every stop, write the picture and
# the structure of the planet control groups: their stacking order, which of them carry an occluder material, and
# the visible controls that still sit outside every group.
DRIVER = '''extends SceneTree
var frame = 0
var output = %s
var viewport_size = Vector2i(%d, %d)
var facts = {}
# Each step: label, kind, a, b. "pan": hold key a for b frames. "drag": middle-button drag with Ctrl from a press
# point in empty space, spread over frames: a is the (dx, dy) of the whole drag, b its length in frames.
var plan = [["pan_right", "pan", KEY_RIGHT, 50], ["pan_up", "pan", KEY_UP, 60], ["pan_left", "pan", KEY_LEFT, 120],
    ["pan_down", "pan", KEY_DOWN, 120], ["pan_up_2", "pan", KEY_UP, 160], ["drag_tilt", "drag", Vector2(0, 200), 8],
    ["drag_turn_1", "drag", Vector2(240, 0), 8], ["drag_turn_2", "drag", Vector2(240, 0), 8],
    ["pan_right_2", "pan", KEY_RIGHT, 100]]
var slot = 0
var slot_start = 0
var held_key = 0
var probes = []
var probe_start = 0
var pending = ""
var drag_from = Vector2(1150, 330)
func _initialize(): call_deferred("start")
func start():
    root.content_scale_factor = 1.0
    root.content_scale_aspect = Window.CONTENT_SCALE_ASPECT_KEEP
    root.content_scale_size = viewport_size
    root.content_scale_mode = Window.CONTENT_SCALE_MODE_VIEWPORT
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func loose(node, rows, in_group):
    for child in node.get_children():
        var inside = in_group or str(child.name).begins_with("planet_ui_")
        if child is Control and child.is_visible_in_tree() and not inside:
            var token = str(child.name)
            if token.begins_with("planet_") or token.begins_with("fleet_presence_") or token.begins_with("ground_presence_"):
                rows.append(token)
        loose(child, rows, inside)
# Visible controls of a group that a nearer body covers keep a meta entry with their old mouse filter and must ignore the mouse.
func covered_controls(node, rows):
    for child in node.get_children():
        if child is Control and child.has_meta("planet_ui_filter"):
            rows.append(child)
        covered_controls(child, rows)
func plan_probes(label, layer):
    probes = []
    for group in layer.get_children():
        if group.material == null: continue
        var discs = group.material.get_shader_parameter("discs")
        var count = int(group.material.get_shader_parameter("disc_count"))
        var rows = []
        covered_controls(group, rows)
        for control in rows:
            if not control.is_visible_in_tree(): continue
            var rect = control.get_global_rect()
            for slot_index in range(count):
                var centre = Vector2(discs[slot_index].x, discs[slot_index].y)
                var point = Vector2(clamp(centre.x, rect.position.x, rect.end.x), clamp(centre.y, rect.position.y, rect.end.y))
                if point.distance_to(centre) < discs[slot_index].z - 1.0 and Rect2(Vector2.ZERO, Vector2(viewport_size)).has_point(point):
                    probes.append({"group": str(group.name), "control": str(control.name), "filter": control.mouse_filter,
                        "point": point, "path": control.get_path()})
                    break
func capture(label):
    var layer = root.find_child("GalacticPlanetUi", true, false)
    var order = []
    var shown = []
    var occluded = []
    var visible_controls = 0
    if layer != null:
        for group in layer.get_children():
            order.append(str(group.name))
            if group.material != null: occluded.append(str(group.name))
            var any = false
            for control in group.get_children():
                if control is Control and control.is_visible_in_tree():
                    visible_controls += 1
                    # Only a control on the screen is stacked for; one far off it may keep an old place.
                    if control.get_global_rect().intersects(Rect2(Vector2.ZERO, Vector2(viewport_size))): any = true
            if any: shown.append(str(group.name))
    var pose = null
    if layer != null and layer.has_meta("camera_eye"):
        var eye = layer.get_meta("camera_eye")
        var forward = layer.get_meta("camera_forward")
        pose = {"eye": [eye.x, eye.y, eye.z], "forward": [forward.x, forward.y, forward.z]}
    var rows = []
    loose(root, rows, false)
    if layer != null: plan_probes(label, layer)
    facts[label] = {"probes": [], "layer": layer != null, "order": order, "shown": shown, "occluded": occluded, "camera": pose, "visible": visible_controls, "loose": rows}
    root.get_texture().get_image().save_png(output + "/" + label + ".png")
    pending = label
    probe_start = frame
func mouse(down, point, button, ctrl):
    var event = InputEventMouseButton.new()
    event.button_index = button
    event.position = point
    event.global_position = point
    event.pressed = down
    event.ctrl_pressed = ctrl
    Input.parse_input_event(event)
func key(code, down):
    var event = InputEventKey.new()
    event.keycode = code
    event.physical_keycode = code
    event.pressed = down
    Input.parse_input_event(event)
func _process(_delta):
    frame += 1
    Input.use_accumulated_input = false
    if frame == 600: capture("start")
    if frame < 620 or slot >= plan.size():
        if slot >= plan.size() and frame > slot_start + 30:
            var file = FileAccess.open(output + "/depth.json", FileAccess.WRITE)
            file.store_string(JSON.stringify(facts))
            file.close()
            current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
            quit()
        return false
    if pending != "":
        # Click check: with the pointer over a covered control inside a nearer planet's disc, no control of the
        # rear planet's group may be under it (the click reaches the nearer planet instead).
        var probe_age = frame - probe_start
        var index = int(probe_age / 4)
        if index >= probes.size():
            pending = ""
            slot += 1
            slot_start = frame + 1
            return false
        if probe_age %% 4 == 0:
            var motion = InputEventMouseMotion.new()
            motion.position = probes[index]["point"]
            motion.global_position = probes[index]["point"]
            Input.parse_input_event(motion)
        elif probe_age %% 4 == 3:
            var hovered = root.gui_get_hovered_control()
            var row = probes[index]
            var inside = hovered != null and str(hovered.get_path()).begins_with("/root") and ("/" + str(row["group"]) + "/") in str(hovered.get_path())
            facts[pending]["probes"].append({"group": row["group"], "control": row["control"], "filter": row["filter"],
                "hovered": str(hovered.get_path()) if hovered != null else "", "reached_rear": inside})
        return false
    var step = plan[slot]
    if slot_start == 0: slot_start = frame
    var age = frame - slot_start
    if step[1] == "pan":
        if age == 0: key(step[2], true)
        if age == step[3]: key(step[2], false)
    else:
        var at = drag_from + step[2] * min(age, step[3]) / float(step[3])
        if age == 0:
            mouse(true, drag_from, MOUSE_BUTTON_MIDDLE, true)
        elif age <= step[3]:
            var motion = InputEventMouseMotion.new()
            motion.position = at
            motion.global_position = at
            motion.relative = step[2] / float(step[3])
            motion.button_mask = MOUSE_BUTTON_MASK_MIDDLE
            motion.ctrl_pressed = true
            Input.parse_input_event(motion)
        elif age == step[3] + 1:
            mouse(false, at, MOUSE_BUTTON_MIDDLE, true)
    var length = step[3] + 14
    if age == length:
        capture(step[0])
    return false
'''


def render_point(position):
    return [position[0], position[2], -position[1]]


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticPlanetUiDepthRuntime(unittest.TestCase):
    def campaign(self, mod_root):
        from tools.inventory.corpus import Corpus

        roots = [str(mod_root)] if mod_root else None
        corpus = Corpus(support.GAME_ROOT, mod_root=roots)
        profile = "remake" if mod_root else "foc"
        registry = corpus.read_effective(profile, "data/xml/campaignfiles.xml")
        self.assertIsNotNone(registry)
        choices = []
        for entry in ET.fromstring(registry.data):
            name = (entry.text or "").strip().replace("\\", "/")
            if not name:
                continue
            if not name.lower().startswith("data/"):
                name = "data/xml/" + name
            source = corpus.read_effective(profile, name)
            if source is None:
                continue
            for campaign in ET.fromstring(source.data):
                count = sum(len((node.text or "").replace(",", " ").split()) for node in campaign.findall("Locations"))
                if count and campaign.findtext("Starting_Active_Player"):
                    choices.append((count, campaign.attrib["Name"]))
        self.assertTrue(choices, "installed profile has no playable registered campaign")
        return max(choices)[1]

    def run_stage(self, mod_root, campaign, width, height):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-planet-ui-depth-"))
        script, report = directory / "depth.gd", directory / "report.json"
        script.write_text(DRIVER % (json.dumps(directory.as_posix()), width, height), encoding="utf-8")
        args = [os.environ["EAWR_GODOT_EXECUTABLE"], "--render-thread", "separate",
                "--resolution", f"{width}x{height}", "--max-fps", "60", "--fixed-fps", "60",
                "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(script), "--",
                "--eawr-galactic", campaign, "--eawr-game-root", support.GAME_ROOT,
                "--eawr-galactic-paused", "yes", "--eawr-galactic-dismiss-story", "yes",
                "--eawr-galactic-filters", "7", "--eawr-audio", "off",
                "--eawr-diagnostic-max-fps", "60", "--eawr-lighting", "sh",
                "--eawr-environment", "map", "--eawr-shadows", "on", "--eawr-report", str(report)]
        if mod_root:
            args += ["--eawr-mod-root", str(mod_root)]
        result = subprocess.run(args, cwd=support.ROOT, capture_output=True, text=True, timeout=600, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, f"{directory}\n{result.stdout}\n{result.stderr}")
        stages = json.loads((directory / "depth.json").read_text(encoding="utf-8"))
        for label, stage in stages.items():
            where = f"{directory}/{label}"
            self.assertTrue(stage["layer"], f"no GalacticPlanetUi layer: {where}")
            self.assertGreater(stage["visible"], 0, f"no visible planet controls: {where}")
            # Every visible planet control sits in its planet's group, so the stacking reaches all of them.
            self.assertEqual(stage["loose"], [], f"planet controls outside their group: {where}")
            self.assertEqual(len(stage["order"]), len(set(stage["order"])), f"duplicate planet group: {where}")
            # A click inside a nearer planet's disc never reaches a rear planet's control.
            for probe in stage["probes"]:
                self.assertEqual(probe["filter"], 2, f"covered control kept a mouse filter: {probe} {where}")
                self.assertFalse(probe["reached_rear"], f"a click inside the nearer planet reached the rear control: {probe} {where}")
        self.assertTrue(report.is_file(), f"no viewer report: {directory}")
        planets = {f"planet_ui_{row['id']}": row for row in json.loads(report.read_text(encoding="utf-8"))["planets"]}
        start = stages["start"]
        self.assertIsNotNone(start["camera"], f"no 3D camera: {directory}")
        start_png = (directory / "start.png").read_bytes()
        overlaps = []
        previous = start
        for label, stage in stages.items():
            where = f"{directory}/{label}"
            pose = stage["camera"]
            self.assertIsNotNone(pose, f"no 3D camera: {where}")
            if label != "start":
                # The drag has to move the camera and the picture; a stop equal to the start proves nothing.
                moved = any(abs(a - b) > 1e-4 for key in ("eye", "forward") for a, b in zip(pose[key], previous["camera"][key]))
                self.assertTrue(moved, f"the camera did not move since the previous stop: {where}")
                self.assertNotEqual((directory / f"{label}.png").read_bytes(), start_png, f"picture equals start: {where}")

            previous = stage

            def depth(name):
                return sum((c - e) * f for c, e, f in zip(render_point(planets[name]["position"]), pose["eye"], pose["forward"]))
            # Planets with visible controls are stacked farthest first, so a nearer planet's group is on top.
            shown = [name for name in stage["order"] if name in stage["shown"]]
            depths = [depth(name) for name in shown]
            for far, near in zip(depths, depths[1:]):
                self.assertGreaterEqual(far + 1e-3, near, f"group order does not follow depth {shown} {depths}: {where}")
            # A group that clears its picture is behind a nearer planet body (its controls may be off the screen).
            for name in stage["occluded"]:
                if name in stage["shown"]:
                    self.assertTrue(any(depth(other) < depth(name) - 1e-3 for other in planets if other != name),
                                    f"{name} is occluded but no nearer planet exists: {where}")
                    overlaps.append(label)
        # The #7809 case: somewhere in the turn a nearer planet covers a farther planet's controls.
        self.assertTrue(overlaps, f"no stop shows a nearer planet over a farther planet's controls: {directory}")
        # Turning the map changes which planet is in front: the stack follows the camera.
        self.assertGreater(len({tuple(stage["order"]) for stage in stages.values()}), 1,
                           f"the group order never changed while the camera moved: {directory}")
        self.assertTrue(any(stage["probes"] for stage in stages.values()),
                        f"no stop had a covered control under a nearer planet's disc to click: {directory}")
        (directory / "overlap_stops.txt").write_text(" ".join(sorted(set(overlaps))), encoding="utf-8")
        return directory

    def test_stock_groups_follow_camera_depth(self):
        campaign = self.campaign(None)
        self.run_stage(None, campaign, 1280, 720)


if __name__ == "__main__":
    unittest.main()
