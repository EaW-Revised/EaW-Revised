"""The options screen (#2927): FoC's options dialogs over the player settings.

`--eawr-options` builds the hub or one options page from the dialog resources, drives it
with scripted steps (`--eawr-options-do`) and writes a report of the saved settings and the
pages' controls. The settings file is named with `--eawr-settings-file`, so a run never
touches the player's own.

The structural half runs everywhere. The graphical half is opt-in: set
EAWR_GODOT_VIEWER_RUNTIME_TEST, EAWR_GODOT_EXECUTABLE and EAWR_EAW_GAME_ROOT. It extracts the
four faces from that install into a temporary cache, which is deleted after the run. Set
EAWR_OPTIONS_CAPTURE_DIR to keep the page captures for an eye check.
"""

import json
import itertools
import math
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
from viewer_mode_sources import source_text  # noqa: E402
from dialog_rectangles import assert_dialog_rectangles  # noqa: E402

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[3] / "tools/common"))
import eawr_python_helpers as python_helpers  # noqa: E402

SRC = ROOT / "apps/viewer/src"
TOOL = ROOT / "tools/fonts/extract_eaw_fonts.py"

RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")


class OptionsScreenStructure(unittest.TestCase):
    def test_the_mode_and_the_screen_are_wired_into_the_host_and_the_build(self):
        ready = (SRC / "viewer_host_ready.cpp").read_text(encoding="utf-8")
        self.assertIn("OptionsMode::requested()", ready)
        self.assertIn("options_mode_->process()", (SRC / "viewer_host_process.cpp").read_text(encoding="utf-8"))
        self.assertIn("EawrOptionsScreen", (SRC / "register_types.cpp").read_text(encoding="utf-8"))
        build = source_text("apps/viewer/CMakeLists.txt")
        for source in ("src/options_mode.cpp", "src/options_screen.cpp", "src/settings_store.cpp"):
            self.assertIn(source, build)
        self.assertIn("settings.cpp", (ROOT / "src/presentation/ui/sources.cmake").read_text(encoding="utf-8"))

    def test_the_screen_reuses_the_dialog_builder_and_signals_its_close(self):
        screen = (SRC / "options_screen.cpp").read_text(encoding="utf-8")
        self.assertIn("build_dialog(", screen)
        self.assertIn('ADD_SIGNAL(MethodInfo("closed"))', screen)
        self.assertIn("EawrUiModalLayer", screen)
        for dialog in ("IDD_AUDIO_OPTIONS_DIALOG", "IDD_VIDEO_OPTIONS_DIALOG", "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG",
                       "IDD_GAMEPLAY_OPTIONS_DIALOG", "IDD_KEYBOARD_OPTIONS_DIALOG", "IDD_NETWORK_OPTIONS_DIALOG",
                       "IDD_MAIN_OPTIONS_DIALOG", "IDD_IN_GAME_OPTIONS_DIALOG"):
            self.assertIn(dialog, screen)

    def test_online_service_rows_use_the_loaded_network_dialog_and_validate_before_saving(self):
        screen = (SRC / "options_screen.cpp").read_text(encoding="utf-8")
        self.assertIn("data::Dialog service = *found", screen)
        self.assertNotIn("std::erase_if(service.controls", screen)
        self.assertIn("validate_online_settings(draft_.network)", screen)
        self.assertIn('IDC_MAIN_OPTIONS_NETWORK', screen)
        self.assertIn('state.dialog.emplace(build_dialog(service', screen)

    def test_settings_never_reach_the_simulation(self):
        # UI-C2 and UI-07: the model is engine-free and the screen and store include no simulation.
        model = (ROOT / "include/eawr/presentation/ui/settings.hpp").read_text(encoding="utf-8")
        self.assertNotIn("godot", model.lower().replace("godot_backend", ""))
        for name in ("options_screen.cpp", "options_screen.hpp", "options_mode.cpp", "settings_store.cpp"):
            text = (SRC / name).read_text(encoding="utf-8")
            self.assertNotIn("eawr/sim/", text, name)
            self.assertNotIn("eawr/skirmish/", text, name)

    def test_nothing_in_the_data_is_a_fixed_content_list(self):
        # Mods change the data: the screen takes captions, skin and dialogs from the loaded files.
        screen = (SRC / "options_screen.cpp").read_text(encoding="utf-8")
        for fixed in ("MasterTextFile_", "res://fonts", ".ttf\""):
            self.assertNotIn(fixed, screen)


def run_viewer(test, directory, name, cache, settings, *extra, resolution="1280x720", capture=True):
    executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
    test.assertTrue(executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot binary")
    report = directory / f"{name}.json"
    arguments = [executable, "--resolution", resolution, "--path", str(ROOT / "apps/viewer/project"), "--",
                 "--eawr-options", "--eawr-game-root", GAME_ROOT, "--eawr-font-cache", str(cache),
                 "--eawr-settings-file", str(settings), *extra, "--eawr-report", str(report)]
    keep = os.environ.get("EAWR_OPTIONS_CAPTURE_DIR")
    picture = (pathlib.Path(keep) if keep else directory) / f"{name}.png"
    if capture:
        picture.parent.mkdir(parents=True, exist_ok=True)
        arguments += ["--eawr-capture", str(picture)]
    completed = python_helpers.run_viewer(arguments, cwd=ROOT, timeout=300)
    test.assertTrue(report.is_file(), completed.stdout)
    result = python_helpers.read_json(report)
    test.assertEqual(completed.returncode, 0, result.get("failure") or completed.stdout)
    test.assertEqual(result["status"], "ok", result.get("failure"))
    test.assertEqual(result["failure"], "")
    test.assertNotIn("ERROR:", completed.stdout)
    if result["display"]["vsync"] and result["backend"]["rendering_method"] != "gl_compatibility":
        if "The requested V-Sync mode Mailbox is not available" in completed.stdout:
            test.assertEqual(result["display"]["vsync_mode"], 1)
        else:
            test.assertEqual(result["display"]["vsync_mode"], 3)
    test.assertTrue(all(action.endswith(" ok") for action in result["actions"]), result["actions"])
    return result


def settings_lines(result):
    return dict(line.split("=", 1) for line in result["committed"][1:])


def controls(snapshot):
    return {control["id"]: control for control in snapshot["controls"]}


@unittest.skipUnless(RUNTIME and GAME_ROOT,
                     "set EAWR_GODOT_VIEWER_RUNTIME_TEST, EAWR_GODOT_EXECUTABLE and EAWR_EAW_GAME_ROOT")
class OptionsScreenGraphical(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-options-"))
        cls.cache = pathlib.Path(tempfile.mkdtemp(prefix="eawr-font-cache-"))
        extracted = subprocess.run([sys.executable, str(TOOL), "--game-root", GAME_ROOT, "--out", str(cls.cache),
                                    "--allow-unknown-build", "--json"],
                                   capture_output=True, text=True, timeout=120, check=False)
        if extracted.returncode != 0:
            raise RuntimeError(extracted.stderr)

    @classmethod
    def tearDownClass(cls):
        # The reports and captures stay (the rig copies the temporary folder back); the faces do not.
        shutil.rmtree(cls.cache, ignore_errors=True)

    def settings(self, name):
        path = self.directory / f"{name}.cfg"
        path.unlink(missing_ok=True)
        return path

    def assert_network_rectangles(self, geometry):
        controls = geometry["controls"]
        self.assertTrue(controls)
        for control in controls:
            assert_dialog_rectangles(self, {"dialog": geometry["dialog"], "controls": [control]})
        for first, second in itertools.combinations(controls, 2):
            intended_frame_overlay = False
            for group, label in ((first, second), (second, first)):
                if group["kind"] != "group" or label["kind"] != "label":
                    continue
                if "authored" not in group or "authored" not in label:
                    continue
                gx, gy, gw, gh = group["rect"]
                lx, ly, lw, lh = label["rect"]
                # Retail Network dialog data lets labels straddle ONLY the left edge
                # of a group backdrop. Their centre and remaining edges stay inside,
                # and neither authored rectangle may move or grow for this exception.
                if (lx < gx and gx <= lx + lw / 2 <= gx + gw and gy <= ly + lh / 2 <= gy + gh
                        and gy <= ly and ly + lh <= gy + gh and lx + lw <= gx + gw):
                    self.assertEqual(group["rect"], group["authored"], "frame overlay requires unchanged retail frame")
                    self.assertEqual(label["rect"], label["authored"], "frame overlay requires unchanged retail label")
                    intended_frame_overlay = True
            if not intended_frame_overlay:
                assert_dialog_rectangles(self, {"dialog": geometry["dialog"], "controls": [first, second]})

    def test_dialog_rectangles_network_options(self):
        for resolution in ("1920x1080", "1280x720"):
            with self.subTest(resolution=resolution):
                result = run_viewer(self, self.directory, "dialog-rectangles-network-" + resolution,
                                    self.cache, self.settings("dialog-rectangles-" + resolution),
                                    "--eawr-options-page", "network", resolution=resolution)
                geometry = result["rectangles"]
                self.assert_network_rectangles(geometry)
                labels = [control for control in geometry["controls"] if control["kind"] == "label"]
                self.assertTrue(labels)
                for label in labels:
                    for actual, authored in zip(label["rect"], label["authored"]):
                        self.assertAlmostEqual(actual, authored, delta=0.1, msg=label["id"])

    def test_online_service_settings_persist_validate_and_cancel(self):
        settings = self.settings("online-service")
        first = run_viewer(self, self.directory, "online-service-save", self.cache, settings,
                           "--eawr-options-page", "network",
                           "--eawr-options-do", "text:IDC_ONLINE_SERVICE_HOST=service.example",
                           "--eawr-options-do", "text:IDC_ONLINE_SERVICE_PORT=27021",
                           "--eawr-options-do", "text:IDC_ONLINE_SERVICE_PIN=" + "a" * 64,
                           "--eawr-options-do", "set:IDC_ONLINE_RELAY_ONLY=1",
                           "--eawr-options-do", "press:IDC_OK")
        saved = settings.read_text(encoding="utf-8")
        self.assertIn("network.online_host=service.example", saved)
        self.assertIn("network.online_port=27021", saved)
        self.assertIn("network.online_relay_only=1", saved)
        self.assertNotIn("service.example", json.dumps(first))
        reopened = run_viewer(self, self.directory, "online-service-reopen", self.cache, settings,
                             "--eawr-options-page", "network",
                             "--eawr-options-do", "text:IDC_ONLINE_SERVICE_PORT=0",
                             "--eawr-options-do", "press:IDC_OK")
        self.assertEqual(reopened["page"], "network", "invalid draft keeps the page open")
        self.assertEqual(settings.read_text(encoding="utf-8"), saved)
        self.assertEqual(controls(reopened)["IDC_ONLINE_SERVICE_HOST"]["text"], "configured")
        for authored in ("IDC_NETWORK_PORT_NUMBER", "IDC_INTERNET_CONNECTION_SPEED_COMBO",
                         "IDC_INTERNET_PREFERRED_LOCALE", "IDC_INTERNET_PREFERRED_QUICKMATCH_FACTION"):
            self.assertIn(authored, controls(reopened))
        cancelled = run_viewer(self, self.directory, "online-service-cancel", self.cache, settings,
                              "--eawr-options-page", "network",
                              "--eawr-options-do", "text:IDC_ONLINE_SERVICE_HOST=changed.example",
                              "--eawr-options-do", "press:IDC_BACK")
        self.assertEqual(settings.read_text(encoding="utf-8"), saved)
        self.assertEqual(cancelled["page"], "hub")

    def test_a_changed_volume_and_video_state_persist_across_a_restart(self):
        # The #2927 GPU case: open the options, change a volume and the full-screen and vsync
        # state, Accept, restart, and the saved values are the ones the pages show.
        settings = self.settings("persist")
        first = run_viewer(self, self.directory, "persist-first", self.cache, settings,
                           "--eawr-options-page", "audio",
                           "--eawr-options-do", "set:IDC_SLIDER_MASTER_VOLUME=30",
                           "--eawr-options-do", "set:IDC_SLIDER_MUSIC_VOLUME=55",
                           "--eawr-options-do", "press:IDC_OK",
                           "--eawr-options-do", "page:video",
                           "--eawr-options-do", "set:IDC_WINDOWED_MODE=0",
                           "--eawr-options-do", "set:IDC_VSYNC_ENABLED=1",
                           "--eawr-options-do", "press:IDC_OK")
        self.assertFalse(first["settings_file"]["existed_at_start"])
        self.assertTrue(settings.is_file())
        saved = settings_lines(first)
        self.assertEqual((saved["audio.master"], saved["audio.music"]), ("30", "55"))
        self.assertEqual((saved["video.windowed"], saved["video.vsync"]), ("0", "1"))
        # A capture run's window is pinned: vsync is applied, the window mode is not.
        self.assertTrue(first["display"]["vsync"])
        # Mailbox is synchronized; a driver without it may fall back to FIFO.
        self.assertIn(first["display"]["vsync_mode"], (1, 3))
        self.assertIn("window: not applied in this run", first["skipped"])
        # The record a battle would take: the saved volumes reach their buses, the speed its step.
        self.assertAlmostEqual(first["session"]["master"], 0.30)
        self.assertAlmostEqual(first["session"]["music"], 0.55)
        self.assertAlmostEqual(first["session"]["sfx"], 0.75)
        self.assertEqual(first["session"]["speed_step"], 2)

        second = run_viewer(self, self.directory, "persist-second", self.cache, settings,
                            "--eawr-options-page", "audio",
                            "--eawr-options-do", "snapshot",
                            "--eawr-options-do", "page:video",
                            "--eawr-options-do", "snapshot")
        self.assertTrue(second["settings_file"]["existed_at_start"])
        audio, video = (controls(snapshot) for snapshot in second["snapshots"])
        self.assertEqual(audio["IDC_SLIDER_MASTER_VOLUME"]["value"], 30)
        self.assertEqual(audio["IDC_SLIDER_MUSIC_VOLUME"]["value"], 55)
        self.assertEqual(audio["IDC_SLIDER_SOUND_EFFECTS_VOLUME"]["value"], 75)
        self.assertEqual(video["IDC_WINDOWED_MODE"]["value"], 0)
        self.assertEqual(video["IDC_VSYNC_ENABLED"]["value"], 1)
        self.assertEqual(settings_lines(second), saved)
        # Evidence runs load the saved controls but do not apply saved video at startup.

    def test_vsync_can_be_disabled_after_enabling_it(self):
        settings = self.settings("vsync-off")
        disabled = run_viewer(self, self.directory, "vsync-off", self.cache, settings,
                              "--eawr-options-page", "video",
                              "--eawr-options-do", "set:IDC_VSYNC_ENABLED=1",
                              "--eawr-options-do", "press:IDC_OK",
                              "--eawr-options-do", "page:video",
                              "--eawr-options-do", "set:IDC_VSYNC_ENABLED=0",
                              "--eawr-options-do", "press:IDC_OK")
        self.assertEqual(settings_lines(disabled)["video.vsync"], "0")
        self.assertFalse(disabled["display"]["vsync"])
        self.assertEqual(disabled["display"]["vsync_mode"], 0)

    def test_cancel_and_defaults_follow_retail(self):
        settings = self.settings("cancel")
        cancelled = run_viewer(self, self.directory, "cancel", self.cache, settings,
                               "--eawr-options-page", "audio",
                               "--eawr-options-do", "set:IDC_SLIDER_MASTER_VOLUME=10",
                               "--eawr-options-do", "press:IDC_CANCEL")
        self.assertFalse(settings.exists(), "Cancel saves nothing")
        self.assertEqual(settings_lines(cancelled)["audio.master"], "75")
        self.assertEqual(cancelled["page"], "hub")

        defaulted = run_viewer(self, self.directory, "defaults", self.cache, settings,
                               "--eawr-options-page", "audio",
                               "--eawr-options-do", "set:IDC_SLIDER_MASTER_VOLUME=10",
                               "--eawr-options-do", "press:IDC_RESTORE_DEFAULT",
                               "--eawr-options-do", "snapshot")
        self.assertEqual(controls(defaulted["snapshots"][0])["IDC_SLIDER_MASTER_VOLUME"]["value"], 75)
        self.assertFalse(settings.exists(), "Defaults alone saves nothing until Accept")

    def test_sliders_reach_the_shared_buses_without_a_battle(self):
        # #3165: no battle exists in the options mode; a slider still moves the bus at once, Cancel
        # puts the saved level back.
        settings = self.settings("buses")
        moved = run_viewer(self, self.directory, "buses-moved", self.cache, settings,
                           "--eawr-options-page", "audio",
                           "--eawr-options-do", "set:IDC_SLIDER_MASTER_VOLUME=30",
                           "--eawr-options-do", "set:IDC_SLIDER_MUSIC_VOLUME=50")
        self.assertNotIn("audio: no battle buses yet", " ".join(moved["skipped"]))
        self.assertAlmostEqual(moved["buses"]["EAWR_Mix"], 20 * math.log10(0.30), delta=0.1)
        self.assertAlmostEqual(moved["buses"]["EAWR_Music"], 20 * math.log10(0.50), delta=0.1)
        cancelled = run_viewer(self, self.directory, "buses-cancel", self.cache, settings,
                               "--eawr-options-page", "audio",
                               "--eawr-options-do", "set:IDC_SLIDER_MASTER_VOLUME=10",
                               "--eawr-options-do", "press:IDC_CANCEL")
        self.assertAlmostEqual(cancelled["buses"]["EAWR_Mix"], 20 * math.log10(0.75), delta=0.1)

    def test_options_this_build_cannot_honour_are_disabled_with_a_reason(self):
        settings = self.settings("disabled")
        result = run_viewer(self, self.directory, "disabled", self.cache, settings,
                            "--eawr-options-page", "video",
                            "--eawr-options-do", "snapshot",
                            "--eawr-options-do", "page:gameplay",
                            "--eawr-options-do", "snapshot",
                            "--eawr-options-do", "page:keyboard",
                            "--eawr-options-do", "snapshot")
        video, gameplay, keyboard = (controls(snapshot) for snapshot in result["snapshots"])
        for honoured in ("IDC_RESOLUTION_LIST", "IDC_WINDOWED_MODE", "IDC_VSYNC_ENABLED", "IDC_ANTI_ALIAS_SLIDER"):
            self.assertTrue(video[honoured]["enabled"], honoured)
        for refused in ("IDC_GAMMA_SLIDER", "IDC_DETAIL_LEVEL_SLIDER", "IDC_WIDESCREEN"):
            self.assertFalse(video[refused]["enabled"], refused)
            self.assertTrue(video[refused]["reason"], refused)
        self.assertTrue(gameplay["IDC_GAME_SPEED_SLIDER"]["enabled"])
        self.assertFalse(gameplay["IDC_SCROLL_SPEED_SLIDER"]["enabled"])
        self.assertEqual(gameplay["IDC_GAME_SPEED_SLIDER"]["value"], 2)
        self.assertEqual(keyboard["IDC_EDIT_SELECT_ALL"]["text"], "Ctrl+A")
        self.assertFalse(keyboard["IDC_EDIT_SELECT_ALL"]["enabled"])

    def test_a_damaged_settings_file_loads_as_defaults_with_a_note(self):
        settings = self.settings("damaged")
        settings.write_text("eawr-settings 1\naudio.master=banana\nvideo.vsync=9\n\x00junk\n", encoding="utf-8")
        result = run_viewer(self, self.directory, "damaged", self.cache, settings, "--eawr-options-page", "audio",
                            "--eawr-options-do", "snapshot")
        self.assertTrue(result["settings_file"]["notes"])
        self.assertEqual(controls(result["snapshots"][0])["IDC_SLIDER_MASTER_VOLUME"]["value"], 75)

    def test_every_page_builds_and_the_in_game_hub_lists_network(self):
        for page in ("hub", "audio", "video", "advanced", "gameplay", "keyboard", "network"):
            settings = self.settings(f"page-{page}")
            result = run_viewer(self, self.directory, f"page-{page}", self.cache, settings,
                                "--eawr-options-page", page, "--eawr-options-context", "game")
            self.assertEqual(result["page"], page)
            self.assertTrue(result["controls"], page)
        hub = run_viewer(self, self.directory, "hub-main", self.cache, self.settings("hub-main"))
        self.assertNotIn("IDC_IN_GAME_OPTIONS_NETWORK", {control["id"] for control in hub["controls"]})
        # Retail shows Mods with the executable's built-in caption; it is disabled until the page exists.
        mods = controls(hub)["IDC_MAIN_OPTIONS_MODS"]
        self.assertTrue(mods["visible"])
        self.assertFalse(mods["enabled"])
        self.assertEqual(mods["text"], "Mods")
        # Keyboard Accept stays off: no key can change yet.
        keyboard = run_viewer(self, self.directory, "keyboard-accept", self.cache, self.settings("keyboard-accept"),
                              "--eawr-options-page", "keyboard")
        self.assertFalse(controls(keyboard)["IDC_APPLY"]["enabled"])
        game = run_viewer(self, self.directory, "hub-game", self.cache, self.settings("hub-game"),
                          "--eawr-options-context", "game")
        self.assertIn("IDC_IN_GAME_OPTIONS_NETWORK", {control["id"] for control in game["controls"]})


if __name__ == "__main__":
    unittest.main()
