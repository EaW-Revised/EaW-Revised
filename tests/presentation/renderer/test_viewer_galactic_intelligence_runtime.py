"""GHUD-S02: a populated secret payload never bypasses the local observation."""
import math
import os
import pathlib
import subprocess
import tempfile
import unittest
if __package__:
    from . import test_viewer_planet_hud_runtime as support
else:
    import test_viewer_planet_hud_runtime as support


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticIntelligenceRuntime(unittest.TestCase):
    def capture(self, fixture):
        return support.PlanetHudRuntime.capture(self, "synthetic",
            "--eawr-galactic-hud-fixture", fixture)

    def test_masked_known_and_known_zero_use_the_same_admission_path(self):
        captures = []
        for fixture in ["masked", "known", "known-zero", "discovered-only"]:
            with self.subTest(fixture=fixture):
                data, capture = self.capture(fixture)
                captures.append(capture.read_bytes())
                hud = data["hud"]
                self.assertTrue(hud["base_levels_visible"])
                facts = {row["planet"]: row for row in hud["intelligence"]}
                self.assertFalse(facts[103]["discovered"])
                self.assertEqual(facts[103]["name"], "")
                self.assertIsNone(facts[103]["income"])
                self.assertNotIn(103, {label["planet"] for label in hud["labels"]})
                for row in facts.values():
                    if not row["discovered"] or fixture in ["masked", "discovered-only"]:
                        for field in ["owner", "ground", "space", "structures", "control"]:
                            self.assertIsNone(row[field], (fixture, row))
                        if not row["discovered"] or fixture == "masked":
                            self.assertNotIn("Rebel", row["hover"])
                    else:
                        self.assertEqual(row["owner"], 1)
                        self.assertEqual(row["ground"], 0 if fixture == "known-zero" else 4)
                        self.assertEqual(row["space"], 0 if fixture == "known-zero" else 2)
                    for field in ["apparent_owner", "apparent_allegiance_owner"]:
                        self.assertEqual(row[field], 1 if row["discovered"] and fixture != "masked" else None)
                labels = {row["planet"]: row for row in hud["labels"]}
                if fixture in ["masked", "discovered-only"]:
                    self.assertEqual(facts[101]["income"], 75)
                    self.assertIn("Income: 75", facts[101]["hover"])
                    self.assertEqual(hud["selected_income"], "$ 75")
                    self.assertIsNone(facts[101]["ground"])
                    self.assertIsNone(facts[101]["structures"])
                    self.assertEqual(labels[101]["colour"], [100, 100, 100])
                    self.assertIn("Ground level: ?", facts[101]["hover"])
                    self.assertIn("Space level: ?", facts[101]["hover"])
                    if fixture == "discovered-only":
                        self.assertIn("Rebel", facts[101]["hover"])
                elif fixture == "known-zero":
                    self.assertEqual(facts[101]["income"], 0)
                    self.assertIn("Ground level: 0", facts[101]["hover"])
                else:
                    self.assertEqual(facts[101]["income"], 190)
                    self.assertEqual(labels[101]["colour"], [185, 40, 39])
                    self.assertIn("Political control: 3", facts[101]["hover"])
        self.assertEqual(len(set(captures[:3])), 3, "masked, known, and known zero must draw distinct states")

    def assert_name_pixels(self, capture, rows, expected_minimum):
        # The glyph pixels of enough names carry the reported colour: the
        # frame, not only the report, shows the tint (some names may sit under
        # HUD art or off screen and are skipped).
        from tests.presentation.renderer.space_map_test_support import decode_png
        width, height, image = decode_png(capture.read_bytes())
        tested = 0
        for row in rows:
            x, y, w, h = row["rect"]
            if x < 0 or y < 0 or x + w >= width or y + h >= height:
                continue
            rgb = row["colour"]
            pixels = [pixel for line in image[int(y):int(y + h + 1)] for pixel in line[int(x):int(x + w + 1)]]
            if sum(all(abs(pixel[channel] - rgb[channel]) <= 16 for channel in range(3)) for pixel in pixels) >= 6:
                tested += 1
        self.assertGreaterEqual(tested, expected_minimum, "coloured name glyphs in the captured frame")

    def named_campaign(self, campaign):
        data, capture = support.PlanetHudRuntime.capture(self, campaign)
        hud = data["hud"]
        self.assertEqual(data["flights"][0]["state"], "overview", "zoomed-out frame")
        facts = {row["planet"]: row for row in hud["intelligence"]}
        labels = {row["planet"]: row for row in hud["labels"] if row["visible"]}
        planets = {row["id"]: row for row in data["planets"]}
        blips = {row["planet"]: row for row in data["radar_hud"]["blips"] if row["radar"] == 0}
        return data, capture, facts, labels, planets, blips

    def test_seat_with_ownership_intelligence_colours_distant_enemy_planets(self):
        # WGUI-06/38/96: the Underworld seat starts with ownership and fleet
        # presence on every planet (debug build), so a discovered enemy planet
        # far from the camera and from any own fleet draws in its owner's
        # faction colour: name, radar blip and nothing camera-dependent.
        data, capture, facts, labels, planets, blips = self.named_campaign("Sandbox_Equal_Footing_Underworld")
        local = data["local"]["player"]
        selected = labels[data["hud"]["selected"]]["anchor"]
        far_enemies = []
        for planet, label in labels.items():
            fact = facts[planet]
            owner = planets[planet].get("owner")
            if owner is None or owner == local:
                continue
            self.assertEqual(fact["owner"], owner, (planet, fact))
            self.assertEqual(label["colour"], planets[planet]["colour"], label)
            self.assertEqual(blips[planet]["colour"], label["colour"], blips[planet])
            self.assertEqual(blips[planet]["alpha"], 255)
            if math.dist(label["anchor"], selected) > 300:
                far_enemies.append(planet)
        self.assertGreaterEqual(len(far_enemies), 2, "need distant enemy planets in the frame")
        self.assertGreaterEqual(len({tuple(labels[planet]["colour"]) for planet in far_enemies}), 2,
                                "both enemy factions keep their own colour")
        neutral = next(blip["colour"] for blip in blips.values() if blip["alpha"] == 128)
        self.assertTrue(all(labels[planet]["colour"] != neutral for planet in far_enemies))
        self.assert_name_pixels(capture, [labels[planet] for planet in far_enemies], 2)

    def test_seat_without_ownership_intelligence_keeps_unobserved_enemy_planets_neutral(self):
        # WGUI-06/12/38: the Empire's default intelligence is empty (debug
        # build), so a discovered enemy planet with no own fleet, visible base
        # or spy stays Neutral on the name and the radar blip while own
        # planets keep the faction colour. This is retail, not a gating bug.
        data, capture, facts, labels, planets, blips = self.named_campaign("Sandbox_Equal_Footing_Empire")
        local = data["local"]["player"]
        neutral = next(blip["colour"] for blip in blips.values() if blip["alpha"] == 128)
        unknown, own = [], []
        for planet, label in labels.items():
            owner = planets[planet].get("owner")
            if owner == local:
                own.append(planet)
                self.assertEqual(facts[planet]["owner"], local)
                self.assertEqual(label["colour"], planets[planet]["colour"], label)
            elif owner is not None and facts[planet]["owner"] is not None:
                self.assertEqual(label["colour"], planets[planet]["colour"], "known through an own presence")
            elif owner is not None:
                unknown.append(planet)
                self.assertEqual(label["colour"], neutral, label)
                self.assertEqual(blips[planet]["colour"], neutral, blips[planet])
                self.assertEqual(blips[planet]["alpha"], 255, "discovered, so opaque")
                self.assertIsNone(facts[planet]["apparent_owner"])
        self.assertTrue(own)
        self.assertGreaterEqual(len(unknown), 2, "discovered enemy planets without ownership knowledge")
        self.assertTrue(all(planets[planet]["colour"] != neutral for planet in own))
        self.assert_name_pixels(capture, [labels[planet] for planet in unknown], 2)
        # WGUI-99/100: a trade line shows only between discovered planets and
        # takes no type from an owner the seat cannot see.
        hidden = 0
        for route in data["routes"]:
            ends = [facts[route["from"]], facts[route["to"]]]
            self.assertEqual(route["discovered"], all(fact["discovered"] for fact in ends), route)
            hidden += not route["discovered"]
            if any(fact["apparent_owner"] is None for fact in ends):
                self.assertEqual(route["line_type"], "neutral", route)
        self.assertGreater(hidden, 0, "the Empire start has trade lines to undiscovered planets")

    def test_native_hover_survives_refresh_and_drops_when_camera_hides_label(self):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-hover-lifetime-"))
        driver, capture = directory / "hover.gd", directory / "hover.png"
        driver.write_text('''extends SceneTree
func fail(message):
    push_error(message)
    quit(1)
func _initialize():
    call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
    for index in range(20):
        await process_frame
    var target = root.find_child("planet_name_102", true, false)
    if target == null or not target.is_visible_in_tree():
        fail("discovered label is not visible")
        return
    if not target.tooltip_text.contains("Rebel"):
        fail("apparent faction is missing from tooltip")
        return
    var point = target.get_global_rect().get_center()
    var motion = InputEventMouseMotion.new()
    motion.position = point
    motion.global_position = point
    root.push_input(motion, true)
    if root.gui_get_hovered_control() != target:
        fail("native GUI did not admit the initial hover")
        return
    await create_timer(1.2).timeout
    if root.gui_get_hovered_control() != target or not target.is_visible_in_tree():
        fail("unchanged label lost hover across HUD refreshes")
        return
    var tooltip_visible = false
    for label in root.find_children("*", "Label", true, false):
        if label != target and label.text == target.tooltip_text and label.is_visible_in_tree():
            tooltip_visible = true
    if not tooltip_visible:
        fail("native tooltip did not appear after its delay")
        return
    print("HOVER_PERSISTED_WITH_TOOLTIP")
    await RenderingServer.frame_post_draw
    var args = OS.get_cmdline_user_args()
    var path = args[args.find("--eawr-hover-capture") + 1]
    if root.get_texture().get_image().save_png(path) != OK:
        fail("hover screenshot failed")
        return
    # A real world zoom fades/hides overview labels (WGUI-96), without moving
    # the pointer off the hovered label or directly changing its visibility.
    var wheel = InputEventMouseButton.new()
    wheel.position = point
    wheel.global_position = point
    wheel.button_index = MOUSE_BUTTON_WHEEL_UP
    wheel.pressed = true
    root.push_input(wheel, true)
    await create_timer(1.5).timeout
    if target.is_visible_in_tree() or root.gui_get_hovered_control() == target:
        fail("a camera-hidden label retained visibility or hover")
        return
    print("HIDDEN_LABEL_DROPPED_HOVER")
    current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
    quit()
''', encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-game-root", support.GAME_ROOT,
            "--eawr-galactic-paused", "yes", "--eawr-galactic-hud-fixture", "discovered-only",
            "--eawr-hover-capture", str(capture)], cwd=support.ROOT,
            capture_output=True, text=True, timeout=300, check=False)
        log = result.stdout + result.stderr
        (directory / "viewer.log").write_text(log, encoding="utf-8")
        self.assertEqual(result.returncode, 0, log)
        self.assertIn("HOVER_PERSISTED_WITH_TOOLTIP", log)
        self.assertIn("HIDDEN_LABEL_DROPPED_HOVER", log)
        self.assertTrue(capture.is_file(), log)


if __name__ == "__main__":
    unittest.main()
