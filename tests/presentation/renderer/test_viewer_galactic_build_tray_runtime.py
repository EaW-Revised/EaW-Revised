"""GBT-01 to GBT-03, GBT-13: the galactic build tray in the galaxy view (docs/ui/galactic-build-tray.md).

A stock Empire sandbox staged at tech 5 with a level 5 star base offers more space choices than the bar has slots, so
the handle appears; the stock start fits the bar, so it does not. A tray entry buys through the bar's own path. With
the Tweaks switch off nothing is built and nothing about the tray is reported, even with a broken grouping file.
"""
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

from tests.presentation.renderer import test_viewer_galactic_command_bar_runtime as support

ROOT, RUNTIME, GAME_ROOT = support.ROOT, support.RUNTIME, support.GAME_ROOT
CAMPAIGN = "Sandbox_Equal_Footing_Empire"

DRIVER = '''extends SceneTree
var frame = 0
var elapsed = 0.0
var seen = -1
var opened = -1
var done = false
var space = JSON.parse_string(%(space)s)
var open_tray = %(open)s
var clicks = JSON.parse_string(%(clicks)s)
var capture_path = %(capture)s
func _initialize(): call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func press(position):
    var motion = InputEventMouseMotion.new()
    motion.position = position
    motion.global_position = position
    Input.parse_input_event(motion)
    for down in [true, false]:
        var event = InputEventMouseButton.new()
        event.button_index = MOUSE_BUTTON_LEFT
        event.pressed = down
        event.position = position
        event.global_position = position
        Input.parse_input_event(event)
var states = []
func key(name, ctrl):
    for down in [true, false]:
        var event = InputEventKey.new()
        event.keycode = OS.find_keycode_from_string(name)
        # A bare key also carries its physical code (the tray key reads it); a Ctrl chord is sent as the planet
        # search's own tests send it, the key code alone.
        if not ctrl: event.physical_keycode = event.keycode
        event.ctrl_pressed = ctrl
        event.pressed = down
        Input.parse_input_event(event)
func hover(position):
    var motion = InputEventMouseMotion.new()
    motion.position = position
    motion.global_position = position
    Input.parse_input_event(motion)
func shown(name):
    var node = root.find_child(name, true, false)
    return node != null and node.is_visible_in_tree()
func finish():
    done = true
    var file = FileAccess.open(capture_path.get_basename() + "-steps.json", FileAccess.WRITE)
    file.store_string(JSON.stringify(states))
    file.close()
    root.get_texture().get_image().save_png(capture_path)
    current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
    quit()
func _process(delta):
    elapsed += delta
    if done or elapsed < 1.0: return false
    frame += 1
    # The space tab's hit area (from a first run's report) is smaller than its art: press its centre.
    if frame == 40 and space != null: press(Vector2(space[0], space[1]))
    if frame < 80: return false
    if not open_tray:
        if frame == 100: finish()
        return false
    var handle = root.find_child("GalacticBuildTrayHandle", true, false)
    if seen < 0 and handle != null and handle.is_visible_in_tree(): seen = frame
    if seen > 0 and opened < 0 and frame == seen + 20:
        press(handle.get_global_rect().get_center())
        opened = frame
    if opened > 0:
        var step = (frame - opened) / 20
        if (frame - opened) %% 20 == 0 and step >= 1 and step <= clicks.size():
            # A step is a click [x, y], a key {"key": name, "ctrl": bool} or a pointer move {"move": [x, y]}.
            var at = clicks[step - 1]
            if at is Dictionary and at.has("move"): hover(Vector2(at["move"][0], at["move"][1]))
            elif at is Dictionary: key(at["key"], at.get("ctrl", false))
            else: press(Vector2(at[0], at[1]))
        if (frame - opened) %% 20 == 15 and step >= 1 and step <= clicks.size():
            states.append({"handle": shown("GalacticBuildTrayHandle"), "panel": shown("GalacticBuildTray")})
        if frame - opened >= 20 * (clicks.size() + 1) + 20: finish()
    if frame > 900: finish()
    return false
'''


def centre(rect):
    return [rect[0] + rect[2] / 2, rect[1] + rect[3] / 2]


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires the GPU viewer and installed FoC")
class GalacticBuildTrayRuntime(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from tools.inventory.corpus import Corpus

        cls.directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-build-tray-"))
        # The Revised layer with the shipped tray files (GBT-11, GBT-12), mounted through the settings file.
        cls.layer = cls.directory / "revised"
        layer_xml = cls.layer / "Data/XML"
        layer_xml.mkdir(parents=True)
        for name in ("RevisedBuildTray.xml", "RevisedBuildTrayGroups.xml"):
            shutil.copyfile(ROOT / "tools/revised_build_tray" / name, layer_xml / name)
        # The planet search (opened with Ctrl+F in the open-state case) builds its dialog from its own layer file.
        shutil.copyfile(ROOT / "tools/revised_planet_search/RevisedPlanetSearch.xml", layer_xml / "RevisedPlanetSearch.xml")
        # A broken copy of the layer: the grouping file does not parse.
        cls.broken_layer = cls.directory / "revised-broken"
        shutil.copytree(cls.layer, cls.broken_layer)
        (cls.broken_layer / "Data/XML/RevisedBuildTrayGroups.xml").write_text("<Not_The_Tray_Groups/>", encoding="utf-8")
        # The overflowing start: tech 5, a level 5 star base at Coruscant and credits for every choice.
        cls.mod = cls.directory / "mod"
        xml = cls.mod / "Data/XML"
        xml.mkdir(parents=True)
        (cls.mod / "Data/MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
        source = Corpus(pathlib.Path(GAME_ROOT)).read_effective("foc", "data/xml/campaigns_underworld_gc.xml")
        tree = ET.fromstring(source.data)
        campaign = next(node for node in tree if node.get("Name") == CAMPAIGN)
        for node in campaign:
            text = (node.text or "").strip()
            if node.tag == "Starting_Tech_Level" and text.startswith("Empire,"):
                node.text = "Empire, 5"
            elif node.tag == "Starting_Credits" and text.startswith("Empire,"):
                node.text = "Empire, 50000"
            elif node.tag == "Starting_Forces":
                fields = [value.strip() for value in text.split(",")]
                if len(fields) == 3 and fields[:2] == ["Empire", "Coruscant"] and "Star_Base" in fields[2]:
                    node.text = "Empire, Coruscant, Empire_Star_Base_5"
        (xml / "campaigns_underworld_gc.xml").write_bytes(ET.tostring(tree, encoding="utf-8"))
        # GBT-14: a copy whose Coruscant also has the stock ground factories, so the GROUND tab overflows too.
        cls.ground_mod = cls.directory / "mod-ground"
        shutil.copytree(cls.mod, cls.ground_mod)
        for name in ("E_Ground_Barracks", "E_Ground_Light_Vehicle_Factory", "E_Ground_Heavy_Vehicle_Factory",
                     "E_Ground_Advanced_Vehicle_Factory", "E_Ground_Officer_Academy"):
            forces = ET.SubElement(campaign, "Starting_Forces")
            forces.text = "Empire, Coruscant, " + name
        (cls.ground_mod / "Data/XML/campaigns_underworld_gc.xml").write_bytes(ET.tostring(tree, encoding="utf-8"))

    def tab_point(self, name):
        """The centre of a production tab's hit area, read once from a run on the overflowing start."""
        cls = type(self)
        if getattr(cls, "_tab_points", None) is None:
            initial, _ = self.run_viewer("initial", overflow=True, space=False)
            cls._tab_points = {control["name"]: centre(control["hit_rect"])
                               for control in initial["command_bar"]["controls"] if control["name"].endswith("_tab")}
        return cls._tab_points[name]

    def space_point(self):
        return self.tab_point("b_space_tab")

    def run_viewer(self, name, *, overflow, space, open_tray=False, clicks=(), switch=None, layer=None, tray_key=None,
                   mod=None):
        settings = self.directory / (name + ".cfg")
        lines = ["eawr-settings 1", "revised_layer_enabled=1", "revised_layer_path=" + (layer or self.layer).as_posix()]
        if switch is not None:
            lines.append("gameplay.galactic_build_tray=" + ("1" if switch else "0"))
        if tray_key is not None:
            lines.append("key.GALACTIC_BUILD_TRAY=" + tray_key)
        settings.write_text("\n".join(lines) + "\n", encoding="utf-8")
        report, capture, driver = (self.directory / (name + suffix) for suffix in (".json", ".png", ".gd"))
        driver.write_text(DRIVER % {
            "space": repr(json.dumps(self.space_point() if space else None)), "open": "true" if open_tray else "false",
            "clicks": repr(json.dumps([step if isinstance(step, dict) else list(step) for step in clicks])),
            "capture": json.dumps(capture.as_posix())},
            encoding="utf-8")
        arguments = ["--eawr-galactic", CAMPAIGN, "--eawr-game-root", GAME_ROOT, "--eawr-galactic-paused", "yes",
                     "--eawr-galactic-dismiss-story", "yes", "--eawr-settings-file", str(settings), "--eawr-report", str(report),
                     "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on"]
        if overflow:
            arguments += ["--eawr-mod-root", str(mod or self.mod)]
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                                 "--path", str(ROOT / "apps/viewer/project"), "--script", str(driver), "--", *arguments],
                                cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        log = result.stdout + result.stderr
        (self.directory / (name + ".log")).write_text(log, encoding="utf-8")
        self.assertTrue(report.is_file(), log)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, data.get("failure") or log)
        self.assertTrue(capture.is_file(), log)
        return data, log

    def test_a_fitting_planet_has_no_handle_and_the_bar_is_unchanged(self):
        data, _ = self.run_viewer("fitting", overflow=False, space=False)
        tray = data["build_tray"]
        self.assertTrue(tray["enabled"], tray)
        self.assertEqual(tray["omitted"], 0, "the stock start fits the bar")
        self.assertFalse(tray["handle"], tray)
        self.assertEqual(tray["entries"], 0, tray)
        off, _ = self.run_viewer("fitting-off", overflow=False, space=False, switch=False)
        self.assertEqual(data["command_bar"]["production_choices"], off["command_bar"]["production_choices"],
                         "the bar's cards are the same with the tray on and off")

    def test_switch_off_builds_and_reports_nothing(self):
        data, log = self.run_viewer("off", overflow=True, space=True, switch=False, layer=self.broken_layer)
        tray = data["build_tray"]
        self.assertFalse(tray["enabled"], tray)
        self.assertFalse(tray["handle"], tray)
        self.assertNotIn("EAWR-UI-BUILD-TRAY", log, "off means no tray files are read past the layout and nothing is reported")
        # The same broken file with the switch on is reported.
        _, on_log = self.run_viewer("broken-on", overflow=True, space=True, layer=self.broken_layer)
        self.assertIn("EAWR-UI-BUILD-TRAY: invalid grouping file", on_log)

    def test_an_overflowing_planet_opens_the_tray_and_an_entry_buys_like_the_bar(self):
        opened, _ = self.run_viewer("open", overflow=True, space=True, open_tray=True)
        tray = opened["build_tray"]
        self.assertTrue(opened["command_bar"]["space_tab"], opened["command_bar"])
        self.assertGreater(tray["omitted"], 0, "tech 5 with a level 5 star base offers more space choices than the bar holds")
        self.assertTrue(tray["handle"] and tray["open"], tray)
        self.assertEqual(tray["handle_presses"], 1, tray)
        entries = tray["shown_entries"]
        self.assertEqual(len(entries), tray["entries"], tray)
        self.assertGreater(len(entries), tray["omitted"], "the tray lists every choice, not only the omitted ones")
        hits = {control["name"]: control["hit_rect"] for control in opened["command_bar"]["controls"]}
        bar = {card["type_id"]: dict(card, button_rect=hits.get(card["button"], card["button_rect"]))
               for card in opened["command_bar"]["production_choices"]}
        only_tray = next(entry for entry in entries if entry["type_id"] not in bar and entry["enabled"])
        both = next(entry for entry in entries if entry["type_id"] in bar and entry["enabled"] and bar[entry["type_id"]]["enabled"])

        bought, _ = self.run_viewer("buy", overflow=True, space=True, open_tray=True, clicks=[
            centre(only_tray["rect"]), centre(bar[both["type_id"]]["button_rect"]), centre(both["rect"])])
        # The session submits one order of its own at startup; each of the three clicks adds one (tray, bar, tray).
        self.assertEqual(bought["live_session"]["submitted_orders"] - opened["live_session"]["submitted_orders"], 3,
                         "one order per click")
        queued = [(row["domain"], row["type"], row["price"]) for row in bought["production_queues"]]
        self.assertIn(only_tray["type_id"], [row[1] for row in queued], "a choice the bar omits is bought from the tray")
        same = [row for row in queued if row[1] == both["type_id"]]
        self.assertEqual(len(same), 2, queued)
        self.assertEqual(same[0], same[1], "the tray entry queues exactly what the bar card queues")

    def test_the_open_state_lasts_and_the_tray_key_acts_like_the_handle(self):
        """GBT-07: Tab closes and opens the tray like the handle; a planet tab whose choices fit hides handle and tray
        but keeps the open state, so the tray is back on the overflowing tab; a focused text field keeps the key."""
        ground, space = self.tab_point("b_planet_tab"), self.space_point()
        data, _ = self.run_viewer("open-state", overflow=True, space=True, open_tray=True, clicks=[
            {"key": "Tab"}, {"key": "Tab"}, ground, space, {"move": [640, 160]}, {"key": "F", "ctrl": True}, {"key": "Tab"}])
        steps = json.loads((self.directory / "open-state-steps.json").read_text(encoding="utf-8"))
        self.assertEqual(len(steps), 7, steps)
        self.assertEqual(steps[0], {"handle": True, "panel": False}, "Tab closes the open tray")
        self.assertEqual(steps[1], {"handle": True, "panel": True}, "Tab opens it again")
        self.assertEqual(steps[2], {"handle": False, "panel": False}, "the ground tab fits: no handle and no tray")
        self.assertEqual(steps[3], {"handle": True, "panel": True}, "back on the space tab the tray is still open")
        self.assertTrue(steps[5]["panel"] and steps[6]["panel"], "the planet search's text field keeps Tab")
        self.assertGreaterEqual(data["planet_search"]["opens"], 1, "Ctrl+F opened the planet search")
        tray = data["build_tray"]
        self.assertEqual((tray["handle_presses"], tray["key_presses"], tray["open"]), (1, 2, True), tray)

    def test_a_rebound_key_fires_and_is_ignored_with_modifiers_without_a_handle_and_under_a_modal(self):
        """GBT-07: with the key rebound to B, Tab and Ctrl+B do nothing and B toggles; on a tab whose choices fit (no
        handle) B does nothing; with the menu open (a modal) B does nothing."""
        ground, space = self.tab_point("b_planet_tab"), self.space_point()
        data, _ = self.run_viewer("rebound", overflow=True, space=True, open_tray=True, tray_key="B", clicks=[
            {"key": "Tab"}, {"key": "B", "ctrl": True}, {"key": "B"}, {"key": "B"}, ground, {"key": "B"}, space,
            {"key": "Escape"}, {"key": "B"}])
        steps = json.loads((self.directory / "rebound-steps.json").read_text(encoding="utf-8"))
        self.assertEqual(len(steps), 9, steps)
        self.assertTrue(steps[0]["panel"] and steps[1]["panel"], "Tab and Ctrl+B leave the tray as it is")
        self.assertEqual(steps[2], {"handle": True, "panel": False}, "the rebound key closes the tray")
        self.assertEqual(steps[3], {"handle": True, "panel": True}, "and opens it")
        self.assertEqual(steps[5], {"handle": False, "panel": False}, "no handle: the key does nothing")
        self.assertTrue(steps[6]["panel"], "the open state is kept for the overflowing tab")
        tray = data["build_tray"]
        self.assertEqual(tray["key_presses"], 2, "only the two bare B presses with a handle and no modal count")

    def test_queued_items_are_never_covered_by_the_tray_and_never_cover_the_handle(self):
        """GBT-14 (owner pick B): with items queued, the queue strip the open tray would cover moves up above the tray,
        and no strip (space or land) covers the handle; closing the tray puts the strip back above the handle only."""
        opened, _ = self.run_viewer("queue-open", overflow=True, space=True, open_tray=True)
        cards = {control["name"]: control["hit_rect"] for control in opened["command_bar"]["controls"]}
        data, _ = self.run_viewer("queued", overflow=True, space=True, open_tray=True, clicks=[
            centre(cards["b_Create_00"]), centre(cards["b_Create_01"])])
        tray = data["build_tray"]
        self.assertTrue(tray["open"], tray)
        queue = [control for control in data["command_bar"]["controls"]
                 if control["name"].startswith("queue") and control["visible"]]
        self.assertGreaterEqual(len(queue), 2, "two items are queued and shown")

        def meets(a, b, slack=0.01):
            # The strip sits tight on what it clears (gap 0), and the report prints six significant digits: an overlap
            # under a hundredth of a pixel is that rounding, not a covered item.
            return a[2] > 0 and a[3] > 0 and b[2] > 0 and b[3] > 0 and a[0] < b[0] + b[2] - slack \
                and b[0] < a[0] + a[2] - slack and a[1] < b[1] + b[3] - slack and b[1] < a[1] + a[3] - slack
        for control in queue:
            self.assertFalse(meets(control["rect"], tray["panel_rect"]), f"the open tray covers {control}")
            self.assertFalse(meets(control["rect"], tray["handle_rect"]), f"{control} covers the handle")
        self.assertGreater(tray["queue_lift"][0], 0, "the space strip was lifted above the open tray")

        closed, _ = self.run_viewer("queued-closed", overflow=True, space=True, open_tray=True, clicks=[
            centre(cards["b_Create_00"]), centre(cards["b_Create_01"]), {"key": "Tab"}])
        tray = closed["build_tray"]
        self.assertFalse(tray["open"], tray)
        for control in closed["command_bar"]["controls"]:
            if control["name"].startswith("queue") and control["visible"]:
                self.assertFalse(meets(control["rect"], tray["handle_rect"]), f"{control} covers the handle")
        shown = [control["rect"] for control in closed["command_bar"]["controls"]
                 if control["name"].startswith("queue") and control["visible"]]
        handle = tray["handle_rect"]
        if tray["queue_lift"][0] > 0:
            bottom = max(rect[1] + rect[3] for rect in shown if rect[0] < handle[0] + handle[2] and handle[0] < rect[0] + rect[2])
            self.assertTrue(handle[1] - 12 <= bottom <= handle[1] + 0.01,  # 0.01: the report's six-digit rounding
                            f"closed, the strip is lifted just clear of the handle (bottom {bottom}, handle top {handle[1]})")

    def test_the_land_strip_is_never_covered_on_the_ground_tab(self):
        """GBT-14 on the GROUND tab: with the stock ground factories at Coruscant the ground list overflows; land units
        queued with the tray open: no visible land-strip slot meets the tray or the handle."""
        opened, _ = self.run_viewer("ground-open", overflow=True, space=False, open_tray=True, mod=self.ground_mod)
        tray = opened["build_tray"]
        self.assertFalse(opened["command_bar"]["space_tab"], "the ground tab is shown")
        self.assertGreater(tray["omitted"], 0, "the ground list overflows the bar")
        self.assertTrue(tray["open"], tray)
        cards = {control["name"]: control["hit_rect"] for control in opened["command_bar"]["controls"]}
        data, _ = self.run_viewer("ground-queued", overflow=True, space=False, open_tray=True, mod=self.ground_mod, clicks=[
            centre(cards["b_Create_00"]), centre(cards["b_Create_01"])])
        tray = data["build_tray"]
        land = [control for control in data["command_bar"]["controls"] if control["name"].startswith("queue")
                and control["visible"]]
        self.assertGreaterEqual(len(land), 1, "land units are queued and shown")
        for control in land:
            for other, what in ((tray["panel_rect"], "the open tray"), (tray["handle_rect"], "the handle")):
                # Under a hundredth of a pixel is the report's six-digit rounding, not an overlap (strips sit tight, gap 0).
                rect, slack = control["rect"], 0.01
                meets = rect[0] < other[0] + other[2] - slack and other[0] < rect[0] + rect[2] - slack \
                    and rect[1] < other[1] + other[3] - slack and other[1] < rect[1] + rect[3] - slack
                self.assertFalse(meets and other[2] > 0, f"{control} meets {what}")

    def test_the_panel_wears_the_bars_art_its_entries_are_three_quarters_and_the_close_button_closes_it(self):
        """GBT-06, GBT-07, GBT-10 (slice 6b): the shipped layout draws the panel from the bar's own shell art, sizes it
        automatically from the bar's width, shows the entries at 75 percent and offers a small close icon in its top right
        corner that covers no entry and closes the tray like the tray key; the handle opens it again."""
        data, _ = self.run_viewer("look", overflow=True, space=True, open_tray=True)
        tray = data["build_tray"]
        self.assertTrue(tray["open"], tray)
        self.assertEqual(tray["backdrop"]["source"], "bar", "the back is the bar's own shell art, no named component")
        self.assertGreater(tray["backdrop"]["back_layers"], 0, tray["backdrop"])
        self.assertGreater(tray["backdrop"]["frame_layers"], 0, "the stock bar has art just above its slots for the frame")
        self.assertEqual(tray["width_mode"], "auto", tray)
        self.assertLessEqual(tray["rows"], 7, "the 7-row budget")
        self.assertEqual(tray["lost"], 0, "every choice has a place: the rows fill, then the panel widens")
        panel, bar = tray["panel_rect"], tray["bar_rect"]
        self.assertGreaterEqual(panel[2] + 0.5, bar[2], "the automatic width starts at the bar's width")
        self.assertLessEqual(panel[0] + panel[2], 1280 + 0.5, "and never leaves the view")
        self.assertAlmostEqual(panel[0], bar[0], delta=0.5, msg="the panel starts at the bar's left edge")
        card = next(control["rect"] for control in data["command_bar"]["controls"] if control["name"] == "b_Create_00")
        scale = tray["entry_scale"]
        self.assertIn(scale, (0.75, 0.6), tray)
        for entry in tray["shown_entries"]:
            self.assertAlmostEqual(entry["rect"][2], card[2] * scale, delta=0.5, msg=entry)
            self.assertAlmostEqual(entry["rect"][3], card[3] * scale, delta=0.5, msg=entry)
            self.assertTrue(panel[0] - 0.5 <= entry["rect"][0] and entry["rect"][0] + entry["rect"][2] <= panel[0] + panel[2] + 0.5,
                            entry)
        close = tray["close"]
        self.assertTrue(close["visible"], close)
        self.assertEqual(close["art"], "bar_button", "no mod close icon is named: the bar's own button art")
        self.assertTrue(close["tooltip"], "the close icon has the game's close tooltip")
        rect = close["rect"]
        self.assertLess(rect[2], card[2] * 0.5, "a small icon, well under half a bar card")
        self.assertAlmostEqual(rect[2], rect[3], delta=0.5, msg="square")
        self.assertTrue(panel[0] + panel[2] / 2 < rect[0] and rect[0] + rect[2] <= panel[0] + panel[2] + 0.5
                        and panel[1] - 0.5 <= rect[1] and rect[1] + rect[3] < panel[1] + panel[3] / 2,
                        "inside the panel's top right corner")
        for entry in tray["shown_entries"]:
            box = entry["rect"]
            self.assertFalse(box[0] < rect[0] + rect[2] and rect[0] < box[0] + box[2]
                             and box[1] < rect[1] + rect[3] and rect[1] < box[1] + box[3], f"the icon covers {entry}")

        closed, _ = self.run_viewer("close", overflow=True, space=True, open_tray=True, clicks=[
            centre(close["rect"]), centre(tray["handle_rect"])])
        steps = json.loads((self.directory / "close-steps.json").read_text(encoding="utf-8"))
        self.assertEqual(steps[0], {"handle": True, "panel": False}, "the close button closes the tray")
        self.assertEqual(steps[1], {"handle": True, "panel": True}, "the handle opens it again")
        self.assertEqual((closed["build_tray"]["close"]["presses"], closed["build_tray"]["handle_presses"]), (1, 2),
                         closed["build_tray"])


if __name__ == "__main__":
    unittest.main()
