"""Stock solo campaigns: the leaders the story plot places at the start and their price discount in the build menu."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_galactic_command_bar_runtime as support
ROOT, RUNTIME, GAME_ROOT = support.ROOT, support.RUNTIME, support.GAME_ROOT

DRIVER = '''extends SceneTree
var frame = 0
var elapsed = 0.0
var hit_points = %(hit_points)s
var directory = %(directory)s
var run_seconds = %(run_seconds)s
var step = 0
var step_time = 0.0
var notes = []
func _initialize(): call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func shot(label):
    root.get_texture().get_image().save_png(directory + "/" + label + ".png")
    notes.append({"shot": label, "elapsed": elapsed})
func click(name):
    var target = root.find_child(name, true, false)
    notes.append({"click": name, "found": target != null, "visible": target != null and target.is_visible_in_tree(),
        "disabled": target != null and target.disabled, "elapsed": elapsed})
    assert(target != null and target.is_visible_in_tree() and not target.disabled)
    var point = Vector2(hit_points[name][0], hit_points[name][1])
    var motion = InputEventMouseMotion.new()
    motion.position = point
    motion.global_position = point
    Input.parse_input_event(motion)
    for down in [true, false]:
        var event = InputEventMouseButton.new()
        event.button_index = MOUSE_BUTTON_LEFT
        event.pressed = down
        event.position = point
        event.global_position = point
        Input.parse_input_event(event)
func double_click_centre():
    var point = root.get_visible_rect().size / 2.0
    notes.append({"double_click": [point.x, point.y], "elapsed": elapsed})
    var motion = InputEventMouseMotion.new()
    motion.position = point
    motion.global_position = point
    Input.parse_input_event(motion)
    for count in [1, 2]:
        for down in [true, false]:
            var event = InputEventMouseButton.new()
            event.button_index = MOUSE_BUTTON_LEFT
            event.pressed = down
            event.double_click = down and count == 2
            event.position = point
            event.global_position = point
            Input.parse_input_event(event)
func finish():
    var file = FileAccess.open(directory + "/driver.json", FileAccess.WRITE)
    file.store_string(JSON.stringify(notes))
    file.close()
    current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
    quit()
func _process(delta):
    frame += 1
    elapsed += delta
    if frame == 40 and step == 0:
        shot("%(label)s-00-start"); click("b_space_tab"); step = 1; step_time = elapsed
    elif step == 1 and elapsed - step_time > 1.0:
        shot("%(label)s-01-space-tab")
        if run_seconds > 0.0:
            click("b_play_pause"); step = 2; step_time = elapsed
        else:
            step = 4; step_time = elapsed
    elif step == 2 and elapsed - step_time > run_seconds:
        shot("%(label)s-02-clock-running"); double_click_centre(); step = 3; step_time = elapsed
    elif step == 3 and elapsed - step_time > 4.0:
        shot("%(label)s-03-planet-focused"); step = 4; step_time = elapsed
    elif step == 4 and elapsed - step_time > 1.0:
        finish()
    return false
'''


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticLeadersStartRuntime(unittest.TestCase):
    def launch(self, directory, campaign, name, extra, script=None):
        report = directory / (name + ".json")
        command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                   "--path", str(ROOT / "apps/viewer/project")]
        if script:
            command += ["--script", str(script)]
        command += ["--", "--eawr-galactic", campaign, "--eawr-game-root", GAME_ROOT,
                    "--eawr-report", str(report), "--eawr-environment", "map", "--eawr-lighting", "sh",
                    "--eawr-shadows", "on", *extra]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=600, check=False)
        (directory / (name + ".log")).write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
        return data

    def look(self, campaign, label, run_seconds):
        """Open the stock campaign, click the space tab with real input, optionally run the clock, report the screen."""
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-leaders-%s-" % label))
        initial = self.launch(directory, campaign, "initial", ["--eawr-galactic-paused", "yes",
                                                      "--eawr-capture", str(directory / "initial.png")])
        points = {control["name"]: [control["hit_rect"][0] + control["hit_rect"][2] / 2,
                                   control["hit_rect"][1] + control["hit_rect"][3] / 2]
                  for control in initial["command_bar"]["controls"]}
        for run, seconds in (("before", 0.0), ("after", run_seconds)):
            driver = directory / (run + ".gd")
            driver.write_text(DRIVER % {"hit_points": json.dumps(points), "directory": json.dumps(directory.as_posix()),
                                        "run_seconds": repr(float(seconds)), "label": "%s-%s" % (label, run)},
                              encoding="utf-8")
            data = self.launch(directory, campaign, run, ["--eawr-galactic-paused", "yes"], script=driver)
            self.assertEqual(data["status"], "closed")
            self.assertTrue(data["command_bar"]["space_tab"])
            yield run, data
        print("LEADERS-DIRECTORY", label, directory)

    def survey(self, campaign, label, run_seconds=8.0):
        seen = dict(self.look(campaign, label, run_seconds))
        rows = {}
        for run, data in seen.items():
            groups = [{"slot": card["slot"], "planet": card["planet"], "type": group["type"], "count": group["count"]}
                      for card in data["fleet_hud"]["cards"] if card["local"] for group in card["groups"]]
            planet = data["map_input"]["selected"]
            rows[run] = {"selected": planet, "frame": data["live_session"].get("galactic_frame"),
                         "planet": next(row["name"] for row in data["planets"] if row["id"] == planet),
                         "account": data["command_bar"]["account"], "ground": int(data["ground_hud"]["count"]),
                         "prices": {c["type"]: c["price"] for c in data["command_bar"]["production_choices"]},
                         "orbit_groups": [g for g in groups if g["planet"] == planet],
                         "faction": data["local"]["faction"]}
        print("SURVEY", campaign, json.dumps(rows))
        return rows

    def test_empire_palpatine_discounts_the_coruscant_cards(self):
        rows = self.survey("Sandbox_Equal_Footing_Empire", "empire")
        after = rows["after"]
        self.assertEqual(after["planet"], "Coruscant")
        self.assertGreater(after["ground"], 2, "the plot's leaders (Tarkin, Palpatine) stand on the capital beside the two stock squads")
        # Tartan_Patrol_Cruiser: Build_Cost_Credits 1250, Sullust's Production Boost 20% plus the Emperor's authored
        # Emperor_Reduce_Production_Price_Ability 25% (all categories, his planet): 1250 * (1 - 0.20 - 0.25) = 687.5.
        self.assertEqual(after["prices"]["Tartan_Patrol_Cruiser"], 688)
        self.assertLessEqual(after["prices"]["Tartan_Patrol_Cruiser"], rows["before"]["prices"]["Tartan_Patrol_Cruiser"])

    def test_rebel_mothma_discounts_the_hoth_cards(self):
        rows = self.survey("Sandbox_Equal_Footing_Rebel", "rebel")
        after = rows["after"]
        self.assertEqual(after["planet"], "Hoth")
        # Corellian_Corvette: Build_Cost_Credits 1250; Mothma_Reduce_Production_Price_Ability 25% (all categories, her planet).
        self.assertEqual(after["prices"]["Corellian_Corvette"], 938)

    def test_underworld_has_no_leader_discount(self):
        rows = self.survey("Sandbox_Equal_Footing_Underworld", "underworld")
        self.assertEqual(rows["after"]["planet"], "Hypori")
        self.assertGreater(rows["after"]["ground"], rows["before"]["ground"], "the plot's leaders arrive after one second")
        self.assertEqual(rows["after"]["prices"], rows["before"]["prices"], "no Underworld leader authors a price reduction")

if __name__ == "__main__":
    unittest.main()
