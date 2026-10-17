"""The in-game menu (#3925, MNU-19, docs/ui/menu-screens.md): Escape and the battle HUD's options button
open it in a skirmish, Options and Quit Game work in the campaign too, and Quit Game frees the match and
shows the front end's main menu.

Source contracts always run. The two GPU cases need the viewer lane and the installed game
(EAWR_GODOT_VIEWER_RUNTIME_TEST, EAWR_GODOT_EXECUTABLE, EAWR_EAW_GAME_ROOT): a skirmish battle started
from the front end, and a custom campaign started from the front end.
"""

import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))

import test_front_end as front  # noqa: E402

SRC = ROOT / "apps/viewer/src"
RUNTIME = os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST")
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


class InGameMenuContracts(unittest.TestCase):
    def test_the_options_screen_has_the_retail_menu_page(self):
        screen = read("apps/viewer/src/options_screen.cpp")
        self.assertIn('"IDD_GAME_OPTIONS_DIALOG"', screen)
        # Quit Game asks TEXT_QUIT_SURE (Yes / No) before it closes the screen with quit_requested().
        ask = screen.split("void EawrOptionsScreen::ask_quit() {", 1)[1].split("\n}\n", 1)[0]
        self.assertIn('"TEXT_QUIT_SURE"', ask)
        self.assertIn("BoxKind::quit_confirm", ask)
        confirmed = screen.split("void EawrOptionsScreen::on_confirm_result(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("quit_requested_ = true;", confirmed)
        # The options hub opened from the menu returns to it; Esc on the menu is Resume.
        leave = screen.split("void EawrOptionsScreen::leave_hub() {", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("Page::game_menu", leave)
        self.assertIn("if (page_ == Page::game_menu) close_screen();", screen)

    def test_escape_and_the_options_button_open_it_in_a_battle(self):
        live = read("apps/viewer/src/map_mode_live.cpp")
        # Escape that a targeting mode did not use opens the menu, after the battle input saw it.
        self.assertLess(live.index("state.battle->input(event, *state.live_session"),
                        live.index("state.open_game_menu()"))
        hud = read("apps/viewer/src/map_mode_hud.cpp")
        self.assertIn("hud->set_options_handler([this] { static_cast<void>(open_game_menu()); });", hud)
        opened = hud.split("bool MapMode::State::open_game_menu() {", 1)[1].split("\n}\n", 1)[0]
        # UI-S1: the menu pauses the battle, except where a pause is not the player's (multiplayer).
        self.assertIn("!options.lan && !live_session->time().paused()", opened)
        self.assertIn("press_pause()", opened)
        self.assertIn("EawrOptionsScreen::Page::game_menu", opened)
        build = read("src/presentation/godot/ui/tactical_hud_build.cpp")
        self.assertIn("options_handler", build.split("state.options_button->setup_art", 1)[1].split("}\n", 1)[0])

    def test_quit_game_frees_the_match_and_shows_the_main_menu(self):
        host = read("apps/viewer/src/viewer_host_process.cpp")
        self.assertIn("map_mode_->leave_to_menu()", host)
        self.assertIn("front_end_->return_to_main_menu()", host)
        # The campaign score's Main Menu used to end the whole viewer.
        self.assertNotIn("no main menu in this build", host)
        leave = host.split("void ViewerHost::leave_campaign_to_menu() {", 1)[1].split("\n}\n", 1)[0]
        self.assertLess(leave.index("galactic_mode_.reset();"), leave.index("memdelete(galactic_host_);"))
        self.assertLess(leave.index("memdelete(galactic_host_);"), leave.index("front_end_->return_to_main_menu()"))
        # The menu Quit records the same intentional departure as the HUD quit.
        session = read("apps/viewer/src/live_session_frame.cpp")
        left = session.split("bool LiveSessionView::leave() {", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("quit();", left)
        front_end = read("apps/viewer/src/front_end_mode.cpp")
        back = front_end.split("void FrontEndMode::return_to_main_menu() {", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("state.nav.reset();", back)
        self.assertIn("campaign_started = false;", back)
        ready = read("apps/viewer/src/viewer_host_ready.cpp")
        self.assertIn("galactic_host_ = campaign_host;", ready)

    def test_the_campaign_menu_options_and_quit_are_wired(self):
        widget = read("apps/viewer/src/galactic/save_dialog_widget.cpp")
        self.assertIn('id == "IDC_INGAME_OPTIONS") action(control, [this] { open_options(); });', widget)
        self.assertIn('Modal::quit, localized("TEXT_QUIT_SURE")', widget)
        self.assertIn("previous == Modal::quit && yes", widget)
        save = read("apps/viewer/src/galactic/galactic_save.cpp")
        self.assertNotIn('name == "IDC_INGAME_OPTIONS" || name == "IDC_GAME_OPTIONS_QUIT") disabled = true;', save)
        mode = read("apps/viewer/src/galactic/galactic_mode.cpp")
        self.assertIn("state.leave_requested", mode)
        handoff = read("apps/viewer/src/galactic/galactic_handoff.cpp")
        self.assertEqual(handoff.count("tactical->set_menu_handler("), 2)


DRIVER = '''extends SceneTree
var elapsed = 0.0
var beat = 0.0
var phase = 0
var instance
var opened_ok = false
var wait_for_battle = BATTLE
var trace = {"error": "", "steps": []}
func _initialize():
    instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func key(code):
    for pressed in [true, false]:
        var event = InputEventKey.new()
        event.keycode = code
        event.pressed = pressed
        Input.parse_input_event(event)
func click(node):
    var point = node.get_global_rect().get_center()
    for pressed in [true, false]:
        var event = InputEventMouseButton.new()
        event.position = point
        event.global_position = point
        event.button_index = MOUSE_BUTTON_LEFT
        event.pressed = pressed
        Input.parse_input_event(event)
func visible_node(name):
    var node = instance.find_child(name, true, false)
    if node == null or not node.is_visible_in_tree(): return null
    return node
func button(name):
    var node = visible_node(name)
    if node == null or node.disabled: return false
    click(node)
    return true
func modal_open(menu):
    var modal = null if menu == null else menu.find_child("SaveModal", true, false)
    return modal != null and modal.is_visible_in_tree()
func step(name):
    trace["steps"].append(name)
func finish(error):
    trace["error"] = error
    trace["phase"] = phase
    var out = FileAccess.open(TRACE, FileAccess.WRITE)
    out.store_string(JSON.stringify(trace))
    quit(0 if error == "" else 2)
func _process(delta):
    elapsed += delta
    beat += delta
    if elapsed > 900:
        finish("in-game menu driver timed out at phase " + str(phase))
        return false
    if beat < 0.4: return false
    beat = 0
    var menu = instance.find_child("GalacticSaveMenu", true, false)
    var escape = null if menu == null else menu.find_child("IDD_GAME_OPTIONS_DIALOG", true, false)
    match phase:
        0:
            # The campaign is up once it takes Escape; a refused open (loading) is tried again. In a space
            # battle Escape is only tried once the battle's map has been handed over.
            if wait_for_battle and instance.find_child("CampaignSpaceBattleReady", true, false) == null:
                return false
            if menu != null and menu.is_visible_in_tree() and escape != null and escape.is_visible_in_tree():
                step("escape_menu"); phase = 1
                # Options and Quit Game are live in the dialog (they used to be forced disabled).
                var options_button = escape.find_child("IDC_INGAME_OPTIONS", true, false)
                var quit_button = escape.find_child("IDC_GAME_OPTIONS_QUIT", true, false)
                if options_button == null or quit_button == null or options_button.disabled or quit_button.disabled:
                    finish("Options or Quit Game is disabled in the menu"); return false
                step("controls_enabled")
            else:
                key(KEY_ESCAPE)
        1:
            if button("IDC_INGAME_OPTIONS"): phase = 2
        2:
            if visible_node("IDD_IN_GAME_OPTIONS_DIALOG") != null:
                if escape.is_visible_in_tree(): finish("the menu stayed visible under the options"); return false
                step("options_hub"); phase = 3
        3:
            if button("IDC_OK_IN_GAME"): phase = 4
        4:
            if visible_node("IDD_IN_GAME_OPTIONS_DIALOG") == null and escape.is_visible_in_tree():
                step("back_in_menu"); phase = 5
        5:
            if button("IDC_GAME_OPTIONS_QUIT"): phase = 6
        6:
            if modal_open(menu): step("quit_confirm"); phase = 7
        7:
            if button("IDC_DIALOG_BOX_BUTTON_RIGHT"): phase = 8
        8:
            if not modal_open(menu) and escape.is_visible_in_tree():
                step("quit_declined"); phase = 9
        9:
            if button("IDC_GAME_OPTIONS_QUIT"): phase = 10
        10:
            if modal_open(menu): phase = 11
        11:
            if button("IDC_DIALOG_BOX_BUTTON_LEFT"): step("quit_accepted"); phase = 12
        12:
            # The campaign is freed and the front end shows its main menu again.
            if visible_node("IDC_MAIN_MENU_EXIT2") != null:
                if instance.find_child("GalacticSaveMenu", true, false) != null:
                    finish("the campaign's menu node outlived the campaign"); return false
                step("main_menu"); phase = 13; elapsed = 0.0
        13:
            # Let the freed campaign's deferred deletions run, then the shell must still answer.
            if elapsed > 2.0:
                if visible_node("IDC_MAIN_MENU_EXIT2") == null: finish("the main menu went away"); return false
                finish("")
    return false
'''


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires the viewer GPU lane and installed game data")
class InGameMenuGpu(unittest.TestCase):
    def test_skirmish_escape_and_button_open_the_menu_and_quit_reaches_the_main_menu(self):
        # Start from the single-player menu, begin the battle, open the menu with the HUD's button, close
        # it with Escape, open it with Escape, walk Options and back, answer No and then Yes to Quit Game.
        # The battle stays paused behind the menu, so the gestures are on frames.
        gestures = (
            "f5:click:hud=begin", "f40:click:hud=options", "f60:key:ESCAPE", "f80:key:ESCAPE",
            "f100:click:hud=menu_options", "f120:click:hud=menu_back", "f140:click:hud=menu_quit",
            "f160:click:hud=menu_no", "f180:click:hud=menu_quit", "f200:click:hud=menu_yes")
        with tempfile.TemporaryDirectory(prefix="ingame-menu-") as temporary:
            output = pathlib.Path(temporary).resolve()
            report = output / "battle.json"
            arguments = ["--eawr-front-end-test-battle", "--eawr-report", str(report), "--eawr-live-step", "1",
                         "--eawr-live-ticks", "6000", "--eawr-live-ai", "off", "--eawr-audio", "off"]
            for gesture in gestures:
                arguments += ["--eawr-live-input", gesture]
            result, _ = front.run_front_end(output, "menu", arguments, engine=("--quit-after", "6000"), timeout=420)
            log = result.stdout
            self.assertEqual(result.returncode, 0, log[-6000:])
            self.assertEqual(log.count("EAWR in-game menu opened: paused true; by menu true"), 2, log[-6000:])
            self.assertEqual(log.count("EAWR in-game menu closed: resumed true; quit false"), 1, log[-6000:])
            self.assertEqual(log.count("EAWR in-game menu closed: resumed true; quit true"), 1, log[-6000:])
            self.assertLess(log.index("EAWR in-game menu closed: resumed true; quit true"),
                            log.index("EAWR front end returned to the main menu"))
            self.assertRegex(log, r"EAWR front end battle returned: main_menu; setup visible false")
            self.assertIn("EAWR front end quit", log)
            self.assertNotIn("EAWR front end test:", log)
            self.assertNotIn("its target is not on screen", log)

    def test_campaign_menu_options_quit_and_main_menu(self):
        self.walk_campaign_menu("galaxy", ["--eawr-front-end-test-campaign", "start"])

    def test_space_battle_menu_options_quit_and_main_menu(self):
        # The Gateways route (Rebel Bespin fleet into Imperial Byss) played through the ordinary Move and
        # Fight commands; the menu is opened in the space battle the campaign hands over.
        self.walk_campaign_menu("space", ["--eawr-front-end-test-campaign", "tech:Sandbox_Gateways_Rebel",
                                          "--eawr-galactic-paused", "no", "--eawr-galactic-space-proof", "save"])

    def walk_campaign_menu(self, where, campaign_arguments):
        with tempfile.TemporaryDirectory(prefix="ingame-menu-gc-") as temporary:
            directory = pathlib.Path(temporary).resolve()
            trace = directory / "trace.json"
            driver = directory / "driver.gd"
            driver.write_text(DRIVER.replace("TRACE", json.dumps(str(trace).replace("\\", "/")))
                              .replace("BATTLE", "true" if where == "space" else "false"), encoding="utf-8")
            font_cache = directory / "fonts"
            extracted = subprocess.run([
                sys.executable, str(ROOT / "tools/fonts/extract_eaw_fonts.py"), "--game-root", GAME_ROOT,
                "--out", str(font_cache), "--allow-unknown-build", "--json"], capture_output=True, text=True, timeout=120)
            self.assertEqual(extracted.returncode, 0, extracted.stdout + extracted.stderr)
            result = subprocess.run([
                os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                "--path", str(ROOT / "apps/viewer/project"), "--script", str(driver), "--",
                "--eawr-front-end", "--eawr-front-end-test", *campaign_arguments,
                "--eawr-game-root", GAME_ROOT, "--eawr-font-cache", str(font_cache), "--eawr-audio", "off",
                "--eawr-attract", "off", "--eawr-lighting", "sh", "--eawr-save-dir", str(directory / "saves")],
                cwd=ROOT, capture_output=True, text=True, timeout=600)
            (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
            self.assertTrue(trace.is_file(), result.stdout[-6000:] + result.stderr[-2000:])
            data = json.loads(trace.read_text(encoding="utf-8"))
            self.assertEqual(data["error"], "", data)
            self.assertEqual(data["steps"], ["escape_menu", "controls_enabled", "options_hub", "back_in_menu",
                                             "quit_confirm", "quit_declined", "quit_accepted", "main_menu"], data)
            self.assertIn("EAWR campaign left for the main menu", result.stdout)
            self.assertIn("EAWR front end returned to the main menu", result.stdout)


if __name__ == "__main__":
    unittest.main()
