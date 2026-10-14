"""WGP-23/24 local-faction music through the installed XML and viewer adapter."""
import json
import subprocess
import os
import pathlib
import tempfile
import unittest

from tests.galactic.test_galactic_viewer_mode import run_viewer


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST")
                     and os.environ.get("EAWR_EAW_GAME_ROOT"), "requires viewer GPU host and game data")
class GalacticAudioGpu(unittest.TestCase):
    def test_qualified_construction_unlock_is_one_sfx_per_research(self):
        from tests.presentation.renderer import test_viewer_galactic_production_runtime as support
        for fixture, count in (("production", 0), ("production-unlocks", 1)):
            with self.subTest(fixture=fixture):
                harness = support.GalacticProductionRuntime()
                initial, _ = harness.capture(3, fixture)
                research = next(row["button"] for row in initial["command_bar"]["production_choices"]
                                if row["type_id"] == 1002)
                data = harness.native_run([research, "b_play_pause", "b_play_pause"],
                                          fixture=fixture, action_seconds=12.0)
                self.assertEqual(data["production_queues"], [])
                audio = data["galactic_audio"]
                # The local fixture seat resolves stock Empire faction fields.
                self.assertEqual(audio["sounds"].get("EHD_New_Construction", 0), count, audio)
                self.assertEqual(audio["played_sounds"].get("EHD_New_Construction", 0), count, audio)

    def live_input(self, planet, destination=None, queue=False):
        from tests.presentation.renderer.test_viewer_planet_hud_runtime import ROOT
        directory = pathlib.Path(tempfile.mkdtemp(prefix="galactic-live-audio-"))
        driver, report = directory / "input.gd", directory / "report.json"
        driver.write_text('''extends SceneTree
var elapsed = 0.0
var frame = 0
var viewport: SubViewport
var source: Vector2
var target: Vector2
func _initialize():
    viewport = SubViewport.new()
    viewport.size = Vector2i(1280,720)
    viewport.handle_input_locally = true
    viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
    root.add_child(viewport)
    viewport.add_child(load("res://main.tscn").instantiate())
    viewport.notify_mouse_entered()
    current_scene = viewport
func first_queue(node):
    if str(node.name).begins_with("queue") and node is BaseButton and node.is_visible_in_tree() and not node.disabled:
        return node
    for child in node.get_children():
        var found = first_queue(child)
        if found != null: return found
    return null
func motion(point, held):
    var event = InputEventMouseMotion.new()
    event.position = point
    event.global_position = point
    event.relative = Vector2(24,0)
    event.button_mask = MOUSE_BUTTON_MASK_LEFT if held else 0
    viewport.push_input(event,true)
func mouse(point, down):
    var event = InputEventMouseButton.new()
    event.button_index = %s
    event.position = point
    event.global_position = point
    event.pressed = down
    viewport.push_input(event,true)
func _process(delta):
    elapsed += delta
    if elapsed < 2.0: return false
    frame += 1
    if frame == 1:
        var button = %s
        assert(button != null and button.is_visible_in_tree() and not button.disabled)
        source = button.get_global_rect().get_center()
        target = source
        %s
        motion(source,false)
        mouse(source,true)
    if frame == 3 and %s: motion(source + Vector2(24,0),true)
    if frame == 5 and %s: motion(target,true)
    if frame == 8: mouse(target,false)
    if frame == 120:
        viewport.get_texture().get_image().save_png(%s)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % ("MOUSE_BUTTON_RIGHT" if queue else "MOUSE_BUTTON_LEFT",
       "first_queue(root)" if queue else 'root.find_child("q1_0001",true,false)',
       "pass" if queue else 'var radar = root.find_child("radar_galactic",true,false)\n        target = radar.get_global_position() + Vector2(radar.size.x * (0.5 + %s / 760.0),radar.size.y * (0.5 - %s / 920.0))' % tuple(destination[:2]),
       "false" if queue else "true", "false" if queue else "true",
       json.dumps((directory / "capture.png").as_posix())), encoding="utf-8")
        args = ["--eawr-galactic", "Sandbox_Equal_Footing_Underworld", "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                "--eawr-galactic-focus", str(planet), "--eawr-galactic-flight-ms", "750", "--eawr-report", str(report)]
        if queue:
            args += ["--eawr-galactic-queue", "Underworld_Merc_Squad", "--eawr-galactic-queue-planet", str(planet),
                     "--eawr-galactic-paused", "yes"]
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720", "--path",
            str(ROOT / "apps/viewer/project"), "--script", str(driver), "--", *args], cwd=ROOT,
            capture_output=True, text=True, timeout=300)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads(report.read_text(encoding="utf-8"))

    def test_live_campaign_move_and_cancel_request_feedback(self):
        from tests.presentation.renderer import test_viewer_planet_hud_runtime as support
        initial, _ = support.PlanetHudRuntime.capture(self, "Sandbox_Equal_Footing_Underworld")
        own = {row["id"]: row for row in initial["planets"] if row["owner"] == initial["local"]["player"]}
        source = initial["map_input"]["selected"]
        self.assertIn(source, own)
        self.assertTrue(any(row["planet"] == source and row["owner"] == initial["local"]["player"] for row in initial["fleets"]))
        destination = min((row for key, row in own.items() if key != source),
                          key=lambda row: sum((a - b) ** 2 for a, b in zip(row["position"], own[source]["position"])))
        moved = self.live_input(source, destination["position"])
        self.assertGreater(moved["fleet_hud"]["drag"]["orders"], 0, moved["fleet_hud"]["drag"])
        self.assertTrue(any(name.startswith("Unit_Fleet_Move_") for name in moved["galactic_audio"]["sounds"]), moved["galactic_audio"])
        self.assertTrue(any(name.startswith("Unit_Fleet_Move_") for name in moved["galactic_audio"]["played_sounds"]), moved["galactic_audio"])
    def test_live_campaign_queue_cancel_requests_feedback(self):
        from tests.presentation.renderer import test_viewer_planet_hud_runtime as support
        initial, _ = support.PlanetHudRuntime.capture(self, "Sandbox_Equal_Footing_Underworld")
        cancelled = self.live_input(initial["map_input"]["selected"], queue=True)
        self.assertEqual(cancelled["production_queues"], [])
        self.assertEqual(cancelled["galactic_audio"]["sounds"].get("UHD_Recruitment_Canceled", 0), 1, cancelled["galactic_audio"])
        self.assertEqual(cancelled["galactic_audio"]["played_sounds"].get("UHD_Recruitment_Canceled", 0), 1, cancelled["galactic_audio"])

    def test_fleet_move_requests_ranked_member_acknowledgement(self):
        from tests.presentation.renderer import test_viewer_galactic_fleet_drag_runtime as support
        harness = support.GalacticFleetDragRuntime()
        data = harness.native_drag("second-member-radar")
        self.assertEqual(data["fleet_hud"]["drag"]["orders"], 1)
        sounds = data["galactic_audio"]["sounds"]
        self.assertEqual(sounds.get("Unit_Fleet_Move_Star_Destroyer", 0), 1, data["galactic_audio"])
        self.assertEqual(data["galactic_audio"]["played_sounds"].get("Unit_Fleet_Move_Star_Destroyer", 0), 1, data["galactic_audio"])

    def test_xwing_fleet_move_resolves_craft_and_ranks_mixed_fleet(self):
        from tests.presentation.renderer import test_viewer_galactic_fleet_drag_runtime as support
        harness = support.GalacticFleetDragRuntime()
        for fixture, cue in (("fleet-drag-xwing", "Unit_Fleet_Move_X_Wing"),
                             ("fleet-drag-xwing-roster", "Unit_Fleet_Move_X_Wing"),
                             ("fleet-drag-xwing-mixed", "Unit_Fleet_Move_Tartan")):
            with self.subTest(fixture=fixture):
                data = harness.native_drag("member-radar", fixture=fixture)
                self.assertEqual(data["fleet_hud"]["drag"]["orders"], 1)
                audio = data["galactic_audio"]
                self.assertEqual(audio["sounds"].get(cue, 0), 1, audio)
                self.assertEqual(audio["played_sounds"].get(cue, 0), 1, audio)
                self.assertEqual(audio["resolved_type_audio"]["X-WING"]["fleet_move"],
                                 "Unit_Fleet_Move_X_Wing")

    def test_queue_cancel_requests_unit_and_structure_feedback(self):
        from tests.presentation.renderer import test_viewer_galactic_production_runtime as support
        harness = support.GalacticProductionRuntime()
        initial, _ = harness.capture(3, "production")
        choices = {row["type_id"]: row["button"] for row in initial["command_bar"]["production_choices"]}
        data = harness.native_run([choices[1000], choices[1002], "b_planet_tab", "b_Create_00", "b_Create_01",
                                  "right:queue06", "right:queue05", "right:queue01", "right:queue00"], action_seconds=3.0)
        self.assertEqual(data["production_queues"], [])
        sounds = data["galactic_audio"]["sounds"]
        self.assertEqual(sounds.get("EHD_Unit_Canceled", 0), 1, data["galactic_audio"])
        self.assertEqual(sounds.get("EHD_Building_Canceled", 0), 1, data["galactic_audio"])
        for name in ("EHD_Unit_Canceled", "EHD_Building_Canceled"):
            self.assertEqual(data["galactic_audio"]["played_sounds"].get(name, 0), 1, data["galactic_audio"])

    def test_auto_resolve_win_loss_and_fought_summary_restore_strategic_music(self):
        from tests.presentation.renderer import test_viewer_galactic_battle_dialog_runtime as support
        for suffix, before_mode in (("auto-win", "battle_pending"),
                                    ("auto-loss", "battle_pending"),
                                    ("fought", "tactical")):
            with self.subTest(path=suffix):
                fixture = "battle-summary-audio-" + suffix
                initial, _ = support.GalacticBattleDialogRuntime.capture(self, fixture)
                audio = initial["galactic_audio"]
                # Stock authors both summary events blank: the existing music
                # remains until the result dialog closes (WGP-185).
                self.assertEqual(audio["mode"], before_mode, audio)
                self.assertEqual(initial["battle_dialog"]["phase"], "summary")
                button = support.GalacticBattleDialogRuntime.control(initial, "IDC_CONTINUE_BUTTON")
                returned = support.GalacticBattleDialogRuntime.click(self, fixture, button)
                self.assertEqual(returned["battle_dialog"]["phase"], "hidden")
                restored = returned["galactic_audio"]
                self.assertEqual(restored["mode"], "strategic_ambient", restored)
                self.assertTrue(restored["music"][-1].startswith("Galactic_Map_Rebel_Music_Event "), restored)
                self.assertEqual(sum(row.startswith("Galactic_Map_Rebel_Music_Event ")
                                     for row in restored["music"]), 2, restored)

    def test_fought_child_return_restores_strategic_music(self):
        with tempfile.TemporaryDirectory(prefix="galactic-audio-return-") as temporary:
            result, _ = run_viewer(self, pathlib.Path(temporary), "fought-return",
                "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                "--eawr-galactic-battle", "synthetic", "--eawr-galactic-coop-preview", "yes")
            self.assertTrue(result["handoff"]["returned"], result)
            audio = result["galactic_audio"]
            self.assertEqual(audio["mode"], "strategic_ambient", audio)
            self.assertTrue(audio["music"][-1].startswith("Galactic_Map_Rebel_Music_Event "), audio)

    def test_authored_summary_events_select_win_loss_once_then_restore_ambient(self):
        from tests.presentation.renderer.test_viewer_planet_hud_runtime import ROOT
        with tempfile.TemporaryDirectory(prefix="galactic-summary-events-") as temporary:
            directory = pathlib.Path(temporary)
            mod = directory / "mod"
            xml = mod / "Data" / "XML"
            xml.mkdir(parents=True)
            (xml.parent / "MegaFiles.xml").write_text(
                "<Mega_Files><File>Missing.meg</File></Mega_Files>", encoding="utf-8")
            (xml / "Audio.xml").write_text(
                "<Audio><Music_Event_Battle_End_Summary_Screen_Win>Galactic_Map_Empire_Music_Event"
                "</Music_Event_Battle_End_Summary_Screen_Win>"
                "<Music_Event_Battle_End_Summary_Screen_Lose>Galactic_Map_Underworld_Music_Event"
                "</Music_Event_Battle_End_Summary_Screen_Lose></Audio>", encoding="utf-8")
            driver = directory / "driver.gd"
            driver.write_text('''extends SceneTree
var elapsed = 0.0
var sent = false
func _initialize():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func _process(delta):
    elapsed += delta
    if elapsed > 1.0 and not sent:
        var button = root.find_child("IDC_CONTINUE_BUTTON", true, false)
        assert(button != null)
        sent = true
        for pressed in [true, false]:
            var event = InputEventMouseButton.new()
            event.position = button.get_global_rect().get_center()
            event.global_position = event.position
            event.button_index = MOUSE_BUTTON_LEFT
            event.pressed = pressed
            Input.parse_input_event(event)
    if elapsed > 2.0:
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''', encoding="utf-8")
            for suffix, event in (("auto-win", "Galactic_Map_Empire_Music_Event"),
                                  ("auto-loss", "Galactic_Map_Underworld_Music_Event"),
                                  ("fought", "Galactic_Map_Empire_Music_Event")):
                with self.subTest(path=suffix):
                    args = ("--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                            "--eawr-mod-root", str(mod), "--eawr-galactic-hud-fixture",
                            "battle-summary-audio-" + suffix, "--eawr-galactic-paused", "yes")
                    initial, _ = run_viewer(self, directory, suffix + "-summary", *args)
                    self.assertEqual(initial["galactic_audio"]["mode"], "battle_summary")
                    self.assertEqual(sum(row.startswith(event + " ")
                                         for row in initial["galactic_audio"]["music"]), 1)
                    report = directory / (suffix + "-returned.json")
                    completed = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"],
                        "--resolution", "1280x720", "--path", str(ROOT / "apps/viewer/project"),
                        "--script", str(driver), "--", "--eawr-galactic", "synthetic", *args,
                        "--eawr-report", str(report)], cwd=ROOT, capture_output=True, text=True, timeout=300)
                    self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
                    returned = json.loads(report.read_text(encoding="utf-8"))
                    self.assertEqual(returned["battle_dialog"]["phase"], "hidden")
                    self.assertEqual(returned["galactic_audio"]["mode"], "strategic_ambient")
                    self.assertTrue(returned["galactic_audio"]["music"][-1].startswith(
                        "Galactic_Map_Rebel_Music_Event "))

    def test_reference_opener_uses_component_sounds_and_rejects_disabled_right_click(self):
        from tests.presentation.renderer import test_viewer_galactic_command_bar_runtime as support
        initial, _ = support.GalacticCommandBarRuntime.capture(self, 1, "accessible")
        control = next(row for row in initial["command_bar"]["controls"] if row["name"] == "b_droid_help")
        rect = control["hit_rect"]
        point = [rect[0] + rect[2] / 2, rect[1] + rect[3] / 2]
        with tempfile.TemporaryDirectory(prefix="galactic-component-audio-") as temporary:
            directory = pathlib.Path(temporary)
            report, driver = directory / "report.json", directory / "input.gd"
            driver.write_text("""extends SceneTree
var frame = 0
var point = Vector2(%s, %s)
func _initialize():
    call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func motion(position):
    var event = InputEventMouseMotion.new()
    event.position = position
    event.global_position = position
    Input.parse_input_event(event)
func click(button, down):
    var event = InputEventMouseButton.new()
    event.position = point
    event.global_position = point
    event.button_index = button
    event.pressed = down
    Input.parse_input_event(event)
func _process(_delta):
    frame += 1
    var target = root.find_child("b_droid_help", true, false)
    if frame == 20:
        assert(target != null)
        target.disabled = true
        motion(point)
    if frame == 21: click(MOUSE_BUTTON_LEFT, true)
    if frame == 22: click(MOUSE_BUTTON_LEFT, false)
    if frame == 25: motion(Vector2(640, 200))
    if frame == 30:
        target.disabled = false
        motion(point)
    if frame == 31: click(MOUSE_BUTTON_RIGHT, true)
    if frame == 32: click(MOUSE_BUTTON_RIGHT, false)
    if frame == 35: click(MOUSE_BUTTON_LEFT, true)
    if frame == 36: click(MOUSE_BUTTON_LEFT, false)
    if frame == 60:
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
""" % tuple(point), encoding="utf-8")
            result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
                "--eawr-galactic", "synthetic", "--eawr-galactic-seat", "1",
                "--eawr-galactic-hud-fixture", "accessible", "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                "--eawr-galactic-paused", "yes", "--eawr-report", str(report)],
                cwd=support.ROOT, capture_output=True, text=True, timeout=300, check=False)
            self.assertTrue(report.is_file(), result.stdout + result.stderr)
            data = json.loads(report.read_text(encoding="utf-8"))
            self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
            sounds = data["galactic_audio"]["sounds"]
            self.assertEqual(sounds.get("GUI_Dial_Switch_SFX", 0), 1, sounds)
            self.assertEqual(sounds.get("GUI_Highlight_Mouse_Over", 0), 1, sounds)

    def test_music_decodes_content_under_reversed_extensions(self):
        from tools.inventory.corpus import Corpus
        from tests.presentation.renderer.audio_stream_fixture import reversed_extensions
        with tempfile.TemporaryDirectory(prefix="galactic-audio-content-") as temporary:
            directory = pathlib.Path(temporary)
            mod = directory / "mod"
            xml = mod / "Data/XML"
            xml.mkdir(parents=True)
            (xml.parent / "MegaFiles.xml").write_text(
                "<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
            files = reversed_extensions(self, Corpus(os.environ["EAWR_EAW_GAME_ROOT"]), xml.parent)
            for name in files:
                with self.subTest(file=name):
                    (xml / "MusicEvents.xml").write_text(
                        '<MusicEvents><MusicEvent Name="Galactic_Map_Rebel_Music_Event">'
                        f'<Files>{name}</Files></MusicEvent></MusicEvents>', encoding="utf-8")
                    result, _ = run_viewer(self, directory, name,
                        "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                        "--eawr-mod-root", str(mod), "--eawr-galactic-seat", "1")
                    audio = result["galactic_audio"]
                    self.assertTrue(any(name in row for row in audio["music"]), audio)
                    self.assertFalse(any("undecodable" in row or "could not load" in row
                                         for row in audio["problems"]), audio)

    def test_missing_music_file_reports_its_named_cue(self):
        with tempfile.TemporaryDirectory(prefix="galactic-audio-missing-") as temporary:
            directory = pathlib.Path(temporary)
            mod = directory / "mod"
            xml = mod / "Data" / "XML"
            xml.mkdir(parents=True)
            (xml.parent / "MegaFiles.xml").write_text(
                "<Mega_Files><File>Missing.meg</File></Mega_Files>", encoding="utf-8")
            (xml / "MusicEvents.xml").write_text(
                '<MusicEvents><MusicEvent Name="Galactic_Map_Rebel_Music_Event">'
                '<Files>gc_audio_missing_regression.mp3</Files>'
                '</MusicEvent></MusicEvents>', encoding="utf-8")
            result, _ = run_viewer(self, directory, "missing-music",
                                  "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                                  "--eawr-mod-root", str(mod), "--eawr-galactic-seat", "1")
            self.assertIn("music cue Galactic_Map_Rebel_Music_Event could not load: "
                          "gc_audio_missing_regression.mp3", result["galactic_audio"]["problems"])

    def test_coop_seats_share_music_and_the_other_faction_has_its_own_event(self):
        with tempfile.TemporaryDirectory(prefix="galactic-audio-") as temporary:
            rows = []
            for seat, event in ((1, "Galactic_Map_Rebel_Music_Event"),
                                (2, "Galactic_Map_Rebel_Music_Event"),
                                (5, "Galactic_Map_Empire_Music_Event")):
                result, _ = run_viewer(self, pathlib.Path(temporary), f"seat-{seat}",
                                      "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                                      "--eawr-galactic-seat", str(seat), "--eawr-galactic-coop-preview", "yes")
                audio = result["galactic_audio"]
                self.assertTrue(audio["muted"])
                self.assertEqual(len(audio["music"]), 1, audio)
                self.assertTrue(audio["music"][0].startswith(event + " "), audio)
                self.assertFalse(any("missing or undecodable audio" in row for row in audio["problems"]), audio)
                self.assertEqual(result["audio_event_gaps"], 0)
                rows.append(audio["music"])
            self.assertEqual(rows[0], rows[1])
            self.assertNotEqual(rows[0], rows[2])


if __name__ == "__main__":
    unittest.main()
