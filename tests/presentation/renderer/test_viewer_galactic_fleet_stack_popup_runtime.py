"""WGUI-166 / GHUDL-17: hovering a fleet stack icon opens the shared encyclopedia popup."""
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

try:
    from . import test_viewer_planet_hud_runtime as support
except ImportError:
    import test_viewer_planet_hud_runtime as support


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticFleetStackPopupRuntime(unittest.TestCase):
    def hover(self, fixture, badge, campaign=None, paused="yes", chain=None):
        """Hovers the named overview stack icon past the encyclopedia delay; returns the report. A campaign name runs
        that real campaign as it starts (its overview already shows the planet) instead of a synthetic fixture. A mod
        chain runs it on that stack; an empty badge name only looks (no hover) and captures after the start settles."""
        navigate = "true" if campaign is None else "false"
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-stack-popup-"))
        report = directory / "report.json"
        driver = directory / "input.gd"
        driver.write_text('''extends SceneTree
var frame = 0
var elapsed = 0.0
var hover_start = -1.0
var capture_path = ''' + json.dumps((directory / "native.png").as_posix()) + '''
func _initialize():
    call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func key(code):
    for pressed in [true, false]:
        var event = InputEventKey.new()
        event.keycode = code
        event.pressed = pressed
        Input.parse_input_event(event)
func _process(delta):
    elapsed += delta
    if elapsed < 1.0: return false
    frame += 1
    # GHUDL-16: the fixture's planet starts left of the overview frame; native
    # previous/next navigation centres it before the icon is pointed at.
    if frame == 10 and ''' + navigate + ''': key(KEY_PAGEUP)
    if frame == 12 and ''' + navigate + ''': key(KEY_PAGEDOWN)
    if frame >= 70 and frame <= 230 and frame % 20 == 10 and ''' + json.dumps(badge or "") + ''' != "":
        var target = root.find_child(''' + json.dumps(badge or "") + ''', true, false)
        assert(target != null and target.is_visible_in_tree())
        var motion = InputEventMouseMotion.new()
        motion.position = target.get_global_rect().get_center()
        motion.global_position = motion.position
        Input.parse_input_event(motion)
    if frame >= 70 and hover_start < 0.0: hover_start = elapsed
    var looking = ''' + json.dumps(not badge) + '''
    # The encyclopedia delay is wall-clock time, so the capture waits for it rather than for a frame count.
    if frame >= 330 and hover_start >= 0.0 and (looking or elapsed - hover_start > 3.0):
        root.get_texture().get_image().save_png(capture_path)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''', encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            *(("--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", fixture) if campaign is None
              else ("--eawr-galactic", campaign, "--eawr-galactic-dismiss-story", "yes")),
            *(("--eawr-mod-root", chain) if chain else ()),
            "--eawr-game-root", support.GAME_ROOT, "--eawr-galactic-paused", paused,
            "--eawr-report", str(report)], cwd=support.ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
        self.last_native = directory / "native.png"
        return data

    @staticmethod
    def hover_facts(data):
        popup = data["fleet_hud"]["stack_popup"]
        return {"key": popup["key"], "admitted": popup.get("admitted"), "pointer": popup.get("pointer"),
                "icons": [(row["icon"], row["rect"]) for row in data["fleet_hud"]["presence"] if row["planet"] == 101]}

    @staticmethod
    def presence(data, icon):
        return next(row for row in data["fleet_hud"]["presence"] if row["planet"] == 101 and row["icon"] == icon)

    def test_known_contents_list_each_type_with_its_count(self):
        # Mixed planet: the right icon merges the local slot 0 and 2 fleets.
        data = self.hover("fleet-contents", "fleet_presence_101_2")
        popup = data["fleet_hud"]["stack_popup"]
        self.assertNotEqual(popup["key"], 0, self.hover_facts(data))
        shown = popup["popup"]["tooltip"]
        self.assertIsNotNone(shown, self.hover_facts(data))
        self.assertTrue(shown["external"])
        self.assertGreater(shown["alpha"], 0)
        self.assertTrue(shown["text"])
        self.assertNotIn("TEXT_", shown["text"])  # the side's localized fleet name
        self.assertTrue(shown["icon_back"].lower().startswith("fleet_"))
        expected = [(row["type"], row["count"]) for row in self.presence(data, 2)["contents"]]
        self.assertEqual(expected, [(1000, 2), (1003, 1)])
        cells = shown["cells"]
        self.assertEqual([cell["count"] for cell in cells], [2, 1])
        self.assertTrue(all(cell["icon"] for cell in cells))
        self.assertLess(shown["population"], 0)  # usage, drawn in the usage colour

    def test_count_only_enemy_cells_name_no_type(self):
        data = self.hover("fleet-count", "fleet_presence_101_0")
        contents = self.presence(data, 0)["contents"]
        shown = data["fleet_hud"]["stack_popup"]["popup"]["tooltip"]
        self.assertIsNotNone(shown, self.hover_facts(data))
        cells = shown["cells"]
        self.assertEqual(len(cells), len(contents))
        for cell, row in zip(cells, contents):
            self.assertEqual(cell["count"], row["count"])
            self.assertEqual(bool(cell["icon"]), row["type"] is not None)
        self.assertTrue(any(row["type"] is None for row in contents))
        self.assertEqual(shown["population"], 0)  # no population from unknown types

    def test_large_mixed_fleets_wrap_inside_the_frame(self):
        # WGUI-166: debug-build captures at 5/10/20/40 types retain six
        # cells per row, three heroes in the grid and an x17 stack caption.
        previous_height = 0
        frame_width = None
        frame_bottom = None
        icon_extent = None
        for count in (5, 10, 20, 40):
            with self.subTest(types=count):
                data = self.hover(f"fleet-popup-{count}", "fleet_presence_101_2")
                shown = data["fleet_hud"]["stack_popup"]["popup"]["tooltip"]
                self.assertIsNotNone(shown, self.hover_facts(data))
                cells = shown["cells"]
                self.assertEqual(len(cells), count)
                self.assertEqual(sorted(cell["count"] for cell in cells), [1] * (count - 1) + [17])
                self.assertTrue(all(cell["icon"] for cell in cells))
                left, top, width, height = shown["rect"]
                # The stock atlas rectangle is 50 texels wide, and the
                # authored fleet-list scale is .75 in the 262-unit frame.
                self.assertAlmostEqual(cells[0]["rect"][2] / width, 50 * .75 / 262, places=4)
                if icon_extent is None:
                    icon_extent = cells[0]["rect"][2:]
                self.assertEqual(cells[0]["rect"][2:], icon_extent)
                if frame_width is None:
                    frame_width, frame_bottom = width, top + height
                self.assertAlmostEqual(width, frame_width, delta=1)
                self.assertAlmostEqual(top + height, frame_bottom, delta=1)
                self.assertGreater(height, previous_height)
                previous_height = height
                rows = {}
                for cell in cells:
                    x, y, w, h = cell["rect"]
                    self.assertGreater(w, 0)
                    self.assertGreater(h, 0)
                    self.assertGreaterEqual(x, left)
                    self.assertGreaterEqual(y, top)
                    self.assertLessEqual(x + w, left + width + 0.01)
                    self.assertLessEqual(y + h, top + height + 0.01)
                    rows.setdefault(round(y, 2), []).append((x, w))
                self.assertEqual([len(row) for _, row in sorted(rows.items())],
                                 [6] * (count // 6) + ([count % 6] if count % 6 else []))
                for row in rows.values():
                    for (x, w), (next_x, _) in zip(row, row[1:]):
                        self.assertLessEqual(x + w, next_x + 0.01)

    def test_centre_icon_opens_and_presence_only_icon_does_not(self):
        data = self.hover("fleet-contents", "fleet_presence_101_1")
        self.assertIsNotNone(data["fleet_hud"]["stack_popup"]["popup"]["tooltip"], self.hover_facts(data))
        # GHUD-S76: stack icons need a filtered fleet card, so a presence-only enemy draws no stack at all and there is
        # nothing to hover or open. The plain fixture has no attached superweapon, which would grant counts (WGUI-17).
        data = self.hover("fleet-presence-plain", "fleet_presence_101_1")
        self.assertFalse([row for row in data["fleet_hud"]["presence"] if row["planet"] == 101 and row["icon"] == 0],
                         self.hover_facts(data))

    def test_real_campaign_hero_medal_and_stack_icons_open_the_popup(self):
        # Legacy #7177: the stock Imperial start has fleets and landed heroes at Coruscant. Every stack icon of the
        # planet, the fleet icons and the ground one that carries the heroes, opens the popup when hovered.
        for paused, badge in (("yes", "fleet_presence_15_0"), ("yes", "ground_presence_15"),
                              ("no", "fleet_presence_15_0"), ("no", "ground_presence_15")):
            with self.subTest(badge=badge, paused=paused):
                data = self.hover(None, badge, campaign="Sandbox_Equal_Footing_Empire", paused=paused)
                popup = data["fleet_hud"]["stack_popup"]
                shown = popup["popup"]["tooltip"]
                self.assertIsNotNone(shown, {"key": popup["key"], "admitted": popup.get("admitted"),
                                             "pointer": popup.get("pointer")})
                self.assertTrue(shown["cells"], shown)

    def remake_chain(self):
        # The Remake stack under test (leaf first): French Fried Taters > Clone Wars Holdouts > Remake 4.0.
        remake = os.environ.get("EAWR_REMAKE_MOD_ROOT")
        if not remake:
            self.skipTest("requires the Remake mod root")
        chain = [pathlib.Path(remake).parent / "3229239424", pathlib.Path(remake).parent / "2794270450", pathlib.Path(remake)]
        if not all(path.is_dir() for path in chain):
            self.skipTest("requires Remake 4.0, Holdouts and French Fried Taters")
        return ";".join(str(path) for path in chain)

    def test_remake_extra_faction_fleet_is_named_by_its_faction_never_as_the_enemy(self):
        # #8396: the Republic seat of the Remake stack owns a fleet of a faction that is none of the three sides. Its popup
        # header names the faction (a project format over the faction's display name); no stack icon of any faction reads
        # "enemy". The still is kept as remake-republic-own-fleet-popup.png beside the run.
        chain = self.remake_chain()
        campaign = "GC_Final_Destination_Republic"
        looking = self.hover(None, None, campaign=campaign, chain=chain)
        local = looking["local"]["player"]
        # The overview draws every planet's icons; only those inside the 1280x720 view can be pointed at.
        rows = [row for row in looking["fleet_hud"]["presence"] if row["visible"] and not row["ground"] and row["contents"]
                and 0 <= row["rect"][0] and row["rect"][0] + row["rect"][2] <= 1280
                and 0 <= row["rect"][1] and row["rect"][1] + row["rect"][3] <= 720]
        own = [row for row in rows if row["owner"] == local]
        self.assertTrue(own, {"local": local, "presence": [(row["planet"], row["owner"], row["icon"]) for row in rows]})
        data = self.hover(None, own[0]["name"], campaign=campaign, chain=chain)
        shown = data["fleet_hud"]["stack_popup"]["popup"]["tooltip"]
        self.assertIsNotNone(shown, self.hover_facts(data))
        print("REMAKE OWN FLEET POPUP TITLE", repr(shown["text"]), flush=True)
        shutil.copyfile(self.last_native, self.last_native.with_name("remake-republic-own-fleet-popup.png"))
        self.assertTrue(shown["text"].endswith(" Fleet"), shown["text"])
        self.assertNotIn("enemy", shown["text"].lower())
        self.assertNotIn("TEXT_", shown["text"])
        self.assertTrue(shown["text"].lower().startswith("republic"), shown["text"])

    def test_hero_status_icon_hover_lists_the_planets_heroes(self):
        # Legacy #7177 / WGSI-8/9: hovering the hero medal under a planet's name lists its named heroes as icons. The stock
        # Imperial start has Veers, Tarkin and the Emperor landed on Coruscant (the Empire side icon, kind 9, planet 15).
        for paused in ("yes", "no"):
            with self.subTest(paused=paused):
                data = self.hover(None, "planet_status_15_9", campaign="Sandbox_Equal_Footing_Empire", paused=paused)
                tip = data["hero_tooltip"]
                self.assertTrue(tip["visible"], tip)
                self.assertEqual((tip["planet"], tip["kind"]), (15, 9), tip)
                types = sorted(cell["type"] for cell in tip["cells"])
                for hero in (647, 690, 457):
                    self.assertIn(hero, types, tip)
                for cell in tip["cells"]:
                    x, y, width, height = cell["rect"]
                    self.assertGreater(width, 0, cell)
                    self.assertGreater(height, 0, cell)
                    self.assertGreaterEqual(x, 0, cell)
                    self.assertGreaterEqual(y, 0, cell)
                    self.assertLessEqual(x + width, 1280, cell)
                    self.assertLessEqual(y + height, 720, cell)


if __name__ == "__main__":
    unittest.main()
