"""WGP-23/24 local-faction music through the installed XML and viewer adapter."""
import json
import math
import subprocess
import os
import pathlib
import tempfile
import unittest

from tests.galactic.test_galactic_viewer_mode import run_viewer


class GalacticMusicMixerContract(unittest.TestCase):
    def test_samples_use_localize_category_without_duplicate_slider_gain(self):
        root = pathlib.Path(__file__).resolve().parents[3]
        source = (root / "apps/viewer/src/galactic/galactic_audio.cpp").read_text(encoding="utf-8")
        self.assertNotIn("audio::category_gain", source)
        self.assertNotIn('set_bus_send(bus, "Master")', source)
        self.assertIn("speech_player_->set_bus(audio_buses::names[audio_buses::voice])", source)
        service = source.split("void GalacticAudio::service(", 1)[1].split("void GalacticAudio::record_start", 1)[0]
        self.assertEqual(service.count("request.event->localized ? audio_buses::voice : audio_buses::sfx"), 2)
        self.assertIn("voice.gain = start.volume;", service)
        self.assertIn("audio::speech_sfx_gain(start.volume, speech_stream_.active(), false)", service)
        self.assertIn("voice.gain * events_.fade(voice.handle) * audio::falloff_gain", service)

    def test_opening_and_faded_tracks_use_options_buses_before_playback(self):
        root = pathlib.Path(__file__).resolve().parents[3]
        source = (root / "apps/viewer/src/galactic/galactic_audio.cpp").read_text(encoding="utf-8")
        constructor = source.split("GalacticAudio::GalacticAudio(", 1)[1].split("GalacticAudio::~", 1)[0]
        self.assertIn("audio_buses::ensure(saved ? mix_levels(saved->audio) : audio::MixLevels{})", constructor)
        tracks = constructor.split("for (auto& track : tracks_)", 1)[1].split("speech_player_", 1)[0]
        self.assertLess(tracks.index("set_bus(audio_buses::names[audio_buses::music])"),
                        tracks.index("host.add_child(track.player)"))
        playback = source.split("void GalacticAudio::play(", 1)[1].split("void GalacticAudio::write_report", 1)[0]
        self.assertNotIn("default_volume_slider", playback)
        self.assertIn("db(muted_ ? 0 : next.volume * next.fade.level)", playback)
        self.assertIn("db(muted_ ? 0 : track.volume * track.fade.level)", playback)
        self.assertLess(playback.index("music_starts_.push_back"), playback.index("next.player->play()"))


class GalacticAdvisorDurationContract(unittest.TestCase):
    def test_missing_or_invalid_duration_disables_only_text(self):
        from tools.native_cpp_fixture import compile_fixture, select_toolchain
        root = pathlib.Path(__file__).resolve().parents[3]
        toolchain = select_toolchain()
        self.assertIsNotNone(toolchain, "the advisor contract requires a native C++ toolchain")
        source = r'''
#include "__HEADER__"
#include <cassert>
int main() {
    using eawr::viewer::galactic::advisor_hint_duration;
    assert(!advisor_hint_duration({})); // absent XML text, not a fabricated default
    for (const auto text : {" ", "garbage", "-0.1", "nan", "inf", "1e999", "0.1s"})
        assert(!advisor_hint_duration(text));
    assert(advisor_hint_duration("0") == 0.0);
    assert(advisor_hint_duration(" 0.1 ") == 0.1);
    assert(advisor_hint_duration("2.75") == 2.75);
    assert(advisor_hint_duration("1e-1") == 0.1);
}
'''.replace("__HEADER__", (root / "apps/viewer/src/galactic/advisor_duration.hpp").as_posix())
        with tempfile.TemporaryDirectory(prefix="eawr-advisor-duration-") as temporary:
            directory = pathlib.Path(temporary)
            cpp = directory / "duration.cpp"
            executable = directory / ("duration.exe" if os.name == "nt" else "duration")
            cpp.write_text(source, encoding="utf-8")
            compile_fixture(toolchain, cpp, executable)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST")
                     and os.environ.get("EAWR_EAW_GAME_ROOT"), "requires viewer GPU host and game data")
class GalacticAudioGpu(unittest.TestCase):
    def test_opening_music_starts_at_saved_music_and_master_levels(self):
        from tests.presentation.renderer.test_viewer_planet_hud_runtime import ROOT
        with tempfile.TemporaryDirectory(prefix="galactic-opening-volume-") as temporary:
            directory = pathlib.Path(temporary)
            driver = directory / "input.gd"
            driver.write_text('''extends SceneTree
var frame = 0
func _initialize():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func _process(_delta):
    frame += 1
    if frame == 20:
        var button = root.find_child("b_play_pause", true, false)
        assert(button != null)
        assert(not button.disabled, "the audio input fixture must enable the pause control")
        var motion = InputEventMouseMotion.new()
        motion.position = button.get_global_rect().get_center()
        motion.global_position = motion.position
        Input.parse_input_event(motion)
        for pressed in [true, false]:
            var event = InputEventMouseButton.new()
            event.position = button.get_global_rect().get_center()
            event.global_position = event.position
            event.button_index = MOUSE_BUTTON_LEFT
            event.pressed = pressed
            Input.parse_input_event(event)
    if frame == 60:
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''', encoding="utf-8")
            for master, music in ((40, 60), (0, 60), (40, 0)):
                with self.subTest(master=master, music=music):
                    name = f"master-{master}-music-{music}"
                    settings = directory / "eawr-settings.cfg"
                    settings.write_text(f"eawr-settings 1\naudio.master={master}\naudio.music={music}\n", encoding="utf-8")
                    report = directory / (name + ".json")
                    result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                        "--path", str(ROOT / "apps/viewer/project"), "--script", str(driver), "--",
                        "--eawr-galactic", "synthetic", "--eawr-galactic-seat", "1",
                        "--eawr-galactic-hud-fixture", "accessible",
                        "--eawr-galactic-paused", "yes",
                        "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                        "--eawr-settings-file", str(settings), "--eawr-report", str(report)],
                        cwd=ROOT, capture_output=True, text=True, timeout=300)
                    (directory / (name + ".log")).write_text(result.stdout + result.stderr, encoding="utf-8")
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    data = json.loads(report.read_text(encoding="utf-8"))
                    self.assertFalse(data["paused"], "the real click must resume the galaxy")
                    audio = data["galactic_audio"]
                    self.assertTrue(audio["music"], audio)
                    self.assertTrue(audio["music_starts"], audio)
                    db = lambda gain: -80.0 if gain <= 0.0001 else 20.0 * math.log10(gain)
                    for start in audio["music_starts"]:
                        self.assertEqual(start["bus"], "EAWR_Music", start)
                        self.assertEqual(start["send"], "EAWR_Mix", start)
                        self.assertAlmostEqual(start["music_db"], db(music / 100), delta=0.001)
                        self.assertAlmostEqual(start["master_db"], db(master / 100), delta=0.001)
                        gain = 0 if audio["muted"] else start["event_volume"] * start["fade"]
                        self.assertAlmostEqual(start["stream_db"], db(gain), delta=0.001)

    def mixer_run(self, directory, levels, preview=False):
        from tests.presentation.renderer.test_viewer_planet_hud_runtime import ROOT
        settings, report = directory / "settings.cfg", directory / "report.json"
        settings.write_text("eawr-settings 1\n" + "".join(
            f"audio.{key}={value}\n" for key, value in levels.items()), encoding="utf-8")
        trace = directory / "mixer.json"
        driver = directory / "input.gd"
        driver.write_text(r'''extends SceneTree
const SETTINGS = __SETTINGS__
const TRACE = __TRACE__
const PREVIEW = __PREVIEW__
var instance
var elapsed = 0.0
var beat = 0.0
var phase = 0
var snapshots = []
var sliders = ["IDC_SLIDER_MASTER_VOLUME", "IDC_SLIDER_SOUND_EFFECTS_VOLUME", "IDC_SLIDER_DIALOG_VOLUME", "IDC_SLIDER_MUSIC_VOLUME"]
var changed = [25, 35, 45, 55]
func _initialize():
    # The in-game Options screen reads user://; seed only this launch's private profile.
    DirAccess.copy_absolute(SETTINGS, ProjectSettings.globalize_path("user://eawr-settings.cfg"))
    instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func visible(node, name):
    if str(node.name) == name and node is Control and node.is_visible_in_tree(): return node
    for child in node.get_children():
        var found = visible(child, name)
        if found != null: return found
    return null
func button(name):
    var node = visible(root, name)
    if node == null or node.disabled: return false
    var motion = InputEventMouseMotion.new()
    motion.position = node.get_global_rect().get_center()
    motion.global_position = motion.position
    Input.parse_input_event(motion)
    for pressed in [true, false]:
        var event = InputEventMouseButton.new()
        event.position = motion.position
        event.global_position = motion.position
        event.button_index = MOUSE_BUTTON_LEFT
        event.pressed = pressed
        Input.parse_input_event(event)
    return true
func escape():
    for pressed in [true, false]:
        var event = InputEventKey.new()
        event.keycode = KEY_ESCAPE
        event.pressed = pressed
        Input.parse_input_event(event)
func snapshot(label):
    var buses = {}
    for name in ["EAWR_Mix", "EAWR_SFX", "EAWR_Voice", "EAWR_Music"]:
        var index = AudioServer.get_bus_index(name)
        assert(index >= 0)
        buses[name] = {"db": AudioServer.get_bus_volume_db(index), "send": str(AudioServer.get_bus_send(index))}
    snapshots.append({"label": label, "buses": buses})
func finish(error):
    var file = FileAccess.open(TRACE, FileAccess.WRITE)
    file.store_string(JSON.stringify({"error": error, "phase": phase, "snapshots": snapshots, "engine_arguments": OS.get_cmdline_args()}))
    instance.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
    quit(0 if error == "" else 2)
func _process(delta):
    elapsed += delta
    beat += delta
    if elapsed > 120:
        finish("mixer driver timeout")
        return false
    if beat < 0.3: return false
    beat = 0
    match phase:
        0:
            if button("b_play_pause"): phase = 1
        1:
            if elapsed < 5: return false
            snapshot("saved")
            if not PREVIEW:
                finish("")
                return false
            escape()
            phase = 2
        2:
            if button("IDC_INGAME_OPTIONS"): phase = 3
        3:
            if button("IDC_IN_GAME_OPTIONS_AUDIO"): phase = 4
        4, 5, 6, 7:
            var slider = visible(root, sliders[phase - 4])
            if slider == null: return false
            slider.value = changed[phase - 4]
            snapshot("preview-" + str(phase - 4))
            phase += 1
        8:
            if button("IDC_CANCEL"): phase = 9
        9:
            snapshot("cancel")
            if button("IDC_IN_GAME_OPTIONS_AUDIO"): phase = 10
        10:
            for index in range(4):
                var slider = visible(root, sliders[index])
                if slider == null: return false
                slider.value = changed[index]
            if button("IDC_OK"): phase = 11
        11:
            snapshot("accept")
            finish("")
    return false
'''.replace("__SETTINGS__", json.dumps(settings.as_posix()))
            .replace("__TRACE__", json.dumps(trace.as_posix()))
            .replace("__PREVIEW__", "true" if preview else "false"), encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--render-thread", "separate",
            "--resolution", "1280x720",
            "--path", str(ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-seat", "1",
            "--eawr-galactic-hud-fixture", "audio-mixer", "--eawr-galactic-paused", "yes",
            "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
            "--eawr-settings-file", str(settings), "--eawr-report", str(report)],
            cwd=ROOT, capture_output=True, text=True, timeout=300)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        proof = json.loads(trace.read_text(encoding="utf-8"))
        self.assertEqual(proof["error"], "", proof)
        if not preview:
            self.assertFalse(data["paused"], "real pause-button input must resume the galaxy")
        return data, proof

    def test_gc_samples_start_on_saved_independent_category_and_master_levels(self):
        base = dict(master=40, sfx=60, speech=30, music=20)
        for zero in (None, "master", "sfx", "speech", "music"):
            with self.subTest(zero=zero), tempfile.TemporaryDirectory(prefix="galactic-sample-volume-") as temporary:
                levels = dict(base)
                if zero:
                    levels[zero] = 0
                data, _ = self.mixer_run(pathlib.Path(temporary), levels)
                audio = data["galactic_audio"]
                expected = {("pooled_2d", "GUI_Dial_Switch_SFX"): "EAWR_SFX",
                            ("pooled_2d", "Unit_Fleet_Move_Star_Destroyer"): "EAWR_Voice",
                            ("spatial", "Unit_Boron_Missile_Fire"): "EAWR_SFX",
                            ("spatial", "Unit_Attacked_C3PO"): "EAWR_Voice",
                            ("speech", "Speech_Death_Star_Build_Underway"): "EAWR_Voice"}
                starts = {(row["kind"], row["event"]): row for row in audio["sample_starts"]}
                db = lambda percent: -80.0 if percent == 0 else 20 * math.log10(percent / 100)
                for key, category in expected.items():
                    self.assertIn(key, starts, audio)
                    start = starts[key]
                    self.assertEqual(start["category"], category, start)
                    self.assertEqual(start["send"], "EAWR_Mix", start)
                    if key[0] == "pooled_2d":
                        self.assertTrue(start["bus"].startswith("EAWR_GC_Pan_"), start)
                    else:
                        self.assertEqual(start["bus"], category, start)
                    self.assertAlmostEqual(start["category_db"], db(levels["sfx" if category == "EAWR_SFX" else "speech"]), delta=0.001)
                    self.assertAlmostEqual(start["master_db"], db(levels["master"]), delta=0.001)
                    if key[0] != "speech":
                        authored = {"GUI_Dial_Switch_SFX": 0.8, "Unit_Fleet_Move_Star_Destroyer": 0.9,
                                    "Unit_Boron_Missile_Fire": 0.6, "Unit_Attacked_C3PO": 0.75}
                        self.assertAlmostEqual(start["gain"], authored[key[1]], delta=0.001,
                                               msg="authored volume must not include a second slider/master gain")
                self.assertTrue(audio["music_starts"], audio)
                for start in audio["music_starts"]:
                    self.assertAlmostEqual(start["music_db"], db(levels["music"]), delta=0.001)
                    self.assertAlmostEqual(start["master_db"], db(levels["master"]), delta=0.001)

    def test_gc_options_preview_cancel_accept_reach_existing_category_routes(self):
        base = dict(master=40, sfx=60, speech=30, music=20)
        with tempfile.TemporaryDirectory(prefix="galactic-options-mixer-") as temporary:
            _, proof = self.mixer_run(pathlib.Path(temporary), base, preview=True)
            current = list(base.values())
            wanted = {"saved": current.copy()}
            for index, level in enumerate((25, 35, 45, 55)):
                current[index] = level
                wanted[f"preview-{index}"] = current.copy()
            wanted["cancel"] = list(base.values())
            wanted["accept"] = [25, 35, 45, 55]
            self.assertEqual([row["label"] for row in proof["snapshots"]], list(wanted))
            for snapshot in proof["snapshots"]:
                for bus, percent in zip(("EAWR_Mix", "EAWR_SFX", "EAWR_Voice", "EAWR_Music"), wanted[snapshot["label"]]):
                    self.assertAlmostEqual(snapshot["buses"][bus]["db"], 20 * math.log10(percent / 100), delta=0.001)
                    self.assertEqual(snapshot["buses"][bus]["send"], "Master" if bus == "EAWR_Mix" else "EAWR_Mix")

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

    def test_palpatine_fleet_move_uses_live_passenger_response(self):
        from tests.presentation.renderer import test_viewer_galactic_fleet_drag_runtime as support
        harness = support.GalacticFleetDragRuntime()
        for fixture in ("fleet-drag-palpatine", "fleet-drag-palpatine-mixed"):
            with self.subTest(fixture=fixture):
                data = harness.native_drag("member-radar", fixture=fixture)
                self.assertEqual(data["fleet_hud"]["drag"]["orders"], 1)
                audio = data["galactic_audio"]
                self.assertEqual(audio["resolved_type_audio"]["EMPEROR_PALPATINE"]["fleet_move"],
                                 "Unit_Fleet_Move_Emperor")
                self.assertEqual(audio["sounds"].get("Unit_Fleet_Move_Emperor", 0), 1, audio)
                self.assertEqual(audio["played_sounds"].get("Unit_Fleet_Move_Emperor", 0), 1, audio)
                self.assertEqual(audio["sounds"].get("Unit_Fleet_Move_Lamda_Shuttle", 0), 0, audio)

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

    def test_objective_teletype_and_dialog_events_use_gc_router(self):
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
    if frame == 40:
        var close_button = root.find_child("IDC_BUTTON_CLOSE", true, false)
        assert(close_button != null)
        point = close_button.get_global_rect().get_center()
        close_button.disabled = true
        motion(point)
    if frame == 41: click(MOUSE_BUTTON_LEFT, true)
    if frame == 42: click(MOUSE_BUTTON_LEFT, false)
    if frame == 43: motion(Vector2(0, 0))
    if frame == 44:
        root.find_child("IDC_BUTTON_CLOSE", true, false).disabled = false
        motion(point)
    if frame == 45: click(MOUSE_BUTTON_RIGHT, true)
    if frame == 46: click(MOUSE_BUTTON_RIGHT, false)
    if frame == 47: click(MOUSE_BUTTON_LEFT, true)
    if frame == 48: click(MOUSE_BUTTON_LEFT, false)
    if frame == 60:
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
""" % tuple(point), encoding="utf-8")
            result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
                "--eawr-galactic", "synthetic", "--eawr-galactic-seat", "1",
                "--eawr-galactic-hud-fixture", "objectives-audio", "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                "--eawr-galactic-paused", "yes", "--eawr-report", str(report)],
                cwd=support.ROOT, capture_output=True, text=True, timeout=300, check=False)
            self.assertTrue(report.is_file(), result.stdout + result.stderr)
            data = json.loads(report.read_text(encoding="utf-8"))
            self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
            sounds = data["galactic_audio"]["sounds"]
            self.assertEqual(sounds.get("GUI_Dial_Switch_SFX", 0), 1, sounds)
            self.assertEqual(sounds.get("GUI_Highlight_Mouse_Over", 0), 1, sounds)

            # One announce (a non-announcing rebuild is silent), two synthetic
            # reveal steps, and one admitted dialog press; disabled/right input is silent.
            for event, count in (("GUI_Button_Press_2_SFX", 1), ("GUI_Text_SFX", 2),
                                 ("GUI_Common_Dialog_Button_Press", 1),
                                 ("GUI_Common_Dialog_Button_Hilight", 1)):
                self.assertEqual(sounds.get(event, 0), count, data["galactic_audio"])
                self.assertGreater(data["galactic_audio"]["played_sounds"].get(event, 0), 0,
                                   data["galactic_audio"])

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
