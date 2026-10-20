"""GHUD-S01: faction shell pixels and published clock / domain states."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticCommandBarRuntime(unittest.TestCase):
    def capture(self, seat, fixture=None, campaign="synthetic"):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-command-bar-"))
        report, capture = directory / "report.json", directory / "capture.png"
        args = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-galactic", campaign,
                "--eawr-game-root", GAME_ROOT, "--eawr-galactic-seat", str(seat),
                "--eawr-galactic-paused", "yes", "--eawr-report", str(report),
                "--eawr-capture", str(capture)]
        if fixture:
            args += ["--eawr-galactic-hud-fixture", fixture]
        result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
        self.assertEqual(data["status"], "captured")
        self.assertTrue(data["paused"])
        # TM-05: pausing first resets fast forward to 1x, then submits the pause.
        # These are two clock commands, with no production purchase in setup (#4167).
        self.assertEqual(data["live_session"]["submitted_orders"], 2 if fixture else 1)
        if fixture:
            self.assertEqual(data["live_session"]["clock_orders"], [
                {"type": "SetSpeed", "issuer": seat, "session_tick": 0},
                {"type": "PauseGalaxy", "issuer": seat, "session_tick": 0},
            ])
        self.assertEqual(data["live_session"]["pause_depth"], 1)
        self.assertEqual(data["live_session"]["galactic_frame"], 0)
        return data, capture

    def test_each_faction_shell_and_accessible_domain(self):
        from space_map_test_support import decode_png
        pixels = []
        for seat, faction in [(1, "rebel"), (3, "empire"), (4, "underworld")]:
            with self.subTest(faction=faction):
                data, capture = self.capture(seat, "accessible")
                bar = data["command_bar"]
                self.assertEqual(bar["faction"], faction)
                self.assertGreater(bar["meshes"], 0)
                self.assertTrue(bar["ground_enabled"])
                self.assertEqual(bar["technology_visible"], faction != "underworld")
                self.assertEqual(bar["pips"], 10)
                self.assertEqual(bar["cycle"], "Day 1")
                self.assertEqual(bar["account"], "12422")
                self.assertEqual(bar["current_pip"], 0)
                controls = {control["name"]: control for control in bar["controls"]}
                self.assertTrue(controls["b_planet_tab"]["enabled"])
                self.assertTrue(controls["b_play_pause"]["enabled"])
                # TM-03/04: fast forward is unavailable while the galaxy is paused.
                self.assertFalse(controls["b_fast_forward"]["enabled"])
                width, height, rows = decode_png(capture.read_bytes())
                # Shell qualification samples its lower-left area, away from the galaxy.
                sample = [p for row in rows[height - 130:height - 20] for p in row[:width // 3]]
                self.assertGreater(sum(max(p[:3]) > 70 for p in sample), 1000)
                pixels.append(sample)
        self.assertNotEqual(pixels[0], pixels[1])
        self.assertNotEqual(pixels[1], pixels[2])

    def test_inaccessible_selection_forces_space(self):
        data, _ = self.capture(1, "inaccessible")
        bar = data["command_bar"]
        self.assertFalse(bar["ground_enabled"])
        self.assertTrue(bar["space_tab"])
        self.assertFalse(next(c for c in bar["controls"] if c["name"] == "b_planet_tab")["enabled"])
        self.assertEqual(data["hud"]["selected_income"], "")
        # #2883: space active draws only the ground tab's art (its composite shows the space tab selected).
        self.assertEqual(self.tab_visibility(bar), {"b_space_tab": False, "b_planet_tab": True})

    @staticmethod
    def tab_visibility(bar):
        return {c["name"]: c["visible"] for c in bar["controls"] if c["name"] in ("b_space_tab", "b_planet_tab")}

    def test_ground_tab_active_draws_only_the_space_tab_art(self):
        # #2883: the selected tab is hidden so the unselected tab's composite art shows the selection once.
        data, _ = self.capture(1, "accessible")
        bar = data["command_bar"]
        self.assertFalse(bar["space_tab"])
        self.assertEqual(self.tab_visibility(bar), {"b_space_tab": True, "b_planet_tab": False})

    def test_selecting_space_tab_swaps_the_visible_tab_art(self):
        initial, _ = self.capture(1, "accessible")
        controls = {c["name"]: c for c in initial["command_bar"]["controls"]}
        # The ground tab is hidden while the ground list is active; press the visible space tab.
        point = [controls["b_space_tab"]["hit_rect"][0] + controls["b_space_tab"]["hit_rect"][2] / 2,
                 controls["b_space_tab"]["hit_rect"][1] + controls["b_space_tab"]["hit_rect"][3] / 2]
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-tab-input-"))
        report, driver = directory / "report.json", directory / "input.gd"
        driver.write_text('''extends SceneTree
var frame = 0
func _initialize():
    call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func click(down):
    var position = Vector2(''' + repr(point[0]) + ", " + repr(point[1]) + ''')
    var motion = InputEventMouseMotion.new()
    motion.position = position
    motion.global_position = position
    Input.parse_input_event(motion)
    var event = InputEventMouseButton.new()
    event.button_index = MOUSE_BUTTON_LEFT
    event.pressed = down
    event.position = position
    event.global_position = position
    Input.parse_input_event(event)
func _process(_delta):
    frame += 1
    if frame == 20: click(true)
    if frame == 21: click(false)
    if frame == 60:
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''', encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-seat", "1",
            "--eawr-galactic-hud-fixture", "accessible", "--eawr-game-root", GAME_ROOT,
            "--eawr-galactic-paused", "yes", "--eawr-report", str(report)],
            cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
        bar = data["command_bar"]
        self.assertTrue(bar["space_tab"])
        self.assertTrue(bar["ground_enabled"])
        self.assertEqual(self.tab_visibility(bar), {"b_space_tab": False, "b_planet_tab": True})
        planet = next(c for c in bar["controls"] if c["name"] == "b_planet_tab")
        self.assertTrue(planet["enabled"])

    def test_multiplayer_preview_disables_clock_controls(self):
        data, _ = self.capture(1)
        self.assertEqual(data["command_bar"]["cycle"], "")
        self.assertIsNone(data["command_bar"]["current_pip"])
        controls = {control["name"]: control for control in data["command_bar"]["controls"]}
        self.assertFalse(controls["b_play_pause"]["enabled"])
        self.assertFalse(controls["b_fast_forward"]["enabled"])

    def test_campaign_fiscal_provider_publishes_sourced_date_and_pip(self):
        data, _ = self.capture(1, campaign="Sandbox_Equal_Footing_Underworld")
        clock, bar = data["live_session"], data["command_bar"]
        self.assertGreater(clock["fiscal_end"], clock["fiscal_start"])
        # WGEP-59: live campaign setup initializes the first fiscal window.
        self.assertEqual(clock["fiscal_cycle"], 1)
        self.assertEqual(bar["cycle"], "Day 1")
        self.assertEqual(bar["current_pip"], 0)

    def test_double_mouse_key_toggle_submits_one_pause_and_speed_commands(self):
        initial, _ = self.capture(1, "accessible")
        hit_points = {control["name"]: [control["hit_rect"][0] + control["hit_rect"][2] / 2,
            control["hit_rect"][1] + control["hit_rect"][3] / 2]
            for control in initial["command_bar"]["controls"]}
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-command-input-"))
        report = directory / "report.json"
        driver = directory / "input.gd"
        driver.write_text('''extends SceneTree
var frame = 0
var hit_points = JSON.parse_string(''' + repr(json.dumps(hit_points)) + ''')
func _initialize():
    call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
    for name in ["b_space_tab", "b_planet_tab", "b_play_pause", "b_fast_forward"]:
        var target = root.find_child(name, true, false)
        target.pressed.connect(func(): print("HUD_PRESS ", name))
func click(name, down):
    var target = root.find_child(name, true, false)
    assert(target != null)
    var point = hit_points[name]
    var position = Vector2(point[0], point[1])
    var motion = InputEventMouseMotion.new()
    motion.position = position
    motion.global_position = position
    Input.parse_input_event(motion)
    var event = InputEventMouseButton.new()
    event.button_index = MOUSE_BUTTON_LEFT
    event.pressed = down
    event.position = position
    event.global_position = position
    Input.parse_input_event(event)
func space_key():
    for down in [true, false]:
        var event = InputEventKey.new()
        event.keycode = KEY_SPACE
        event.pressed = down
        Input.parse_input_event(event)
func _process(_delta):
    frame += 1
    if frame == 20: click("b_space_tab", true)
    if frame == 21: click("b_space_tab", false)
    if frame == 25: click("b_planet_tab", true)
    if frame == 26: click("b_planet_tab", false)
    if frame == 30: click("b_play_pause", true)
    if frame == 31:
        click("b_play_pause", false)
        space_key()
    if frame == 50: click("b_fast_forward", true)
    if frame == 51: click("b_fast_forward", false)
    if frame == 70: click("b_play_pause", true)
    if frame == 71:
        click("b_play_pause", false)
        space_key()
    if frame == 120:
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''', encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-seat", "1",
            "--eawr-galactic-hud-fixture", "accessible", "--eawr-game-root", GAME_ROOT,
            "--eawr-galactic-paused", "yes", "--eawr-report", str(report)],
            cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
        for name in ["b_space_tab", "b_planet_tab", "b_play_pause", "b_fast_forward"]:
            self.assertIn("HUD_PRESS " + name, result.stdout + result.stderr)
        self.assertEqual(data["live_session"]["submitted_orders"], 4)
        self.assertEqual(data["live_session"]["pause_depth"], 1)
        self.assertEqual(data["live_session"]["strategic_speed_raw"], 2 * (1 << 24))
        self.assertFalse(data["command_bar"]["space_tab"])
        self.assertEqual(data["map_input"]["selected"], initial["map_input"]["selected"])
        self.assertEqual(data["map_input"]["selection_invalidations"], 0)


    def live_tick_rate(self, hit_points, fast, ai):
        """#4031: run the live galaxy unpaused for a few seconds (F3 overlay on) and return the report."""
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-tick-rate-"))
        report, driver = directory / "report.json", directory / "rate.gd"
        point = hit_points["b_fast_forward"]
        driver.write_text('''extends SceneTree
var frame = 0
var started = 0
func _initialize():
    call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func click(down):
    var position = Vector2(''' + repr(point[0]) + ", " + repr(point[1]) + ''')
    var motion = InputEventMouseMotion.new()
    motion.position = position
    motion.global_position = position
    Input.parse_input_event(motion)
    var event = InputEventMouseButton.new()
    event.button_index = MOUSE_BUTTON_LEFT
    event.pressed = down
    event.position = position
    event.global_position = position
    Input.parse_input_event(event)
func f3():
    for down in [true, false]:
        var event = InputEventKey.new()
        event.keycode = KEY_F3
        event.pressed = down
        Input.parse_input_event(event)
func _process(_delta):
    frame += 1
    if frame == 10: f3()
    if frame == 30 and ''' + ("true" if fast else "false") + ''': click(true)
    if frame == 31 and ''' + ("true" if fast else "false") + ''': click(false)
    if frame == 31: started = Time.get_ticks_msec()
    if started > 0 and Time.get_ticks_msec() - started > 5000:
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''', encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "Sandbox_Equal_Footing_Underworld", "--eawr-galactic-seat", "1",
            "--eawr-galactic-ai", "on" if ai else "off", "--eawr-game-root", GAME_ROOT,
            "--eawr-report", str(report)], cwd=ROOT, capture_output=True, text=True, timeout=600, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
        return data

    def test_fast_forward_applies_its_factor_to_the_tick_rate(self):
        # #4031: the applied factor in a real live run, observed through the F3 overlay's TICKS/S.
        initial, _ = self.capture(1, campaign="Sandbox_Equal_Footing_Underworld")
        controls = {control["name"]: control for control in initial["command_bar"]["controls"]}
        # The capture is paused, where the button is disabled; only its place is used here.
        hit_points = {name: [control["hit_rect"][0] + control["hit_rect"][2] / 2,
            control["hit_rect"][1] + control["hit_rect"][3] / 2] for name, control in controls.items()}
        for ai in (False, True):
            with self.subTest(ai=ai):
                normal = self.live_tick_rate(hit_points, False, ai)["perf_overlay"]
                fast_report = self.live_tick_rate(hit_points, True, ai)
                fast = fast_report["perf_overlay"]
                self.assertTrue(normal["shown"] and fast["shown"])
                self.assertEqual(normal["tick_source"], "live_session")
                self.assertEqual(normal["clock"]["speed_factor"], 1.0)
                self.assertEqual(fast["clock"]["speed_factor"], 4.0)
                self.assertEqual(fast_report["live_session"]["strategic_speed_raw"], 4 * (1 << 24))
                print("tick rate ai=%s: 1x %.1f/s (tick %.2f ms), fast forward %.1f/s (tick %.2f ms)" % (
                    ai, normal["ticks_per_second"], normal["tick_ms_avg"], fast["ticks_per_second"], fast["tick_ms_avg"]))
                # Observation: the rates above are numbers for the PR, not a gate (loaded hosts). The gate is the
                # injected-clock schedule test in galactic_live_game_session_tests.
                self.assertGreater(fast["ticks"], 0)


if __name__ == "__main__":
    unittest.main()
