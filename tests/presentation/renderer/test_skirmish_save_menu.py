"""The in-game menu's Save in a single-player skirmish (#3989, docs/checkpoint-format.md "Skirmish battle saves").

Source contracts always run: Save is enabled only for a battle that can be saved, it opens the retail Save
dialog, an existing name asks TEXT_SAVE_CONFIRM first, and the save is taken by the simulation thread and
written by a disk worker, never on the frame thread.

The GPU case needs the viewer lane and the installed game (EAWR_GODOT_VIEWER_RUNTIME_TEST,
EAWR_GODOT_EXECUTABLE, EAWR_EAW_GAME_ROOT): a skirmish started from the front end with the AI on, whose save
folder already holds one save. The menu's Save lists it; choosing it, Save and Yes replace it with this
battle, and the battle's report counts the written save.
"""

import json
import os
import pathlib
import shutil
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))

import test_front_end as front  # noqa: E402

RUNTIME = os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST")
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")
FIXTURE = ROOT / "tests/skirmish/fixtures/saves/tactical-v2-host.eawr-ckp"


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def body(text, opening):
    return text.split(opening, 1)[1].split("\n}\n", 1)[0]


class SkirmishSaveMenuContracts(unittest.TestCase):
    def test_failed_saved_battle_clears_front_end_origin(self):
        host = body(read("apps/viewer/src/viewer_host_process.cpp"), "void ViewerHost::start_saved_battle() {")
        failed = host.split("if (!map_mode_->ready(*skirmish_battle_host_)) {", 1)[1]
        self.assertLess(failed.index("saved_battle_from_front_end_ = false;"), failed.index("front_end_->return_to_main_menu()"))

    def test_save_is_enabled_only_for_a_battle_that_can_be_saved(self):
        screen = read("apps/viewer/src/options_screen.cpp")
        self.assertIn('{EawrOptionsScreen::Page::save_game, "save", "IDD_SAVE_GAME_DIALOG"}', screen)
        wired = body(screen, "void EawrOptionsScreen::wire_page() {")
        self.assertIn('bool(options_.save_game && options_.save_names)', wired)
        self.assertIn('bool(options_.load_game)', wired)
        self.assertIn('set_layer(EawrLoadGameScreen::layer_index - 1)', body(screen, 'void EawrOptionsScreen::open_load_game()'))
        self.assertIn('set_layer(screen_layer)', body(screen, 'void EawrOptionsScreen::on_load_closed()'))
        hud = read("apps/viewer/src/map_mode_hud.cpp")
        opened = body(hud, "bool MapMode::State::open_game_menu() {")
        self.assertIn("if (live_session->can_save()) {", opened)
        view = read("apps/viewer/src/live_session_save.cpp")
        can = body(view, "bool LiveSessionView::can_save() const noexcept {")
        for refusal in ("!setup_->campaign", "ai_scripts_ != nullptr", "!options_.lan", "!options_.borrowed_feed",
                        "!replay_", "!land_content_", "!observer_multiplayer()"):
            self.assertIn(refusal, can)

    def test_an_existing_name_asks_before_it_is_replaced(self):
        screen = read("apps/viewer/src/options_screen.cpp")
        named = body(screen, "void EawrOptionsScreen::save_named(")
        self.assertLess(named.index("ask_overwrite(*found);"), named.index("options_.save_game(name, overwrite)"))
        self.assertIn("nocasecmp_to", named)
        resized = body(screen, "void EawrOptionsScreen::on_viewport_resized() {")
        self.assertIn("if (confirm_ == Confirm::overwrite) ask_overwrite(overwrite_name_);", resized)
        self.assertIn('"TEXT_SAVE_CONFIRM"', body(screen, "void EawrOptionsScreen::ask_overwrite("))
        confirmed = body(screen, "void EawrOptionsScreen::on_confirm_result(")
        self.assertLess(confirmed.index("Confirm::overwrite"), confirmed.index("quit_requested_ = true;"))

    def test_the_file_is_written_off_the_frame_and_simulation_threads(self):
        view = read("apps/viewer/src/live_session_save.cpp")
        sink = body(view, "bool LiveSessionView::save_game(")
        self.assertIn("session_->request_save(", sink)
        self.assertNotIn("write_battle_save", sink)  # the sink only stores the bytes
        service = body(view, "bool LiveSessionView::service_save() {")
        self.assertIn("std::async(std::launch::async", service)
        self.assertLess(service.index("std::async(std::launch::async"), service.index("platform::write_battle_save"))
        session = read("src/platform/live_session.cpp")
        loop = body(session, "    void loop() {")
        # The request is taken under the loop's lock and served between two ticks, before a held (paused) loop waits.
        self.assertIn("requested = std::exchange(save_request_, {});", loop)
        self.assertLess(loop.index("if (requested) save_now(std::move(requested));"), loop.index("if (held) {"))


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires the viewer GPU lane and installed game data")
class SkirmishSaveMenuGpu(unittest.TestCase):
    def test_front_end_load_continue_returns_to_main_menu(self):
        with tempfile.TemporaryDirectory(prefix="skirmish-front-load-") as temporary:
            output = pathlib.Path(temporary).resolve()
            saves = output / "saves"
            saves.mkdir()
            shutil.copyfile(FIXTURE, saves / "Old Save.eawrsave")
            gestures = ("f5:click:hud=begin", "f40:click:hud=options", "f60:click:hud=menu_save",
                        "f80:click:hud=menu_save_first", "f100:click:hud=menu_save_button", "f120:click:hud=menu_yes")
            args = ["--eawr-front-end-test-battle", "--eawr-live-step", "1", "--eawr-live-ticks", "6000",
                    "--eawr-live-ai", "on", "--eawr-audio", "off", "--eawr-save-dir", str(saves)]
            for gesture in gestures:
                args += ["--eawr-live-input", gesture]
            saved, _ = front.run_front_end(output, "make-save", args, engine=("--render-thread", "separate", "--quit-after", "800"), timeout=420)
            self.assertIn("EAWR battle saved: Old Save at tick ", saved.stdout, saved.stdout[-6000:])
            loaded, _ = front.run_front_end(output, "front-load", [
                "--eawr-front-end-test-load-battle", "--eawr-save-dir", str(saves), "--eawr-audio", "off",
                "--eawr-live-step", "3", "--eawr-live-ticks", "300",
                "--eawr-live-order", "15:damage:1@1000000", "--eawr-live-order", "15:damage:2@1000000",
                "--eawr-live-input", "f5:click:hud=begin", "--eawr-live-input", "225:key:ENTER",
                "--eawr-live-input", "225:click:hud=quit"], engine=("--render-thread", "separate", "--quit-after", "20000"), timeout=420)
            self.assertEqual(loaded.returncode, 0, loaded.stdout[-6000:])
            self.assertIn("EAWR skirmish save loaded at tick ", loaded.stdout, loaded.stdout[-6000:])
            self.assertIn("EAWR front end returned to the main menu", loaded.stdout, loaded.stdout[-6000:])
            self.assertIn("EAWR front end battle returned: main_menu; setup visible false", loaded.stdout, loaded.stdout[-6000:])
            # Change the header's mounted-content identity while preserving the payload checksum.
            # This models a save copied from another data set and must be refused before a battle starts.
            save_path = saves / "Old Save.eawrsave"
            foreign = bytearray(save_path.read_bytes())
            foreign[88] ^= 1
            save_path.write_bytes(foreign)
            refused, _ = front.run_front_end(output, "foreign-load", [
                "--eawr-front-end-test-load-battle", "--eawr-save-dir", str(saves), "--eawr-audio", "off"],
                engine=("--render-thread", "separate", "--quit-after", "800"), timeout=420)
            self.assertIn("EAWR front end saved game restore refused:", refused.stdout, refused.stdout[-6000:])
            self.assertNotIn("EAWR skirmish battle restored from", refused.stdout)
            battle, _ = front.run_front_end(output, "foreign-in-game", [
                "--eawr-front-end-test-battle", "--eawr-save-dir", str(saves), "--eawr-audio", "off",
                "--eawr-live-step", "1", "--eawr-live-ticks", "6000", "--eawr-live-ai", "on",
                "--eawr-live-input", "f5:click:hud=begin", "--eawr-live-input", "f40:click:hud=options",
                "--eawr-live-input", "f60:click:hud=menu_load", "--eawr-live-input", "f80:click:hud=menu_load_first",
                "--eawr-live-input", "f100:click:hud=menu_load_button"], engine=("--render-thread", "separate", "--quit-after", "800"), timeout=420)
            self.assertIn("EAWR saved game load refused:", battle.stdout, battle.stdout[-6000:])
            self.assertNotIn("EAWR skirmish battle restored from", battle.stdout)

    def test_menu_load_restarts_the_saved_battle(self):
        gestures = (
            "f5:click:hud=begin", "f40:click:hud=options", "f60:click:hud=menu_save",
            "f80:click:hud=menu_save_first", "f100:click:hud=menu_save_button", "f120:click:hud=menu_yes",
            "f300:click:hud=options", "f320:click:hud=menu_load", "f340:click:hud=menu_load_first",
            "f360:click:hud=menu_load_button")
        with tempfile.TemporaryDirectory(prefix="skirmish-save-load-") as temporary:
            output = pathlib.Path(temporary).resolve()
            saves = output / "saves"
            saves.mkdir()
            shutil.copyfile(FIXTURE, saves / "Old Save.eawrsave")
            arguments = ["--eawr-front-end-test-battle", "--eawr-live-step", "1", "--eawr-live-ticks", "6000",
                         "--eawr-live-ai", "on", "--eawr-audio", "off", "--eawr-save-dir", str(saves)]
            for gesture in gestures:
                arguments += ["--eawr-live-input", gesture]
            result, _ = front.run_front_end(output, "save-load", arguments, engine=("--render-thread", "separate", "--quit-after", "1800"), timeout=420)
            log = result.stdout
            self.assertEqual(result.returncode, 0, log[-6000:])
            self.assertIn("EAWR battle saved: Old Save at tick ", log, log[-6000:])
            self.assertIn("EAWR skirmish save loaded at tick ", log, log[-6000:])
            self.assertIn("EAWR skirmish battle restored from ", log, log[-6000:])

    def test_menu_save_replaces_a_listed_save_with_this_battle(self):
        # Begin, open the menu with the HUD button, Save, pick the listed save, Save, Yes; later quit to the menu.
        gestures = (
            "f5:click:hud=begin", "f40:click:hud=options", "f60:click:hud=menu_save", "f80:click:hud=menu_save_first",
            "f100:click:hud=menu_save_button", "f120:click:hud=menu_yes",
            "f300:click:hud=options", "f320:click:hud=menu_quit", "f340:click:hud=menu_yes")
        with tempfile.TemporaryDirectory(prefix="skirmish-save-menu-") as temporary:
            output = pathlib.Path(temporary).resolve()
            saves = output / "saves"
            saves.mkdir()
            listed = saves / "Old Save.eawrsave"
            shutil.copyfile(FIXTURE, listed)
            report = output / "battle.json"
            arguments = ["--eawr-front-end-test-battle", "--eawr-report", str(report), "--eawr-live-step", "1",
                         "--eawr-live-ticks", "6000", "--eawr-live-ai", "on", "--eawr-audio", "off",
                         "--eawr-save-dir", str(saves)]
            for gesture in gestures:
                arguments += ["--eawr-live-input", gesture]
            result, _ = front.run_front_end(output, "save-menu", arguments, engine=("--render-thread", "separate", "--quit-after", "6000"), timeout=420)
            log = result.stdout
            keep = os.environ.get("EAWR_SKIRMISH_SAVE_MENU_EYECHECK")
            if keep:
                shutil.copytree(output, pathlib.Path(keep) / output.name, dirs_exist_ok=True)
            self.assertEqual(result.returncode, 0, log[-6000:])
            self.assertIn("EAWR battle save requested: Old Save; started true", log, log[-6000:])
            self.assertIn("EAWR battle saved: Old Save at tick ", log, log[-6000:])
            self.assertNotIn("EAWR battle save failed", log)
            written = listed.read_bytes()
            self.assertNotEqual(written, FIXTURE.read_bytes(), "the listed save was replaced by this battle")
            self.assertEqual(written[:7], b"EAWRCKP")
            self.assertEqual(sorted(path.name for path in saves.iterdir()), ["Old Save.eawrsave"])
            if report.is_file():
                saved = json.loads(report.read_text(encoding="utf-8"))
                status = saved.get("live_session", {}).get("player_saves")
                if status is not None:
                    self.assertEqual((status["written"], status["failed"], status["last_name"]), (1, 0, "Old Save"))
            print("SKIRMISH-SAVE-MENU,bytes=%d,replaced=True" % len(written))


if __name__ == "__main__":
    unittest.main()
