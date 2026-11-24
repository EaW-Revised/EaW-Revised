"""GBT-01 to GBT-03, GBT-13: the galactic build tray in the galaxy view (docs/ui/galactic-build-tray.md).

A stock Empire sandbox staged at tech 5 with a level 5 star base offers more space choices than the bar has slots, so
the handle reads +N; the stock start fits the bar, and its handle is there all the same, with no count, and the tray
and Tab open on it (owner, 2026-11-19). A tray entry buys through the bar's own path. With the Tweaks switch off
nothing is built and nothing about the tray is reported, even with a broken grouping file.
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
var probe = %(probe)s
var probe_at = -1
var probe_log = {}
var probe_target = null
var probe_close_at = -1
var probe_pos = Vector2()
var plan_mode = %(plan)s
var plan = []
var plan_index = -1
var plan_done = 0
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
func behind_buttons():
    # Hud buttons of another shell (the planet menu) whose centre lies under the open tray panel.
    var panel = root.find_child("GalacticBuildTray", true, false)
    var found = []
    if panel == null or not panel.is_visible_in_tree(): return found
    var bar = panel.get_parent()
    var area = panel.get_global_rect()
    for node in root.find_children("*", "EawrHudButton", true, false):
        var host = node.get_parent()
        if host == bar or host == null or host.get_class() != "EawrTacticalHud" or not node.is_visible_in_tree(): continue
        if area.has_point(node.get_global_rect().get_center()): found.append(node)
    return found
func tray_targets():
    # A tray card, a category header and an empty point of the panel (nothing of the tray drawn there).
    var panel = root.find_child("GalacticBuildTray", true, false)
    var found = {}
    if panel == null or not panel.is_visible_in_tree(): return found
    var rects = []
    probe_log["children"] = []
    for node in panel.get_parent().get_children():
        probe_log["children"].append([node.get_class(), str(node.name), node.visible if node is CanvasItem else null, str(node.get_global_rect().size) if node is Control else ""])
        if node == panel or not (node is Control) or not node.visible: continue
        var rect = node.get_global_rect()
        # A header label's rect has no height of its own (its text is drawn from its top edge): click on that edge.
        if node.get_class() == "EawrHudText" and rect.size.x > 0:
            if not found.has("header"): found["header"] = rect.position + Vector2(rect.size.x * 0.5, 3)
            continue
        if rect.size.x <= 0 or rect.size.y <= 0: continue
        rects.append(rect)
        if node.get_class() == "EawrHudButton" and not node.name.begins_with("GalacticBuildTray") and not found.has("card"): found["card"] = rect.get_center()
    var area = panel.get_global_rect()
    var y = area.position.y + 6
    while y < area.end.y - 6 and not found.has("background"):
        var x = area.position.x + 6
        while x < area.end.x - 6:
            var point = Vector2(x, y)
            var covered = false
            for rect in rects:
                if rect.has_point(point):
                    covered = true
                    break
            if not covered:
                found["background"] = point
                break
            x += 6
        y += 6
    return found
func build_plan():
    var targets = tray_targets()
    probe_log["targets"] = {}
    for name in targets: probe_log["targets"][name] = [targets[name].x, targets[name].y]
    for name in targets:
        var p = targets[name]
        plan.append(["hover", p])
        for button in [MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_MIDDLE]:
            plan.append(["button", p, button, true, false])
            plan.append(["button", p, button, false, false])
            plan.append(["button", p, button, true, true])
            plan.append(["button", p, button, false, false])
            plan.append(["button", p, button, true, false])
            for step in range(1, 5): plan.append(["motion", p + Vector2(-60 * step, 10 * step), button])
            plan.append(["button", p + Vector2(-240, 40), button, false, false])
        for wheel in [MOUSE_BUTTON_WHEEL_UP, MOUSE_BUTTON_WHEEL_DOWN]:
            plan.append(["button", p, wheel, true, false])
            plan.append(["button", p, wheel, false, false])
    probe_log["events"] = plan.size()
func send_item(item):
    var kind = item[0]
    var p = item[1]
    if kind == "hover":
        hover(p)
        return
    if kind == "motion":
        var motion = InputEventMouseMotion.new()
        motion.position = p
        motion.global_position = p
        motion.button_mask = 1 << (item[2] - 1)
        Input.parse_input_event(motion)
        return
    var event = InputEventMouseButton.new()
    event.position = p
    event.global_position = p
    event.button_index = item[2]
    event.pressed = item[3]
    event.double_click = item[4]
    event.button_mask = (1 << (item[2] - 1)) if (item[3] and item[2] <= 3) else 0
    Input.parse_input_event(event)
func hovered_info():
    var control = root.gui_get_hovered_control()
    if control == null: return {"path": "", "z": -1, "in_layer": false}
    var panel = root.find_child("GalacticBuildTray", true, false)
    return {"path": str(control.get_path()), "z": control.z_index, "in_layer": panel != null and control.get_parent() == panel.get_parent()}
func shown(name):
    var node = root.find_child(name, true, false)
    return node != null and node.is_visible_in_tree()
func finish():
    done = true
    if probe or plan_mode: states.append({"probe": probe_log})
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
    if plan_mode and opened > 0:
        if plan_index < 0 and frame == opened + 30:
            build_plan()
            plan_index = 0
        elif plan_index >= 0:
            if plan_index < plan.size():
                send_item(plan[plan_index])
                plan_index += 1
            else:
                plan_done += 1
                if plan_done == 10: finish()
        return false
    if probe and opened > 0:
        # The pointer over a planet-menu button behind the open tray: the tray must hold the hover and the tooltip.
        var behind = behind_buttons()
        if probe_at < 0 and frame == opened + 30:
            probe_log["behind"] = behind.size()
            if behind.is_empty(): finish()
            else:
                probe_target = behind[0]
                probe_log["target"] = str(probe_target.get_path())
                probe_pos = probe_target.get_global_rect().get_center()
                hover(probe_pos)
                probe_at = frame
        elif probe_at > 0 and frame == probe_at + 30:
            probe_log["hovered"] = hovered_info()
            probe_log["target_hovered"] = probe_target.is_hovered()
            # Close the tray from its handle: the planet view gets the pointer back.
            press(handle.get_global_rect().get_center())
            probe_close_at = frame
            probe_at = -2
        elif probe_close_at > 0 and frame == probe_close_at + 20:
            hover(probe_pos)
        elif probe_close_at > 0 and frame == probe_close_at + 50:
            probe_log["closed_hovered"] = hovered_info()
            finish()
        return false
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
        # Since the bar binds every numbered slot (GHUD-S91, 26 in stock FoC), Coruscant's choices fit the bar; the mod
        # authors the slot limit (GHUD-S92) at 16, 8 per row, so the space list overflows and the tray has work to do.
        constants = ET.fromstring(Corpus(pathlib.Path(GAME_ROOT)).read_effective("foc", "data/xml/gameconstants.xml").data)
        for old in constants.findall("Strategic_Command_Bar_Buildable_Row_Count"):
            constants.remove(old)
        ET.SubElement(constants, "Strategic_Command_Bar_Buildable_Row_Count").text = "16"
        (xml / "GameConstants.xml").write_bytes(ET.tostring(constants, encoding="utf-8"))
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
                   mod=None, size="1280x720", probe=False, extra=(), plan=False):
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
            "capture": json.dumps(capture.as_posix()), "probe": "true" if probe else "false", "plan": "true" if plan else "false"},
            encoding="utf-8")
        arguments = ["--eawr-galactic", CAMPAIGN, "--eawr-game-root", GAME_ROOT, "--eawr-galactic-paused", "yes",
                     "--eawr-galactic-dismiss-story", "yes", "--eawr-settings-file", str(settings), "--eawr-report", str(report),
                     "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on"]
        if overflow:
            arguments += ["--eawr-mod-root", str(mod or self.mod)]
        arguments += list(extra)
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", size,
                                 "--path", str(ROOT / "apps/viewer/project"), "--script", str(driver), "--", *arguments],
                                cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        log = result.stdout + result.stderr
        (self.directory / (name + ".log")).write_text(log, encoding="utf-8")
        self.assertTrue(report.is_file(), log)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, data.get("failure") or log)
        self.assertTrue(capture.is_file(), log)
        return data, log

    def test_the_open_tray_holds_the_pointer_over_the_planet_menu_behind_it(self):
        # #8029: zoomed on a planet, the open tray overlaps the planet menu's buttons. The pointer over one of them is
        # the tray's: the hovered control is a tray control (a child of the tray's own layer, placed after both HUD shells)
        # and the planet-menu button behind it is not hovered, so no hover art and no popup reach through.
        data, _ = self.run_viewer("behind", overflow=True, space=True, open_tray=True, probe=True,
                                  extra=("--eawr-galactic-focus", "15", "--eawr-galactic-flight-ms", "750"))
        steps = json.loads((self.directory / "behind-steps.json").read_text(encoding="utf-8"))
        probe = next(step["probe"] for step in steps if "probe" in step)
        self.assertGreater(probe.get("behind", 0), 0, ("a planet-menu button lies under the open tray", probe))
        hovered = probe["hovered"]
        self.assertTrue(hovered["in_layer"], ("the tray holds the pointer: the hovered control is a child of the tray layer", probe))
        self.assertFalse(probe["target_hovered"], ("the planet-menu button behind the tray is not hovered", probe))
        self.assertFalse(probe["closed_hovered"]["in_layer"], ("with the tray closed it no longer holds the pointer over the planet view", probe))
        stack_popup = data["fleet_hud"]["stack_popup"]
        self.assertEqual(stack_popup["key"], 0, ("no fleet stack popup opens through the tray", stack_popup))

    def click_through_case(self, name, *, space, layer, mod):
        # Clicking the tray: a clicked run (press, release, double click, drag out of the tray, wheel; left, right and middle
        # button; on a card, a category header and an empty point of the panel) against an untouched run of the same start.
        # The planet view behind the tray gets no event (the count the planet view keeps of the mouse events it received
        # over the tray stays 0), and its selection, camera, drag and ground clicks are what the untouched run has.
        extra = ("--eawr-galactic-focus", "15", "--eawr-galactic-flight-ms", "750")
        quiet, _ = self.run_viewer(name + "-quiet", overflow=True, space=space, layer=layer, mod=mod, open_tray=True, extra=extra)
        data, _ = self.run_viewer(name, overflow=True, space=space, layer=layer, mod=mod, open_tray=True, plan=True, extra=extra)
        steps = json.loads((self.directory / (name + "-steps.json")).read_text(encoding="utf-8"))
        log = next(step["probe"] for step in steps if "probe" in step)
        self.assertEqual(data["build_tray"]["world_events_over_tray"], 0, (name, "the planet view got events over the tray"))
        self.assertEqual(data["map_input"]["selected"], quiet["map_input"]["selected"], (name, "the selection changed"))
        for key in ("eye", "target"):
            for now, before in zip(data["camera"][key], quiet["camera"][key]):
                self.assertAlmostEqual(now, before, places=3, msg=(name, "the camera moved", key))
        self.assertEqual(data["fleet_hud"]["drag"], quiet["fleet_hud"]["drag"], (name, "a drag started in the planet view"))
        self.assertEqual(data["ground_hud"]["clicks"], quiet["ground_hud"]["clicks"], (name, "the ground view got clicks"))
        self.assertEqual(set(log.get("targets", {})), {"card", "header", "background"}, (name, "the three click targets exist", log))
        self.assertGreater(log["events"], 100, (name, log))

    def test_clicks_on_the_open_tray_never_reach_the_planet_view(self):
        # #8029 (owner): the tray takes every click, double click, drag and wheel over it: the space and the ground tray, in
        # the Revised look (the bar's own shell art behind it) and with a component's art behind it (Back_Source Component).
        component = self.directory / "revised-component"
        layout = component / "Data/XML/RevisedBuildTray.xml"
        shutil.copytree(self.layer, component)
        text = layout.read_text(encoding="utf-8")
        self.assertIn("<Back_Source>Bar</Back_Source>", text)
        layout.write_text(text.replace("<Back_Source>Bar</Back_Source>",
                                       "<Back_Source>Component</Back_Source><Back_Component>tooltip_back</Back_Component>"),
                          encoding="utf-8")
        for look, layer in (("revised", self.layer), ("component", component)):
            for tab, space, mod in (("space", True, self.mod), ("ground", False, self.ground_mod)):
                self.click_through_case("click-%s-%s" % (look, tab), space=space, layer=layer, mod=mod)

    def test_the_bar_keeps_a_right_margin_at_every_window_size(self):
        # GHUD-S99 (#8022): the bar was clipped at the screen's right edge below a width and flush above it. Every drawn
        # control ends at least as far from the right edge as the first control starts from the left one, at the sizes
        # the owner plays (4:3 to ultrawide); the stock bar and the 16-slot bar of the overflow mod.
        sizes = ("1024x768", "1280x720", "1366x768", "1600x900", "1920x1080", "3440x1440")
        for overflow in (False, True):
            for size in sizes:
                width = int(size.split("x")[0])
                data, _ = self.run_viewer("margin-%s-%d" % (size, overflow), overflow=overflow, space=False, size=size)
                shown = [c["rect"] for c in data["command_bar"]["controls"] if c["visible"] and c["rect"][2] > 0]
                self.assertTrue(shown, (size, "the bar draws controls"))
                left_gap = min(rect[0] for rect in shown)
                right_gap = width - max(rect[0] + rect[2] for rect in shown)
                self.assertGreaterEqual(right_gap, left_gap - 0.5, (size, overflow, "right margin vs left", left_gap, right_gap))
                self.assertGreater(right_gap, 0.0, (size, overflow, "the bar ends inside the screen"))
                self.assertGreater(left_gap, 0.0, (size, overflow, "and starts inside it: the same margin on the left"))

    def test_a_fitting_planet_has_the_handle_without_a_count_and_the_bar_is_unchanged(self):
        data, _ = self.run_viewer("fitting", overflow=False, space=False)
        tray = data["build_tray"]
        self.assertTrue(tray["enabled"], tray)
        self.assertEqual(tray["omitted"], 0, "the stock start fits the bar")
        self.assertTrue(tray["handle"], "the handle is always there for a planet with choices")
        self.assertEqual(tray["handle_text"], "+", "no count when the bar omits nothing")
        self.assertFalse(tray["open"], tray)
        self.assertEqual(tray["entries"], 0, "closed: no entry is drawn")
        off, _ = self.run_viewer("fitting-off", overflow=False, space=False, switch=False)
        self.assertFalse(off["build_tray"]["handle"], off["build_tray"])
        self.assertEqual(data["command_bar"]["production_choices"], off["command_bar"]["production_choices"],
                         "the bar's cards are the same with the tray on and off")

    def test_a_fitting_planet_opens_the_tray_from_the_handle_and_the_key_with_the_bars_choices(self):
        data, _ = self.run_viewer("fits-open", overflow=False, space=False, open_tray=True,
                                  clicks=[{"key": "Tab"}, {"key": "Tab"}])
        steps = json.loads((self.directory / "fits-open-steps.json").read_text(encoding="utf-8"))
        self.assertEqual(len(steps), 2, steps)
        self.assertEqual(steps[0], {"handle": True, "panel": False}, "Tab closes the tray the handle opened")
        self.assertEqual(steps[1], {"handle": True, "panel": True}, "Tab opens it again with nothing omitted")
        tray = data["build_tray"]
        self.assertEqual(tray["omitted"], 0, tray)
        self.assertEqual(tray["handle_text"], "+", tray)
        self.assertTrue(tray["open"], tray)
        self.assertEqual((tray["handle_presses"], tray["key_presses"]), (1, 2), tray)
        bar = {card["type_id"] for card in data["command_bar"]["production_choices"]}
        self.assertTrue(bar, "the stock start has choices on the bar")
        self.assertEqual({entry["type_id"] for entry in tray["shown_entries"]}, bar,
                         "the tray lists the very choices the bar shows, grouped")

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
        self.assertEqual(tray["handle_text"], "+%d" % tray["omitted"], "the handle counts what the bar omits")
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
        """GBT-07: Tab closes and opens the tray like the handle; a planet tab whose choices fit the bar keeps the handle
        and the open tray (nothing omitted is no reason to hide them); a focused text field keeps the key."""
        ground, space = self.tab_point("b_planet_tab"), self.space_point()
        data, _ = self.run_viewer("open-state", overflow=True, space=True, open_tray=True, clicks=[
            {"key": "Tab"}, {"key": "Tab"}, ground, space, {"move": [640, 160]}, {"key": "F", "ctrl": True}, {"key": "Tab"}])
        steps = json.loads((self.directory / "open-state-steps.json").read_text(encoding="utf-8"))
        self.assertEqual(len(steps), 7, steps)
        self.assertEqual(steps[0], {"handle": True, "panel": False}, "Tab closes the open tray")
        self.assertEqual(steps[1], {"handle": True, "panel": True}, "Tab opens it again")
        self.assertEqual(steps[2], {"handle": True, "panel": True}, "the ground tab fits the bar: the handle and the open tray stay")
        self.assertEqual(steps[3], {"handle": True, "panel": True}, "back on the space tab the tray is still open")
        self.assertTrue(steps[5]["panel"] and steps[6]["panel"], "the planet search's text field keeps Tab")
        self.assertGreaterEqual(data["planet_search"]["opens"], 1, "Ctrl+F opened the planet search")
        tray = data["build_tray"]
        self.assertEqual((tray["handle_presses"], tray["key_presses"], tray["open"]), (1, 2, True), tray)

    def test_a_rebound_key_fires_and_is_ignored_with_modifiers_and_under_a_modal(self):
        """GBT-07: with the key rebound to B, Tab and Ctrl+B do nothing and B toggles, also on a tab whose choices fit
        the bar (the handle is there all the same); with the menu open (a modal) B does nothing."""
        ground, space = self.tab_point("b_planet_tab"), self.space_point()
        data, _ = self.run_viewer("rebound", overflow=True, space=True, open_tray=True, tray_key="B", clicks=[
            {"key": "Tab"}, {"key": "B", "ctrl": True}, {"key": "B"}, {"key": "B"}, ground, {"key": "B"}, space,
            {"key": "Escape"}, {"key": "B"}])
        steps = json.loads((self.directory / "rebound-steps.json").read_text(encoding="utf-8"))
        self.assertEqual(len(steps), 9, steps)
        self.assertTrue(steps[0]["panel"] and steps[1]["panel"], "Tab and Ctrl+B leave the tray as it is")
        self.assertEqual(steps[2], {"handle": True, "panel": False}, "the rebound key closes the tray")
        self.assertEqual(steps[3], {"handle": True, "panel": True}, "and opens it")
        self.assertEqual(steps[4], {"handle": True, "panel": True}, "the ground tab fits the bar: handle and tray stay")
        self.assertEqual(steps[5], {"handle": True, "panel": False}, "the key closes the tray there too")
        self.assertEqual(steps[6], {"handle": True, "panel": False}, "and the closed state is kept on the space tab")
        tray = data["build_tray"]
        self.assertEqual(tray["key_presses"], 3, "the three bare B presses with a handle and no modal count")

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

    def test_a_queued_strip_clears_the_always_shown_handle_on_a_fitting_planet(self):
        """GBT-02, GBT-14 (owner, 2026-11-19): on a planet whose choices all fit the bar the handle is shown all the same;
        a queue strip that would sit under it moves up to clear it, closed and open. The capture
        (fits-queued.png beside the report) is the eye check of that case."""
        opened, _ = self.run_viewer("fits-queue-open", overflow=False, space=False)
        cards = {control["name"]: control["hit_rect"] for control in opened["command_bar"]["controls"]}
        data, _ = self.run_viewer("fits-queued", overflow=False, space=False, open_tray=True, clicks=[
            centre(cards["b_Create_00"]), centre(cards["b_Create_01"]), {"key": "Tab"}])
        tray = data["build_tray"]
        self.assertEqual(tray["omitted"], 0, tray)
        self.assertTrue(tray["handle"] and not tray["open"], tray)
        queue = [control for control in data["command_bar"]["controls"]
                 if control["name"].startswith("queue") and control["visible"]]
        self.assertGreaterEqual(len(queue), 1, "something is queued and shown")
        for control in queue:
            a, b = control["rect"], tray["handle_rect"]
            self.assertFalse(a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and a[1] < b[1] + b[3] and b[1] < a[1] + a[3],
                             f"{control} covers the handle")

    def check_header_labels(self, name, tray):
        """#8031, #8159: every category name rides above the first icon of its own group (GBT-06): no label rect intersects
        any entry's frame, the first entry of the group at the label's left edge starts below the label's bottom, the label
        is nearer to that entry than to the icon row above it, and its strip is the line the label draws."""
        self.assertTrue(tray["open"], (name, tray))
        headers, entries = tray["header_rects"], [entry["rect"] for entry in tray["shown_entries"]]
        self.assertTrue(headers and entries, (name, "the tray shows category labels over entries", tray))
        # GBT-05: no two identical headers in one tray (a class group worded like a category is merged into it, #8160).
        texts = tray["header_texts"]
        # GBT-05 (#8361): the stock trays keep their fine rows, never the broad fallback.
        self.assertIs(tray["broad_rows"], False, (name, "the stock tray fell back to the broad rows", texts))
        self.assertEqual(len(texts), len(headers), (name, texts))
        self.assertEqual(len(texts), len({text.lower() for text in texts}), (name, "two labels read the same", texts))
        for header in headers:
            # A label has the height of its text (a HUD text's own minimum height is 0: #8159 laid every label out with a
            # zero-height strip, half over the icons below it).
            self.assertGreaterEqual(header[3], tray["header_text_pixels"] - 0.5,
                                    (name, "a label strip is shorter than the text it draws", header, tray["compact"]))
            # The strip is exactly the line the labels draw, at the full and at the compact size step alike: the compact
            # step used to shrink it to 0.8 of that line (the review of #8179), which the glyph bound above cannot see.
            self.assertGreaterEqual(tray["header_line_pixels"], tray["header_text_pixels"] - 0.5, (name, tray["header_line_pixels"]))
            self.assertAlmostEqual(header[3], tray["header_line_pixels"], delta=0.5,
                                   msg=(name, "a label strip is not the line the label draws", header, tray["compact"]))
            for entry in entries:
                apart = (header[0] + header[2] <= entry[0] + 0.5 or entry[0] + entry[2] <= header[0] + 0.5
                         or header[1] + header[3] <= entry[1] + 0.5 or entry[1] + entry[3] <= header[1] + 0.5)
                self.assertTrue(apart, (name, "a label sits on an icon frame", header, entry))
            own = [entry for entry in entries if abs(entry[0] - header[0]) <= 1.0 and entry[1] >= header[1] + header[3] - 0.5]
            self.assertTrue(own, (name, "no icon of the label's group starts below it", header))
            below = min(entry[1] for entry in own) - (header[1] + header[3])
            self.assertGreater(below, 0.5, (name, "the label touches its own icons: no lift", header))
            over = [entry for entry in entries if entry[1] + entry[3] <= header[1] + 0.5
                    and entry[0] < header[0] + header[2] and header[0] < entry[0] + entry[2]]
            if over:
                gap = header[1] - max(entry[1] + entry[3] for entry in over)
                self.assertGreaterEqual(gap, 2.0 * below - 0.5, (name, "the label hugs the row above, not its own icons", header))

    def test_header_labels_sit_above_their_row_and_never_on_an_icon_frame_on_the_space_and_the_ground_tray(self):
        # The extra window is the ground tab (the space tab's click point is read at 1280x720).
        trays = {}
        for name, mod, space, size in (("labels-space", self.mod, True, "1280x720"), ("labels-ground", self.ground_mod, False, "1280x720"),
                                       ("labels-ground-small", self.ground_mod, False, "1024x768")):
            data, _ = self.run_viewer(name, overflow=True, space=space, open_tray=True, mod=mod, size=size)
            trays[name] = data["build_tray"]
            self.assertFalse(trays[name]["compact"], (name, "the full size step fits the free height"))
            self.check_header_labels(name, trays[name])
        # The compact size step: the same space tray under a layer whose galaxy top limit leaves 90 percent of the full tray's
        # height, so the full step no longer fits and the packer takes the compact one (the close icon is 16 reference
        # units wide, which gives the reference scale).
        full = trays["labels-space"]
        ui = full["close"]["rect"][2] / 16.0
        panel = full["panel_rect"]
        limit = (panel[1] + panel[3] - 0.9 * panel[3]) / ui
        layer = self.directory / "revised-compact"
        shutil.copytree(self.layer, layer)
        path = layer / "Data/XML/RevisedBuildTray.xml"
        text = path.read_text(encoding="utf-8")
        self.assertIn("<Max_Rows>", text)
        top_limit = "<Galaxy_Top_Limit><Y>%.2f</Y></Galaxy_Top_Limit>" % limit
        path.write_text(text.replace("<Max_Rows>", top_limit + "<Max_Rows>", 1), encoding="utf-8")
        data, _ = self.run_viewer("labels-space-compact", overflow=True, space=True, open_tray=True, mod=self.mod, layer=layer)
        compact = data["build_tray"]
        self.assertTrue(compact["compact"], ("the compact size step was not taken", compact["panel_rect"], panel))
        self.check_header_labels("labels-space-compact", compact)

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
        # #7952: the panel ends at the planet info card, whose left edge the stock layout gives.
        card_left = tray["planet_info_left"]
        self.assertGreater(card_left, bar[0], "the stock bar has a planet info card right of the bar's start")
        self.assertLessEqual(panel[0] + panel[2], card_left + 0.5, "the tray never reaches into the planet info card")
        # Auto is the whole span to the card (at most 60 percent of the 1280 px view), whatever the bar's own width.
        span = min(card_left, bar[0] + 0.6 * 1280, 1280) - bar[0]
        self.assertAlmostEqual(panel[2], span, delta=0.5, msg=f"the tray fills the span to the planet info card: {panel} vs {span}")
        self.assertLessEqual(panel[0] + panel[2], 1280 + 0.5, "and never leaves the view")
        sample = tray["backdrop"]["back_sample"]
        self.assertTrue(bar[0] - 0.5 <= sample[0] < sample[1] <= bar[0] + bar[2] + 0.5,
                        f"the back samples only bar art inside the bar's own span (#7952): {sample} of {bar}")
        self.assertAlmostEqual(panel[0], bar[0], delta=0.5, msg="the panel starts at the bar's left edge")
        # GHUD-S74 (#7956): each bar card's icon draws at its own texture size times one factor (the component Scale at
        # this UI scale), centred on the card, aspect kept; never stretched to the card.
        factors = set()
        for choice in data["command_bar"]["production_choices"]:
            texture, icon = choice["icon_texture_size"], choice["icon_rect"]
            if not texture[0] or not texture[1]:
                continue
            fx, fy = icon[2] / texture[0], icon[3] / texture[1]
            self.assertAlmostEqual(fx, fy, delta=0.01, msg=f"aspect kept: {choice['icon']} {texture} drawn {icon}")
            factors.add(round(fx, 2))
            bar_card = next(control["rect"] for control in data["command_bar"]["controls"] if control["name"] == choice["button"])
            self.assertAlmostEqual(icon[0] + icon[2] / 2, bar_card[0] + bar_card[2] / 2, delta=0.6, msg=f"centred: {choice}")
            self.assertAlmostEqual(icon[1] + icon[3] / 2, bar_card[1] + bar_card[3] / 2, delta=0.6, msg=f"centred: {choice}")
        self.assertEqual(len(factors), 1, f"one size factor for every icon, so none is fitted to the card: {factors}")
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
