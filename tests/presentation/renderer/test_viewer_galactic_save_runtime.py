"""GHUD-S18/19: native player save/load, first restored HUD and refusal preservation."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

try:
    from . import test_viewer_planet_hud_runtime as support
    from .campaign_space_cases import qualified_campaign_arguments
except ImportError:
    import test_viewer_planet_hud_runtime as support
    from campaign_space_cases import qualified_campaign_arguments


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticSaveRuntime(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # D1/UI-F3: every launch uses fonts extracted from this host's install,
        # independent of an existing checkout cache or the caller's environment.
        cls.font_cache = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-save-fonts-"))
        extracted = subprocess.run([
            sys.executable, str(support.ROOT / "tools/fonts/extract_eaw_fonts.py"),
            "--game-root", support.GAME_ROOT, "--out", str(cls.font_cache),
            "--allow-unknown-build", "--json"], capture_output=True, text=True, timeout=120)
        if extracted.returncode != 0:
            raise RuntimeError(extracted.stdout + extracted.stderr)

    def roundtrip(self, child=False):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-save-"))
        report = directory / "report.json"
        driver = directory / "driver.gd"
        trace = directory / "trace.json"
        driver.write_text('''extends SceneTree
var elapsed = 0.0
var beat = 0.0
var phase = 0
var instance
var restored = false
var last_click = "none"
var wait_for_child = CHILD
var font_checks = {}
func _initialize():
    instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
    DirAccess.make_dir_recursive_absolute("user://saves")
    var disposable = FileAccess.open("user://saves/Disposable.eawrsave", FileAccess.WRITE)
    disposable.store_string("invalid disposable checkpoint")
    disposable.close()
func key(code):
    for pressed in [true, false]:
        var event = InputEventKey.new()
        event.keycode = code
        event.pressed = pressed
        Input.parse_input_event(event)
func check_world():
    var menu = instance.find_child("GalacticSaveMenu", true, false)
    if menu != null and menu.get_meta("save_world_leaks", 0) != 0:
        finish("world selection leaked after " + str(last_click)); return false
    return true
func click(node, point = Vector2.ZERO, double = false):
    last_click = node.name
    if point == Vector2.ZERO:
        point = node.get_global_rect().get_center()
    for pressed in [true, false]:
        var event = InputEventMouseButton.new()
        event.position = point
        event.global_position = point
        event.button_index = MOUSE_BUTTON_LEFT
        event.double_click = double
        event.pressed = pressed
        Input.parse_input_event(event)
    call_deferred("check_world")
func button(parent, id):
    var node = parent.find_child(id, true, false)
    if node == null or not node.is_visible_in_tree() or node.disabled:
        return false
    click(node)
    return true
func row(parent, name, double = false):
    var list = parent.find_child("IDC_SAVE_GAME_LIST", true, false)
    for i in list.item_count:
        if list.get_item_text(i) == name:
            click(list, list.global_position + list.get_item_rect(i).get_center(), double)
            return true
    return false
func acknowledged(menu):
    var panel = menu.find_child("IDD_DIALOG_BOX1", true, false)
    return panel != null and panel.is_visible_in_tree()
func finish(error):
    var out = FileAccess.open(TRACE, FileAccess.WRITE)
    out.store_string(JSON.stringify({"error": error, "restored": restored, "phase": phase, "fonts": font_checks}))
    current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
    quit(0 if error == "" else 2)
func capture_dialog(panel, title_name, button_name, name):
    if panel == null or not panel.is_visible_in_tree(): return false
    var title = panel.find_child(title_name, true, false)
    var button = panel.find_child(button_name, true, false)
    var title_face = title.get_theme_font("L_Text").get_font_name()
    var button_face = button.get_theme_font("Push_Button").get_font_name()
    var title_size = title.get_theme_font_size("L_Text")
    var button_size = button.get_theme_font_size("Push_Button")
    # Stock GUIDialogs overrides and UI-F1: 14pt -> 22px, 8pt -> 12px at 720p.
    if title_face != "Empire At War Bold" or button_face != "Empire At War Bold" or title_size != 22 or button_size != 12:
        finish("authored dialog font differs: " + name + " title=" + title_face + "/" + str(title_size) + " button=" + button_face + "/" + str(button_size))
        return false
    font_checks[name] = {"title_face": title_face, "title_pixels": title_size, "button_face": button_face, "button_pixels": button_size}
    root.get_texture().get_image().save_png(OUTPUT + "/" + name + ".png")
    return true
func _process(delta):
    if not check_world(): return false
    elapsed += delta
    beat += delta
    if elapsed > 600:
        finish("save/load driver timed out at phase " + str(phase))
        return false
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
            if wait_for_child and instance.find_child("CampaignSpaceBattleReady", true, false) == null:
                return false
            key(KEY_ESCAPE); phase = 1
        1:
            # Escape is refused while the battle's loading state is still shown: press it again.
            if esc == null or not esc.is_visible_in_tree():
                key(KEY_ESCAPE); return false
            if not capture_dialog(esc, "IDC_STATIC_MEDIUM", "IDC_GAME_OPTIONS_RESUME", "escape"): return false
            if button(esc, "IDC_GAME_OPTIONS_RESUME"): phase = 2
        2:
            if not menu.visible: key(KEY_ESCAPE); phase = 3
        3:
            if button(esc, "IDC_SAVE_GAME"): phase = 4
        4:
            if not capture_dialog(save, "IDC_STATIC_BIG", "IDC_SAVE_GAME_BUTTON", "save"): return false
            if button(save, "IDC_SAVE_CANCEL_BUTTON"): phase = 5
        5:
            if button(esc, "IDC_LOAD_GAME"): phase = 6
        6:
            if not capture_dialog(load_panel, "IDC_STATIC_MEDIUM", "IDC_LOAD_GAME_BUTTON", "load"): return false
            if button(load_panel, "IDC_LOAD_CANCEL_BUTTON"): phase = 7
        7:
            if button(esc, "IDC_SAVE_GAME"): phase = 8
        8:
            var name = save.find_child("SaveName", true, false)
            if name.max_length != 63: finish("filename limit differs"); return false
            name.text = "[QuickSave]"
            if button(save, "IDC_SAVE_GAME_BUTTON"): phase = 9
        9:
            var name = save.find_child("SaveName", true, false)
            if name.text != "": finish("reserved name was not cleared"); return false
            name.text = "GPU Roundtrip"
            if button(save, "IDC_SAVE_GAME_BUTTON"): phase = 10
        10:
            if acknowledged(menu):
                var caption = modal.find_child("IDC_CAPTION", true, false).get_meta("save_caption")
                if caption != "Game Saved": finish("manual publication failed: " + str(caption)); return false
                root.get_texture().get_image().save_png(SAVED)
                if button(modal, "IDC_DIALOG_BOX_BUTTON_LEFT"): phase = 11
        11:
            if button(esc, "IDC_SAVE_GAME"): phase = 12
        12:
            if row(save, "GPU Roundtrip"): phase = 13
        13:
            if button(save, "IDC_SAVE_GAME_BUTTON"): phase = 14
        14:
            if button(modal, "IDC_DIALOG_BOX_BUTTON_RIGHT"): phase = 15
        15:
            if button(save, "IDC_SAVE_GAME_BUTTON"): phase = 16
        16:
            if button(modal, "IDC_DIALOG_BOX_BUTTON_LEFT"): phase = 17
        17:
            if acknowledged(menu) and button(modal, "IDC_DIALOG_BOX_BUTTON_LEFT"): phase = 171
        171:
            if button(esc, "IDC_SAVE_GAME"): phase = 172
        172:
            if row(save, "GPU Roundtrip"): phase = 173
        173:
            # A changed name colliding with another file is a new-name request,
            # not selected-row overwrite confirmation. The provider refuses it.
            save.find_child("SaveName", true, false).text = "Disposable"
            if button(save, "IDC_SAVE_GAME_BUTTON"): phase = 174
        174:
            if acknowledged(menu):
                if modal.find_child("IDC_CAPTION", true, false).get_meta("save_caption") == "Game Saved":
                    finish("failed publication falsely acknowledged"); return false
                if button(modal, "IDC_DIALOG_BOX_BUTTON_LEFT"): phase = 18
        18:
            if button(esc, "IDC_SAVE_GAME"): phase = 19
        19:
            if row(save, "Disposable"): phase = 20
        20:
            if button(save, "IDC_DELETE_GAME_BUTTON"): phase = 21
        21:
            if button(modal, "IDC_DIALOG_BOX_BUTTON_RIGHT"): phase = 22
        22:
            if button(save, "IDC_DELETE_GAME_BUTTON"): phase = 23
        23:
            if button(modal, "IDC_DIALOG_BOX_BUTTON_LEFT"): phase = 24
        24:
            if FileAccess.file_exists("user://saves/Disposable.eawrsave"): return false
            if button(save, "IDC_SAVE_CANCEL_BUTTON"): phase = 25
        25:
            if button(esc, "IDC_LOAD_GAME"): phase = 26
        26:
            if row(load_panel, "GPU Roundtrip"): phase = 27
        27:
            if wait_for_child:
                if row(load_panel, "GPU Roundtrip", true): phase = 28
            elif button(load_panel, "IDC_LOAD_GAME_BUTTON"): phase = 28
        28:
            if not menu.visible:
                restored = true
                root.get_texture().get_image().save_png(RESTORED)
                if wait_for_child: phase = 37
                else: key(KEY_F6); phase = 30
        30:
            if acknowledged(menu):
                # F5 during a persistent save acknowledgement retains an intent;
                # it runs after Okay, without dismissing or dropping the modal.
                key(KEY_F5); phase = 300
        300:
            if acknowledged(menu) and button(modal, "IDC_DIALOG_BOX_BUTTON_LEFT"): phase = 301
        301:
            if not menu.visible: phase = 31
        31:
            if not menu.visible: key(KEY_F6); phase = 32
        32:
            if acknowledged(menu) and button(modal, "IDC_DIALOG_BOX_BUTTON_LEFT"): phase = 33
        33:
            if not menu.visible: key(KEY_F5); phase = 34
        34:
            if not menu.visible: phase = 37
        37:
            var corrupt = FileAccess.open("user://saves/Corrupt.eawrsave", FileAccess.WRITE)
            corrupt.store_string("invalid checkpoint"); corrupt.close()
            key(KEY_ESCAPE); phase = 38
        38:
            # The restored battle shows its loading state again: Escape is refused until it is up.
            if esc == null or not esc.is_visible_in_tree():
                key(KEY_ESCAPE); return false
            if button(esc, "IDC_LOAD_GAME"): phase = 39
        39:
            if row(load_panel, "Corrupt"): phase = 40
        40:
            if button(load_panel, "IDC_LOAD_GAME_BUTTON"): phase = 41
        41:
            if acknowledged(menu):
                root.get_texture().get_image().save_png(REFUSED)
                finish("")
    return false
'''.replace("CHILD", "true" if child else "false")
            .replace("TRACE", json.dumps(str(trace).replace("\\", "/")))
            .replace("SAVED", json.dumps(str(directory / "saved.png").replace("\\", "/")))
            .replace("RESTORED", json.dumps(str(directory / "restored.png").replace("\\", "/")))
            .replace("REFUSED", json.dumps(str(directory / "refused.png").replace("\\", "/")))
            .replace("OUTPUT", json.dumps(directory.as_posix()))
            , encoding="utf-8")
        arguments = [
            os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-game-root", support.GAME_ROOT,
            "--eawr-font-cache", str(self.font_cache),
            "--eawr-lighting", "sh", "--eawr-shadows", "on", "--eawr-report", str(report)]
        if child:
            arguments += qualified_campaign_arguments("save")
        else:
            arguments += ["--eawr-galactic", "Sandbox_Equal_Footing_Empire", "--eawr-galactic-paused", "yes"]
        result = subprocess.run(arguments, cwd=support.ROOT, capture_output=True, text=True, timeout=900)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
        trace_data = json.loads(trace.read_text(encoding="utf-8"))
        self.assertEqual(trace_data["error"], "")
        self.assertEqual(set(trace_data["fonts"]), {"escape", "save", "load"})
        self.assertEqual(data["save_controls"]["saved"], 2 if child else 4)
        self.assertEqual(data["save_controls"]["loaded"], 1 if child else 3, "corrupt refusal replaced the successful owner")
        self.assertTrue(data["save_controls"]["visible"], "load refusal discarded the old menu/view")
        self.assertNotEqual(data["save_controls"]["message"], "Game loaded.")
        self.assertGreater(data["save_controls"]["restored_tick"], 0)
        self.assertEqual(data["failure"], "")
        self.assertEqual(data["save_controls"]["world_leaks"], 0, "modal clicks leaked into world selection")
        self.assertEqual(data["save_controls"]["widget_failure"], "")
        self.assertGreaterEqual(data["save_controls"]["requests_after_fade"], 3)
        if child:
            self.assertTrue(data["space_cycle_proof"]["moved"] and data["space_cycle_proof"]["fight_enabled"])
            self.assertTrue(data["space_cycle_proof"]["save_ready"])
            self.assertTrue(data["space_cycle_proof"]["map"].lower().endswith(".ted"))
            self.assertGreater(data["save_controls"]["restored_conflict"], 0)
            self.assertEqual(data["handoff"]["conflict"], data["save_controls"]["restored_conflict"])
            self.assertEqual(data["handoff"]["child_ready_tick"], data["save_controls"]["restored_child_tick"],
                             "loaded success preceded the retained MapMode readiness barrier")
        for name in ("escape.png", "save.png", "load.png", "saved.png", "restored.png", "refused.png"):
            self.assertTrue((directory / name).is_file())

    def run_viewer(self, directory, driver_source, extra):
        report = directory / ("report-%d.json" % len(list(directory.glob("report-*.json"))))
        driver = directory / "driver.gd"
        driver.write_text(driver_source, encoding="utf-8")
        arguments = [
            os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-game-root", support.GAME_ROOT,
            "--eawr-font-cache", str(self.font_cache),
            "--eawr-lighting", "sh", "--eawr-shadows", "on", "--eawr-report", str(report)] + extra
        result = subprocess.run(arguments, cwd=support.ROOT, capture_output=True, text=True, timeout=900)
        (directory / (report.stem + ".log")).write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        return result, json.loads(report.read_text(encoding="utf-8"))

    def test_restore_at_start_matches_in_campaign_load(self):
        """#3035: GalacticMode restore_save prepares the save's own campaign, then loads it
        through the same prepare/accept route as the in-campaign Load Game."""
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-restore-"))
        saves = directory / "saves"
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
        settle = common + '''func _process(delta):
    elapsed += delta
    if elapsed > 3.0:
        root.get_texture().get_image().save_png(IMAGE)
        finish("")
    return false
'''.replace("IMAGE", json.dumps(str(directory / "at-start.png").replace("\\", "/")))
        save_dir = ["--eawr-galactic-save-dir", str(saves)]
        result, first = self.run_viewer(directory, save_and_load, [
            "--eawr-galactic", "Sandbox_Equal_Footing_Empire", "--eawr-galactic-paused", "yes"] + save_dir)
        self.assertEqual(result.returncode, 0, first.get("failure") or result.stdout + result.stderr)
        self.assertEqual(json.loads((directory / "trace.json").read_text(encoding="utf-8"))["error"], "")
        self.assertTrue((saves / "Restore Start.eawrsave").is_file())
        self.assertEqual(first["save_controls"]["loaded"], 1)
        in_campaign = first["save_controls"]["restored_hash"]
        self.assertEqual(len(in_campaign), 64)

        result, second = self.run_viewer(directory, settle, ["--eawr-galactic-restore", "Restore Start"] + save_dir)
        self.assertEqual(result.returncode, 0, second.get("failure") or result.stdout + result.stderr)
        self.assertEqual(second["failure"], "")
        self.assertTrue(second["save_controls"]["started_from_save"])
        self.assertEqual(second["save_controls"]["loaded"], 1)
        self.assertEqual(second["save_controls"]["restored_hash"], in_campaign,
                         "restore-at-start differs from the in-campaign load of the same save")
        self.assertEqual(second["save_controls"]["restored_tick"], first["save_controls"]["restored_tick"])
        for name in ("in-campaign.png", "at-start.png"):
            self.assertTrue((directory / name).is_file())

        result, refused = self.run_viewer(directory, settle, ["--eawr-galactic-restore", "Missing Save"] + save_dir)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Save not found", refused["failure"])
        self.assertEqual(refused["status"], "failed")

    def test_player_save_load_and_corrupt_refusal(self):
        self.roundtrip()

    def test_retained_child_save_load_and_corrupt_refusal(self):
        self.roundtrip(child=True)

    def test_save_load_during_pending_prompt_and_summary(self):
        """WGS-P01: manual recovery at both real live conflict presentation barriers."""
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-prompt-save-"))
        driver = r'''extends SceneTree
var instance
var elapsed = 0.0
var beat = 0.0
var phase = 0
var summary = false
var resaved = false
var original = []
var restored = []
var quiet_since = 0.0
var roundtrips = []
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
func control(parent, name):
    return null if parent == null else parent.find_child(name, true, false)
func ready(node):
    return node != null and node.is_visible_in_tree() and not node.is_disabled()
func click(parent, name):
    var node = control(parent, name)
    if not ready(node): return false
    for pressed in [true, false]:
        var event = InputEventMouseButton.new()
        event.position = node.get_global_rect().get_center()
        event.global_position = event.position
        event.button_index = MOUSE_BUTTON_LEFT
        event.pressed = pressed
        Input.parse_input_event(event)
    return true
func captions(node):
    var result = []
    if node == null: return result
    if node is Control and node.is_visible_in_tree() and node.has_meta("battle_caption"):
        result.append(str(node.get_meta("battle_caption")))
    for child in node.get_children(): result.append_array(captions(child))
    return result
func finish(error):
    var file = FileAccess.open(TRACE, FileAccess.WRITE)
    file.store_string(JSON.stringify({"error": error, "phase": phase, "original": original, "restored": restored, "roundtrips": roundtrips}))
    file.close()
    instance.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
    quit(0 if error == "" else 2)
func _process(delta):
    elapsed += delta
    beat += delta
    if elapsed > 300: finish("prompt save timed out"); return false
    if beat < 0.2: return false
    beat = 0
    var battle = control(instance, "CampaignBattleDialog")
    var menu = control(instance, "GalacticSaveMenu")
    var esc = control(menu, "IDD_GAME_OPTIONS_DIALOG")
    var save = control(menu, "IDD_SAVE_GAME_DIALOG")
    var load_panel = control(menu, "IDD_LOAD_GAME_DIALOG")
    var modal = control(menu, "SaveModal")
    var slot = "Prompt Resaved" if resaved else ("Prompt Summary" if summary else "Prompt Pending")
    match phase:
        0:
            if not ready(control(battle, "choice_button_right")): return false
            original = captions(battle)
            root.get_texture().get_image().save_png(OUTPUT + "/pending-before.png")
            key(KEY_ESCAPE); phase = 1
        1:
            if not ready(control(esc, "IDC_SAVE_GAME")) or not ready(control(esc, "IDC_LOAD_GAME")): return false
            if click(esc, "IDC_SAVE_GAME"): phase = 2
        2:
            var name = control(save, "SaveName")
            if name != null and name.is_visible_in_tree():
                name.text = slot
                if click(save, "IDC_SAVE_GAME_BUTTON"): phase = 3
        3:
            var caption = control(modal, "IDC_CAPTION")
            if caption != null and caption.is_visible_in_tree() and caption.get_meta("save_caption", "") == "Game Saved":
                if click(modal, "IDC_DIALOG_BOX_BUTTON_LEFT"): phase = 4
        4:
            if click(esc, "IDC_LOAD_GAME"): phase = 5
        5:
            var rows = control(load_panel, "IDC_SAVE_GAME_LIST")
            if rows == null: return false
            for row in range(rows.item_count):
                if rows.get_item_text(row) == slot:
                    var point = rows.get_global_rect().position + rows.get_item_rect(row).get_center()
                    for pressed in [true, false]:
                        var event = InputEventMouseButton.new()
                        event.position = point
                        event.global_position = point
                        event.button_index = MOUSE_BUTTON_LEFT
                        event.pressed = pressed
                        Input.parse_input_event(event)
                    phase = 6
                    break
        6:
            if click(load_panel, "IDC_LOAD_GAME_BUTTON"): phase = 7
        7:
            if menu != null and not menu.visible:
                if not ready(control(battle, "IDC_CONTINUE_BUTTON" if summary else "choice_button_right")): return false
                restored = captions(battle)
                roundtrips.append({"summary": summary, "before": original, "after": restored})
                if original.is_empty() or restored != original: finish("restored prompt captions differ"); return false
                var restored_image = "/resaved-summary-restored.png" if resaved else ("/summary-restored.png" if summary else "/pending-restored.png")
                root.get_texture().get_image().save_png(OUTPUT + restored_image)
                if summary and not resaved:
                    resaved = true; original = restored
                    key(KEY_ESCAPE); phase = 1; return false
                if click(battle, "IDC_CONTINUE_BUTTON" if summary else "choice_button_right"):
                    phase = 9 if summary else 8
                    quiet_since = elapsed
        8:
            if ready(control(battle, "IDC_CONTINUE_BUTTON")):
                summary = true
                original = captions(battle)
                root.get_texture().get_image().save_png(OUTPUT + "/summary-before.png")
                key(KEY_ESCAPE); phase = 1
        9:
            if battle != null and battle.is_visible_in_tree():
                if elapsed - quiet_since > 1: finish("acknowledged summary reappeared"); return false
            elif elapsed - quiet_since > 4:
                finish("")
    return false
'''.replace("TRACE", json.dumps((directory / "trace.json").as_posix())).replace(
            "OUTPUT", json.dumps(directory.as_posix()))
        result, data = self.run_viewer(directory, driver, qualified_campaign_arguments("prompt-save"))
        self.assertEqual(result.returncode, 0, data.get("failure") or (directory / "report-0.log").read_text(encoding="utf-8"))
        self.assertTrue((directory / "trace.json").is_file(), data.get("failure"))
        trace = json.loads((directory / "trace.json").read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, trace)
        self.assertEqual(trace["error"], "")
        self.assertEqual([row["summary"] for row in trace["roundtrips"]], [False, True, True])
        for row in trace["roundtrips"]:
            self.assertTrue(row["before"])
            self.assertEqual(row["before"], row["after"])
        self.assertEqual(data["save_controls"]["saved"], 3)
        self.assertEqual(data["save_controls"]["loaded"], 3)
        self.assertEqual(data["save_controls"]["world_leaks"], 0)
        self.assertEqual(data["battle_dialog"]["phase"], "hidden")
        self.assertEqual(data["battle_dialog"]["choices"], 2, "one auto resolve and one summary acknowledgement")
        self.assertEqual(data["failure"], "")
        for name in ("pending-before", "pending-restored", "summary-before", "summary-restored", "resaved-summary-restored"):
            self.assertTrue((directory / (name + ".png")).is_file())
        print("prompt save captures:", directory)


if __name__ == "__main__":
    unittest.main()
