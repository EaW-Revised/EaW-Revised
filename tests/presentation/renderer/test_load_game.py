"""The Load Game screen (MNU-39 and MNU-41, docs/ui/menu-screens.md).

`--eawr-load-game script` opens EawrLoadGameScreen on the gallery's retail skin over a replay store and
scripts input through Godot's real dispatch: with an empty save directory the saved-game list is empty
with Load and Delete off, the recordings check lists the store, a click selects one, Delete asks for
confirmation and No keeps the file, and Load (Play) chooses the recording; the host then starts the
replay mode with it, which writes its own report beside the gallery's.

`--eawr-load-game saves` does the same over fixture saves in a temporary save directory (#2942): the
rows come in retail order with their dates, an unreadable save is listed and cannot load, Delete asks and
No keeps the file, Yes removes the file and its row, and Load opens the Loading box and hands the save to
the launcher stand-in (`--eawr-load-launcher accept`: the screen closes with the request; `refuse`: the
reason is shown over the open screen; `none`: Load stays off with its stated reason).

The engine-free half (header summaries, sort, enable rules and a delete scoped to the store; the saved-game
rows, their order and the directory store) is the root CTests `ui_load_game_contracts` and
`ui_saved_games_contracts`.

The structural half runs everywhere. The graphical half is opt-in: set
EAWR_GODOT_VIEWER_RUNTIME_TEST, EAWR_GODOT_EXECUTABLE and EAWR_EAW_GAME_ROOT.
"""

import hashlib
import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
from viewer_mode_sources import source_text  # noqa: E402
import test_ui_gallery  # noqa: E402

RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")
TOOL = ROOT / "tools/fonts/extract_eaw_fonts.py"
FIXTURES = ROOT / "tests/skirmish/fixtures"

# An empty checkpoint container (src/checkpoint/checkpoint.cpp): the save list reads only its header and
# payload digest, so this is a readable save with no sections. The C++ test ui_saved_games_contracts
# holds the same bytes for tick 90 (fixture_layout), so a layout change fails there first.
CHECKPOINT_MAGIC = b"EAWRCKP\0"
CHECKPOINT_VERSION = 1
FLAG_GALAXY = 2
CHECKPOINT_BYTES = 304


def checkpoint_fixture(tick, flags=FLAG_GALAXY):
    payload = b""
    out = CHECKPOINT_MAGIC + struct.pack("<HH", CHECKPOINT_VERSION, flags)
    out += struct.pack("<III", 0, 0, 0) + bytes(32 * 5)  # rules, state encoding, reserved; five identities (all zero)
    out += struct.pack("<QQQ", 0, tick, tick // 3)  # seed, completed tick, galactic frame
    out += struct.pack("<QIIQ", 0, 0, 0, 0) + bytes(32)  # frontier tick, seat, section count, sequence, digest
    out += struct.pack("<Q", len(payload)) + hashlib.sha256(payload).digest()
    return out


class LoadGameStructure(unittest.TestCase):
    def test_the_screen_is_registered_and_built(self):
        register = (ROOT / "apps/viewer/src/register_types.cpp").read_text(encoding="utf-8")
        self.assertIn("register_load_game_classes()", register)
        build = source_text("apps/viewer/CMakeLists.txt")
        self.assertIn("src/ui_gallery_load_game.cpp", build)
        self.assertIn("src/presentation/godot/ui/load_game_view.cpp", build)
        sources = (ROOT / "src/presentation/ui/sources.cmake").read_text(encoding="utf-8")
        self.assertIn("load_game.cpp", sources)

    def test_the_screen_is_built_from_the_catalogue_dialog_and_asks_before_deleting(self):
        view = (ROOT / "src/presentation/godot/ui/load_game_view.cpp").read_text(encoding="utf-8")
        self.assertIn("IDD_LOAD_GAME_DIALOG", view)
        self.assertIn("build_dialog(", view)
        self.assertIn("delete_confirmation()", view)
        self.assertIn("EawrMessageBox", view)
        self.assertIn('ADD_SIGNAL(MethodInfo("replay_chosen"', view)
        self.assertIn('ADD_SIGNAL(MethodInfo("closed")', view)
        self.assertIn("KEY_ESCAPE", view)

    def test_the_host_hook_is_the_standalone_flag_and_the_replay_seam(self):
        mode = source_text("apps/viewer/src/ui_gallery_mode.cpp")
        self.assertIn("--eawr-load-game", mode)
        self.assertIn("--eawr-replay-store", mode)
        process = (ROOT / "apps/viewer/src/viewer_host_process.cpp").read_text(encoding="utf-8")
        self.assertIn("take_replay_request()", process)
        self.assertIn(".replay_input = request.path", process)
        # A refused recording keeps the viewer up: no quit on that path, the page reopens with the reason.
        self.assertIn("options.notice", process)
        ready = (ROOT / "apps/viewer/src/map_mode_ready.cpp").read_text(encoding="utf-8")
        self.assertIn('live_options.fixture = "replay"', ready)

    def test_the_model_stays_engine_free(self):
        header = (ROOT / "include/eawr/presentation/ui/load_game.hpp").read_text(encoding="utf-8")
        self.assertNotIn("godot", header)

    def test_the_save_directory_feeds_the_list_through_the_store_seam(self):
        build = source_text("apps/viewer/CMakeLists.txt")
        self.assertIn("src/saved_games.cpp", build)
        view = (ROOT / "src/presentation/godot/ui/load_game_view.cpp").read_text(encoding="utf-8")
        self.assertIn('ADD_SIGNAL(MethodInfo("save_chosen"', view)
        self.assertIn("TEXT_SAVELOAD_LOADING_MSG", view)
        adapter = (ROOT / "apps/viewer/src/saved_games.cpp").read_text(encoding="utf-8")
        # The retail order key is the in-game dialog's, not a second copy.
        self.assertIn("galactic/save_dialog_model.hpp", adapter)
        self.assertIn("save_list_key(", adapter)
        # The list and delete go through the platform's session-free wrapper, not a second repository reader.
        self.assertIn("eawr/platform/saved_games.hpp", adapter)
        self.assertIn("list_saved_games(", adapter)
        self.assertIn("delete_saved_game(", adapter)
        platform = (ROOT / "src/platform/galactic/sources.cmake").read_text(encoding="utf-8")
        self.assertIn("saved_games.cpp", platform)
        header = (ROOT / "include/eawr/presentation/ui/load_game.hpp").read_text(encoding="utf-8")
        self.assertIn("class SavedGameStore", header)
        self.assertIn("struct SavedGameRequest", header)
        self.assertNotIn("no player save format", header + view)
        mode = source_text("apps/viewer/src/ui_gallery_mode.cpp")
        self.assertIn("--eawr-save-dir", mode)
        self.assertIn("--eawr-load-launcher", mode)

    def test_the_fixture_checkpoint_is_one_header(self):
        data = checkpoint_fixture(90)
        self.assertEqual(len(data), CHECKPOINT_BYTES)
        self.assertEqual(data[:8], CHECKPOINT_MAGIC)


@unittest.skipUnless(RUNTIME and GAME_ROOT,
                     "set EAWR_GODOT_VIEWER_RUNTIME_TEST, EAWR_GODOT_EXECUTABLE and EAWR_EAW_GAME_ROOT")
class LoadGameGraphical(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-load-game-"))
        cls.cache = pathlib.Path(tempfile.mkdtemp(prefix="eawr-font-cache-"))
        extracted = subprocess.run([sys.executable, str(TOOL), "--game-root", GAME_ROOT, "--out", str(cls.cache),
                                    "--allow-unknown-build", "--json"],
                                   capture_output=True, text=True, timeout=120, check=False)
        if extracted.returncode != 0:
            raise RuntimeError(extracted.stderr)
        # Two fixture replays in a store of their own; the names put the one with a map record first.
        cls.store = cls.directory / "replays"
        cls.store.mkdir()
        shutil.copyfile(FIXTURES / "m2-start.eawr-replay", cls.store / "alpha-m2-start.eawr-replay")
        shutil.copyfile(FIXTURES / "installed" / "banked-hardpoints.eawr-replay",
                        cls.store / "beta-banked-hardpoints.eawr-replay")

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.cache, ignore_errors=True)

    def test_toggle_recordings_select_one_and_play_starts_the_replay_mode(self):
        # An empty save directory of its own: the machine's saves must not change the run.
        empty_saves = self.directory / "empty-saves"
        empty_saves.mkdir()
        result = test_ui_gallery.run_viewer(self, self.directory, "load-game", self.cache,
                                            "--eawr-load-game", "script", "--eawr-replay-store", str(self.store),
                                            "--eawr-save-dir", str(empty_saves))
        page = result["load_game"]
        failed = [f"{step['name']}: {step['problem']}" for step in page["steps"] if not step["passed"]]
        self.assertEqual(failed, [], "\n".join(failed))
        self.assertEqual(len(page["steps"]), 6, page["steps"])
        self.assertTrue(page["play"]["path"].endswith("alpha-m2-start.eawr-replay"), page["play"])
        self.assertTrue(page["play"]["map"].endswith(".ted"), page["play"])
        self.assertEqual(len(list(self.store.glob("*.eawr-replay"))), 2, "No must keep both files")
        # Play handed over to the replay mode, which wrote its own report.
        replay_report = self.directory / "load-game.replay.json"
        self.assertTrue(replay_report.is_file(), "the replay mode did not run")
        replay = json.loads(replay_report.read_text(encoding="utf-8"))
        self.assertEqual(replay["live_session"]["fixture"], "replay")

    def test_fixture_save_replay_overwrite_list_and_play(self):
        # UI/store coverage with the pinned skirmish-start recording and multiplayer button
        # visibility. The finished production serializer is checked by tactical_live_session_contracts.
        store = self.directory / "saved-\U0001f600-\u03a3" / "replays"
        saves = self.directory / "save-replay-empty-saves"
        saves.mkdir()
        fixture = FIXTURES / "m2-start.eawr-replay"
        result = test_ui_gallery.run_viewer(self, self.directory, "save-replay", self.cache,
            "--eawr-load-game", "save-replay", "--eawr-save-replay-fixture", str(fixture),
            "--eawr-replay-store", str(store), "--eawr-save-dir", str(saves))
        page = result["load_game"]
        failed = [f"{step['name']}: {step['problem']}" for step in page["steps"] if not step["passed"]]
        self.assertEqual(failed, [], "\n".join(failed))
        self.assertEqual(len(page["steps"]), 15, page["steps"])
        saved = store / "Saved battle-\U0001f600-\u03a3.eawr-replay"
        self.assertEqual(saved.read_bytes(), fixture.read_bytes())
        self.assertEqual(pathlib.Path(page["play"]["path"]), saved)
        replay_report = self.directory / "save-replay.replay.json"
        self.assertTrue(replay_report.is_file(), "the saved recording was not played")
        replay = json.loads(replay_report.read_text(encoding="utf-8"))
        self.assertEqual(replay["live_session"]["fixture"], "replay")


@unittest.skipUnless(RUNTIME and GAME_ROOT,
                     "set EAWR_GODOT_VIEWER_RUNTIME_TEST, EAWR_GODOT_EXECUTABLE and EAWR_EAW_GAME_ROOT")
class LoadGameSavedGamesGraphical(unittest.TestCase):
    """One case: the saved-game list, Delete with its confirmation and the Load hand-off (#2942)."""

    # name, hours since the write, readable. The expected order is the in-game dialog's: the auto save,
    # the quick save, then the newest write first.
    SAVES = [("Auto Save", 30, True), ("Quick Save", 2, True), ("Campaign One", 1, True),
             ("Damaged", 3, False), ("Campaign Two", 5, True), ("Old Campaign", 48, True)]
    ORDER = ["Auto Save", "Quick Save", "Campaign One", "Damaged", "Campaign Two", "Old Campaign"]

    @classmethod
    def setUpClass(cls):
        cls.directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-load-saves-"))
        cls.cache = pathlib.Path(tempfile.mkdtemp(prefix="eawr-font-cache-"))
        extracted = subprocess.run([sys.executable, str(TOOL), "--game-root", GAME_ROOT, "--out", str(cls.cache),
                                    "--allow-unknown-build", "--json"],
                                   capture_output=True, text=True, timeout=120, check=False)
        if extracted.returncode != 0:
            raise RuntimeError(extracted.stderr)
        # The recordings store is not under test here, and an empty one keeps the run to the saved games.
        cls.store = cls.directory / "replays"
        cls.store.mkdir()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.cache, ignore_errors=True)

    def make_saves(self, name):
        saves = self.directory / name
        saves.mkdir()
        now = time.time()
        for index, (save, hours, readable) in enumerate(self.SAVES):
            path = saves / f"{save}.eawrsave"
            path.write_bytes(checkpoint_fixture(30 * (index + 1)) if readable else b"not a checkpoint at all")
            os.utime(path, (now - hours * 3600, now - hours * 3600))
        # Files the list must skip: another extension, a name the save rule refuses.
        (saves / "notes.txt").write_text("not a save", encoding="utf-8")
        (saves / "bad.name.eawrsave").write_bytes(checkpoint_fixture(7))
        return saves

    def run_saves(self, name, launcher):
        saves = self.make_saves(name)
        result = test_ui_gallery.run_viewer(self, self.directory, name, self.cache, "--eawr-load-game", "saves",
                                            "--eawr-replay-store", str(self.store), "--eawr-save-dir", str(saves),
                                            "--eawr-load-launcher", launcher)
        page = result["load_game"]
        failed = [f"{step['name']}: {step['problem']}" for step in page["steps"] if not step["passed"]]
        self.assertEqual(failed, [], "\n".join(failed))
        self.assertEqual([row["name"] for row in page["rows"]], self.ORDER, page["rows"])
        for row in page["rows"]:
            self.assertTrue(row["text"], row)
            self.assertTrue(row["date"], row)
            self.assertEqual(row["playable"], row["name"] != "Damaged", row)
        by_name = {row["name"]: row for row in page["rows"]}
        self.assertTrue(by_name["Damaged"]["problem"], "an unreadable save says why")
        # The reserved slots show the text database's label, not the key (a missing key shows as written).
        self.assertNotIn("TEXT_GUI_AUTO_SAVE", by_name["Auto Save"]["text"])
        self.assertNotIn("TEXT_GUI_QUICK_SAVE", by_name["Quick Save"]["text"])
        self.assertEqual(by_name["Campaign One"]["text"], "Campaign One")
        # Delete answered Yes removed the last readable save from disk; every other file stayed.
        self.assertFalse((saves / "Old Campaign.eawrsave").exists())
        self.assertEqual(len(list(saves.glob("*.eawrsave"))), len(self.SAVES))  # one deleted, plus the skipped bad.name file
        self.assertTrue((saves / "notes.txt").exists())
        return page

    def test_list_delete_and_load_hand_off(self):
        accepted = self.run_saves("load-saves-accept", "accept")
        self.assertEqual(len(accepted["steps"]), 10, accepted["steps"])
        self.assertEqual(accepted["launches"], 1)
        self.assertGreaterEqual(accepted["loading_boxes"], 1)
        # The first readable row is the auto save: the hand-off names its stored file name and the directory.
        self.assertEqual(accepted["save"]["name"], "Auto Save")
        self.assertEqual(pathlib.Path(accepted["save"]["directory"]), pathlib.Path(accepted["saves"]))
        self.assertIsNone(accepted["play"])

        refused = self.run_saves("load-saves-refuse", "refuse")
        self.assertEqual(len(refused["steps"]), 10, refused["steps"])
        self.assertEqual(refused["launches"], 1)
        self.assertIsNone(refused["save"], "a refused restore chooses nothing")

        off = self.run_saves("load-saves-none", "none")
        self.assertEqual(len(off["steps"]), 7, off["steps"])
        self.assertEqual(off["launches"], 0)
        self.assertIsNone(off["save"])


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires GPU viewer and installed FoC")
class FrontEndSavedGameGraphical(unittest.TestCase):
    def test_front_end_restores_unicode_save_after_visible_refusal(self):
        import test_viewer_galactic_save_runtime as gc
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-menu-restore-"))
        saves = directory / "\u0391\u03b8\u03ae\u03bd\u03b1-\U0001f600"
        trace = json.dumps(str(directory / "trace.json").replace("\\", "/"))
        common = '''extends SceneTree
var elapsed = 0.0
var beat = 0.0
var phase = 0
var instance
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
func click(node, point = Vector2.ZERO):
    if point == Vector2.ZERO:
        point = node.get_global_rect().get_center()
    for pressed in [true, false]:
        var event = InputEventMouseButton.new()
        event.position = point
        event.global_position = point
        event.button_index = MOUSE_BUTTON_LEFT
        event.pressed = pressed
        Input.parse_input_event(event)
func button(parent, id):
    var node = null if parent == null else parent.find_child(id, true, false)
    if node == null or not node.is_visible_in_tree() or node.disabled:
        return false
    click(node)
    return true
func finish(error):
    var out = FileAccess.open(TRACE, FileAccess.WRITE)
    out.store_string(JSON.stringify({"error": error, "phase": phase}))
    out.close()
    current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
    quit(0 if error == "" else 2)
'''.replace("TRACE", trace)
        save_and_load = common + '''func _process(delta):
    elapsed += delta
    beat += delta
    if elapsed > 600:
        finish("save/load driver timed out at phase " + str(phase)); return false
    if elapsed < 1.0 or beat < 0.15:
        return false
    beat = 0
    var menu = instance.find_child("GalacticSaveMenu", true, false)
    var esc = null if menu == null else menu.find_child("IDD_GAME_OPTIONS_DIALOG", true, false)
    var save = null if menu == null else menu.find_child("IDD_SAVE_GAME_DIALOG", true, false)
    var load_panel = null if menu == null else menu.find_child("IDD_LOAD_GAME_DIALOG", true, false)
    var modal = null if menu == null else menu.find_child("SaveModal", true, false)
    match phase:
        0:
            key(KEY_ESCAPE); phase = 1
        1:
            if button(esc, "IDC_SAVE_GAME"): phase = 2
        2:
            save.find_child("SaveName", true, false).text = "Restore Start"
            if button(save, "IDC_SAVE_GAME_BUTTON"): phase = 3
        3:
            var panel = menu.find_child("IDD_DIALOG_BOX1", true, false)
            if panel != null and panel.is_visible_in_tree() and button(modal, "IDC_DIALOG_BOX_BUTTON_LEFT"): phase = 4
        4:
            if button(esc, "IDC_LOAD_GAME"): phase = 5
        5:
            var list = load_panel.find_child("IDC_SAVE_GAME_LIST", true, false)
            for i in list.item_count:
                if list.get_item_text(i) == "Restore Start":
                    click(list, list.global_position + list.get_item_rect(i).get_center()); phase = 6
        6:
            if button(load_panel, "IDC_LOAD_GAME_BUTTON"): phase = 7
        7:
            if not menu.visible:
                root.get_texture().get_image().save_png(IMAGE)
                finish("")
    return false
'''.replace("IMAGE", json.dumps(str(directory / "in-campaign.png").replace("\\", "/")))
        save_dir = ["--eawr-galactic-save-dir", str(saves)]
        result, first = gc.GalacticSaveRuntime.run_viewer(self, directory, save_and_load, [
            "--eawr-galactic", "Sandbox_Equal_Footing_Empire", "--eawr-galactic-paused", "yes"] + save_dir)
        self.assertEqual(result.returncode, 0, first.get("failure") or result.stdout + result.stderr)
        self.assertEqual(json.loads((directory / "trace.json").read_text(encoding="utf-8"))["error"], "")
        self.assertTrue((saves / "Restore Start.eawrsave").is_file())
        self.assertEqual(first["save_controls"]["loaded"], 1)
        in_campaign = first["save_controls"]["restored_hash"]
        self.assertEqual(len(in_campaign), 64)

        frontend = common + '''var original = PackedByteArray()
var refusal_seen = false
var loading_seen = false
var loaded_at = -1.0
func _process(delta):
    elapsed += delta
    beat += delta
    if elapsed > 600:
        finish("front-end load timed out at phase " + str(phase)); return false
    var front_load = null
    for child in instance.get_children():
        if child.is_class("EawrLoadGameScreen"):
            front_load = child
            break
    var list_node = null if front_load == null else front_load.find_child("IDC_SAVE_GAME_LIST", true, false)
    var load_panel = null if list_node == null else list_node.get_parent()
    if load_panel != null and load_panel.is_visible_in_tree():
        var boxes = instance.find_child("IDD_DIALOG_BOX_SMALL", true, false)
        if boxes != null: loading_seen = true
    if elapsed < 1.0 or beat < 0.15: return false
    beat = 0
    match phase:
        0:
            if button(instance, "IDC_LOAD_GAME_MAIN"): phase = 1
        1, 6:
            if load_panel == null: return false
            var list = load_panel.find_child("IDC_SAVE_GAME_LIST", true, false)
            for i in list.item_count:
                if list.get_item_text(i) == "Restore Start":
                    click(list, list.global_position + list.get_item_rect(i).get_center())
                    phase += 1
                    break
        2:
            # Change the selected file after the list was built: the launcher must re-read and refuse it.
            original = FileAccess.get_file_as_bytes(__SAVE_PATH__)
            var file = FileAccess.open(__SAVE_PATH__, FileAccess.WRITE)
            file.store_string("damaged since listing")
            file.close()
            if button(load_panel, "IDC_LOAD_GAME_BUTTON"): phase = 3
        3:
            var box = instance.find_child("IDC_DIALOG_BOX_BUTTON_LEFT", true, false)
            if loading_seen and box != null and box.is_visible_in_tree():
                refusal_seen = true
                root.get_texture().get_image().save_png(REFUSAL_IMAGE)
                click(box)
                var file = FileAccess.open(__SAVE_PATH__, FileAccess.WRITE)
                file.store_buffer(original)
                file.close()
                phase = 4
        4:
            if button(load_panel, "IDC_LOAD_CANCEL_BUTTON"): phase = 5
        5:
            if button(instance, "IDC_LOAD_GAME_MAIN"): phase = 6
        7:
            if button(load_panel, "IDC_LOAD_GAME_BUTTON"): phase = 8
        8:
            var galaxy = instance.find_child("GalacticScenes", true, false)
            if galaxy != null and front_load == null:
                if loaded_at < 0: loaded_at = elapsed
                if elapsed - loaded_at < 3.0: return false
                root.get_texture().get_image().save_png(IMAGE)
                if not refusal_seen or not loading_seen:
                    finish("load refusal or Loading box was not seen")
                else:
                    finish("")
    return false
'''
        frontend = frontend.replace("__SAVE_PATH__", json.dumps(str(saves / "Restore Start.eawrsave").replace("\\", "/"), ensure_ascii=False))
        frontend = frontend.replace("REFUSAL_IMAGE", json.dumps(str(directory / "refusal.png").replace("\\", "/")))
        frontend = frontend.replace("IMAGE", json.dumps(str(directory / "front-end.png").replace("\\", "/")))
        result, restored = gc.GalacticSaveRuntime.run_viewer(self, directory, frontend,
            ["--eawr-front-end", "--eawr-audio", "off"] + save_dir)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads((directory / "trace.json").read_text(encoding="utf-8"))["error"], "")
        self.assertEqual(restored["failure"], "")
        self.assertTrue(restored["save_controls"]["started_from_save"])
        self.assertEqual(restored["save_controls"]["restored_hash"], in_campaign)
        self.assertEqual(restored["save_controls"]["restored_tick"], first["save_controls"]["restored_tick"])
        self.assertIn("EAWR front end saved game restore refused: ", result.stdout)
        self.assertIn("EAWR front end attract started", result.stdout)
        self.assertIn("EAWR front end attract stopped: saved game restore", result.stdout)
        self.assertLess(result.stdout.index("EAWR front end attract stopped: saved game restore"),
                        result.stdout.rindex("EAWR galactic mode:"))
        for name in ("refusal.png", "front-end.png"):
            self.assertTrue((directory / name).is_file())


if __name__ == "__main__":
    unittest.main()
