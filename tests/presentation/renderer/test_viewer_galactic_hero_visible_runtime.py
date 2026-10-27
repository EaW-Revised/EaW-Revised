"""A player of a stock solo campaign can find, buy, wait for and see a generic hero, with real input only."""
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
var card = %(card)s
var tab = %(tab)s
var hero_icon = %(hero_icon)s
var hit_points = %(hit_points)s
var directory = %(directory)s
var step = 0
var step_time = 0.0
var queued_seen = false
var done_time = -1.0
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
func click_node(name):
    var target = root.find_child(name, true, false)
    notes.append({"click_node": name, "found": target != null, "visible": target != null and target.is_visible_in_tree(),
        "elapsed": elapsed})
    assert(target != null and target.is_visible_in_tree())
    var point = target.get_global_rect().get_center()
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
func queue_visible():
    var head = root.find_child("queue00", true, false)
    return head != null and head.is_visible_in_tree()
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
        shot("00-start"); click(tab); step = 5; step_time = elapsed
    elif step == 5 and elapsed - step_time > 1.0:
        shot("01-build-menu"); step = 1; step_time = elapsed
    elif step == 1 and elapsed - step_time > 0.5:
        if card == "":
            finish()
        else:
            click(card); step = 2; step_time = elapsed
    elif step == 2 and elapsed - step_time > 1.0:
        shot("02-queued"); queued_seen = queue_visible(); notes.append({"queued_seen": queued_seen})
        click("b_play_pause"); step = 3; step_time = elapsed
    elif step == 3:
        if queue_visible(): queued_seen = true
        if elapsed - step_time > %(limit)s or (queued_seen and not queue_visible()):
            if done_time < 0.0: done_time = elapsed
            if elapsed - done_time > 2.0:
                shot("03-produced"); step = 4; step_time = elapsed
                click_node(hero_icon); step = 6
        elif int((elapsed - step_time) * 2) != int((elapsed - step_time - delta) * 2) and int(elapsed - step_time) %% 5 == 0:
            shot("progress-" + str(int(elapsed - step_time)))
    elif step == 6 and elapsed - step_time > 1.5:
        shot("04-hero-fleet-popup"); double_click_centre(); step = 7; step_time = elapsed
    elif step == 7 and elapsed - step_time > 4.0:
        shot("05-planet-focused"); step = 4; step_time = elapsed
    elif step == 4 and elapsed - step_time > 1.0:
        finish()
    return false
'''


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticHeroVisibleRuntime(unittest.TestCase):
    campaign = "Sandbox_Equal_Footing_Empire"
    hero = "Probe_Droid_Team"

    def launch(self, directory, name, extra, script=None):
        report = directory / (name + ".json")
        command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                   "--path", str(ROOT / "apps/viewer/project")]
        if os.environ.get("EAWR_JOB_GC_RENDER_THREAD"):
            command += ["--render-thread", os.environ["EAWR_JOB_GC_RENDER_THREAD"]]
        if script:
            command += ["--script", str(script)]
        command += ["--", "--eawr-galactic", self.campaign, "--eawr-game-root", GAME_ROOT,
                    "--eawr-report", str(report), "--eawr-environment", "map", "--eawr-lighting", "sh",
                    "--eawr-shadows", "on", *extra]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=600, check=False)
        (directory / (name + ".log")).write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
        return data

    def test_probe_droid_hero_buy_and_see(self):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-hero-visible-"))
        # Stock campaign, no mod root and no fixture: the paused first frame is the player's starting screen.
        initial = self.launch(directory, "initial", ["--eawr-galactic-paused", "yes",
                                                     "--eawr-capture", str(directory / "00-start.png")])
        bar = initial["command_bar"]
        points = {control["name"]: [control["hit_rect"][0] + control["hit_rect"][2] / 2,
                                   control["hit_rect"][1] + control["hit_rect"][3] / 2]
                  for control in bar["controls"]}
        self.assertFalse(bar["space_tab"], "the campaign opens on the ground tab")

        planet = initial["map_input"]["selected"]

        def drive(name, card):
            driver = directory / (name + ".gd")
            driver.write_text(DRIVER % {"card": json.dumps(card), "tab": json.dumps("b_space_tab"),
                                        "hero_icon": json.dumps("fleet_presence_%d_1" % planet),
                                        "hit_points": json.dumps(points),
                                        "directory": json.dumps(directory.as_posix()), "limit": "90.0"},
                              encoding="utf-8")
            return self.launch(directory, name, ["--eawr-galactic-paused", "yes"], script=driver)

        # Step 1: the player opens the space tab (real click) and reads the cards.
        opened = drive("tab", "")
        choices = {choice["type"]: choice for choice in opened["command_bar"]["production_choices"]}
        (directory / "space-choices.json").write_text(json.dumps(
            {"space_tab": opened["command_bar"]["space_tab"], "choices": list(choices.values()),
             "selected": opened["map_input"]["selected"]}, indent=1), encoding="utf-8")
        self.assertTrue(opened["command_bar"]["space_tab"])
        self.assertIn(self.hero, choices, "the hero's build card is on the space tab")
        card = choices[self.hero]
        self.assertTrue(card["enabled"], "the hero's card is not greyed out")
        self.assertEqual(card["price_text"], str(card["price"]))
        # Step 2: buy it, run the clock until the queue empties, look at the result.
        final = drive("final", card["button"])
        (directory / "final-summary.json").write_text(json.dumps(
            {"queues": final.get("production_queues"), "fleet_hud": final.get("fleet_hud"),
             "command_bar": {key: final["command_bar"][key] for key in ("account", "space_tab", "queue_slots")},
             "status": final.get("status"), "live_session": final.get("live_session")}, indent=1),
            encoding="utf-8")
        self.assertEqual(final["status"], "closed")
        self.assertEqual(final["production_queues"], [], "the hero finished building")
        self.assertEqual(final["command_bar"]["account"], str(12000 - card["price"]))
        hero_cards = [group for row in final["fleet_hud"]["cards"] if row["local"] and row["planet"] == planet
                      for group in row["groups"] if group["type"] == card["type_id"]]
        self.assertEqual(len(hero_cards), 1, "the produced hero is a fleet group at the producing planet")
        notes = json.loads((directory / "driver.json").read_text(encoding="utf-8"))
        clicked = [row for row in notes if row.get("click_node") == "fleet_presence_%d_1" % planet]
        self.assertEqual([(row["found"], row["visible"]) for row in clicked], [(True, True)],
                         "the hero has its own fleet icon on the galaxy map")
        # #683: the stock Empire plot places its heroes at the first service of the unpaused clock (the group's hero
        # flag is only set for disclosed non-local types, so count groups). The capital's orbit holds the starting
        # ships (slot 0), a story hero fleet and the bought hero; its surface holds the story heroes beside the two
        # stock squads.
        local_groups = [(row["slot"], group) for row in final["fleet_hud"]["cards"]
                        if row["local"] and row["planet"] == planet for group in row["groups"]]
        (directory / "capital-groups.json").write_text(json.dumps(local_groups, indent=1), encoding="utf-8")
        extra = [group for slot, group in local_groups if slot != 0 and group["type"] != card["type_id"]]
        self.assertTrue(extra, "a story hero fleet sits in orbit at the capital beside the bought hero")
        self.assertGreater(int(final["ground_hud"]["count"]), 2, "story heroes join the surface roster")
        self.assertEqual(final["flights"][0]["state"], "focused", "a real double click focuses the planet")
        panels = {row["name"]: row for row in final["fleet_hud"]["controls"]}
        self.assertTrue(panels["q2_fleet"]["visible"], "the hero fills its own fleet panel")
        self.assertEqual(panels["q2_fleet"]["count"], "1")
        print("HERO-VISIBLE-DIRECTORY", directory)
        self.assertEqual(final["status"], "closed")


if __name__ == "__main__":
    unittest.main()
