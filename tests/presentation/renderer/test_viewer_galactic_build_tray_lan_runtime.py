"""GBT-01 to GBT-03 in a RUNNING game, single player and two-human multiplayer (docs/ui/galactic-build-tray.md).

The build tray's other GPU cases run a paused single-player game. This case runs the clock (the owner plays unpaused)
and, in the multiplayer case, two real viewers: a host and a guest that start a Galactic Conquest game through the
lobby launch the acceptance rig uses. Every human seat starts at tech 5 with a level 5 star base on every planet it
owns, so each peer's planet offers more space choices than the bar has slots.

Both peers (and the single-player viewer) run the same driver. The planet starts on its ground tab, whose choices fit
the bar: the handle is there all the same (no count), the handle and the Tab key open the tray with the bar's choices
and Tab closes it. Then the space tab, which overflows the bar: the handle reads +N, one choice is bought from the bar
and two from the tray, each waited for in the queue (a planet's space queue holds three orders). The case checks, per peer, those states, that every click showed
its type in the queue, and, across the two peers, that both stayed in sync.
"""
import json
import os
import pathlib
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import xml.etree.ElementTree as ET

from tests.presentation.renderer import test_viewer_galactic_command_bar_runtime as support

ROOT, RUNTIME, GAME_ROOT = support.ROOT, support.RUNTIME, support.GAME_ROOT
CAMPAIGN = "Sandbox_Equal_Footing_Empire"
TOOL = ROOT / "tools/fonts/extract_eaw_fonts.py"
TRAY_CLICKS = 2  # with the bar click: three orders, each one waited for in the queue (the planet's space queue holds three)

DRIVER = '''extends SceneTree
var ROLE = %(role)s
var SP = %(sp)s
var STEPS = %(steps)s
var CAPTURE = %(capture)s
var t0 = -1
var last = 0
var phase = "fit_handle"
var attempts = 0
var clicks = []
var tray_clicks = 0
var done = false
var log = []
func _initialize(): call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func find(name):
    return root.find_child(name, true, false)
func shown(node):
    return node != null and node.is_visible_in_tree()
func visible_entries():
    # Godot renames same-named siblings, so the entries are the visible hud buttons inside the open panel.
    var found = []
    var panel = find("GalacticBuildTray")
    if not shown(panel): return found
    var area = panel.get_global_rect()
    for node in root.find_children("*", "EawrHudButton", true, false):
        var label = str(node.name)
        if not node.is_visible_in_tree() or label == "GalacticBuildTrayClose" or label.begins_with("b_") or label.begins_with("queue") or label.begins_with("ground_presence"):
            continue
        if area.has_point(node.get_global_rect().get_center()): found.append(node)
    # Other hud buttons can sit under the panel (one did in the single-player run: the first "entry" was not a card), so
    # only buttons of the size of the entry the tray named (the first one keeps its name) count.
    var reference = find("build_tray_entry")
    if reference == null: return []
    return found.filter(func(n): return n.get_global_rect().size.is_equal_approx(reference.get_global_rect().size))
func visible_cards():
    return root.find_children("b_Create_*", "", true, false).filter(func(n): return n.is_visible_in_tree())
var pending_click = null
var pending_frames = 0
func press(canvas_position):
    # As a real mouse: the pointer arrives first, the UI hit-test settles over a few frames (a slow host needs them), then
    # the button goes down and, a frame later, up. What the pointer was over at the press is kept with the click.
    var position = root.get_final_transform() * canvas_position
    var motion = InputEventMouseMotion.new()
    motion.position = position
    motion.global_position = position
    Input.parse_input_event(motion)
    pending_click = position
    pending_frames = 0
func button_event(down):
    var event = InputEventMouseButton.new()
    event.button_index = MOUSE_BUTTON_LEFT
    event.pressed = down
    event.position = pending_click
    event.global_position = pending_click
    Input.parse_input_event(event)
func release_pending_click():
    if pending_click == null: return
    pending_frames += 1
    if pending_frames == 4:
        if not clicks.is_empty(): clicks[clicks.size() - 1]["over"] = hovered()
        button_event(true)
    elif pending_frames == 5:
        button_event(false)
        pending_click = null
func key(name):
    for down in [true, false]:
        var event = InputEventKey.new()
        event.keycode = OS.find_keycode_from_string(name)
        event.physical_keycode = event.keycode
        event.pressed = down
        Input.parse_input_event(event)
func modals():
    var shown_layers = []
    for node in get_nodes_in_group("eawr_ui_modal"):
        if node.is_visible_in_tree(): shown_layers.append(str(node.get_path()) + " (" + node.get_class() + ")")
    return shown_layers
func hovered():
    var control = root.gui_get_hovered_control()
    return "" if control == null else str(control.get_path()) + " (" + control.get_class() + ")"
func focus_owner():
    var owner = root.gui_get_focus_owner()
    return "" if owner == null else str(owner.get_path()) + " (" + owner.get_class() + ")"
func icon_hash(button):
    # What a queue slot shows is the clicked card's own icon: the viewer caches one texture per art name, so the texture
    # instance identifies the type (a card's icon and its queue slot's are the same object).
    if button == null: return ""
    for child in button.get_children():
        if child is TextureRect and child.texture != null: return str(child.texture)
    return ""
func icon_count(hash):
    var count = 0
    for node in root.find_children("queue*", "EawrHudButton", true, false):
        if node.is_visible_in_tree() and icon_hash(node) == hash: count += 1
    return count
func order_pending(click):
    # A click is seen once the queue shows one more slot of its icon: the harness waits for that change (8 s), then
    # snapshots it, so the sandbox finishing items later cannot hide a wrong type.
    if click["icon"] == "": return false
    return icon_count(click["icon"]) <= click["before"]
func queued_slots():
    var count = 0
    for node in root.find_children("queue*", "EawrHudButton", true, false):
        if node.is_visible_in_tree(): count += 1
    return count
func note(what):
    log.append({"at": Time.get_ticks_msec() - t0, "what": what, "handle": shown(find("GalacticBuildTrayHandle")),
        "panel": shown(find("GalacticBuildTray")), "entries": visible_entries().size(), "cards": visible_cards().size(), "queued": queued_slots(),
        "names": visible_entries().map(func(n): return str(n.name)), "focus": focus_owner(), "modals": modals(), "hovered": hovered(), "window": [root.size.x, root.size.y], "final": str(root.get_final_transform())})
    var file = FileAccess.open(STEPS, FileAccess.WRITE)
    file.store_string(JSON.stringify({"role": ROLE, "log": log, "clicks": clicks}))
    file.close()
func at(when):
    return Time.get_ticks_msec() - last >= when
func advance(next):
    phase = next
    last = Time.get_ticks_msec()
func tab_point(attempt):
    var rect = find("b_space_tab").get_global_rect()
    var offsets = [Vector2(0.5, 0.5), Vector2(0.5, 0.3), Vector2(0.5, 0.7), Vector2(0.3, 0.5), Vector2(0.7, 0.5)]
    var o = offsets[attempt %% offsets.size()]
    return rect.position + Vector2(rect.size.x * o.x, rect.size.y * o.y)
func finish(why):
    done = true
    note("finish: " + why)
    if SP:
        root.get_texture().get_image().save_png(CAPTURE)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
func _process(_delta):
    if done: return false
    release_pending_click()
    # Story and review prompts of a lobby launch need an OK, as the lobby rig's own drivers press it.
    for node in root.find_children("*", "Button", true, false):
        if node.is_visible_in_tree() and node.text == "OK":
            node.emit_signal("pressed")
            break
    # The campaign's intro summary is a modal window the player closes; the single-player capture flag that does it
    # for the harness is not part of the lobby launch.
    var summary = find("galactic_story_summary")
    if shown(summary):
        var close_button = summary.find_child("IDC_SUMMARY_DIALOG_CLOSE_BUTTON", true, false)
        if close_button != null: close_button.emit_signal("pressed")
        return false
    if t0 < 0:
        if find("GalacticWorldOverlays") == null or find("b_space_tab") == null: return false
        t0 = Time.get_ticks_msec()
        last = t0
        note("ready")
        return false
    if Time.get_ticks_msec() - t0 > 150000:
        finish("timeout in " + phase)
        return false
    # 1. The planet starts on the ground tab, whose choices fit the bar: the handle is there all the same.
    if phase == "fit_handle":
        if not at(%(settle)d): return false
        var handle = find("GalacticBuildTrayHandle")
        if not shown(handle):
            last = Time.get_ticks_msec() - %(settle)d + 1000
            return false
        note("fit handle seen")
        press(handle.get_global_rect().get_center())
        advance("fit_opened")
    elif phase == "fit_opened":
        # Two viewers share one GPU in the multiplayer case: the panel can take longer than a step to appear, so a step
        # waits for the state it asserts (up to 8 s), and the assertion still reads the state it got.
        if not at(1500) or (not shown(find("GalacticBuildTray")) and not at(8000)): return false
        note("fit opened by the handle")
        key("Tab")
        advance("fit_key_closed")
    elif phase == "fit_key_closed":
        if not at(1200) or (shown(find("GalacticBuildTray")) and not at(8000)): return false
        note("fit closed by Tab")
        key("Tab")
        advance("fit_key_opened")
    elif phase == "fit_key_opened":
        if not at(1200) or (not shown(find("GalacticBuildTray")) and not at(8000)): return false
        note("fit opened by Tab")
        advance("tab")
    # 2. The space tab overflows the bar: the tray (kept open) lists more entries than the bar shows cards.
    elif phase == "tab":
        if not at(1500): return false
        if visible_entries().size() > visible_cards().size():
            note("space tab overflows")
            advance("bar")
        elif attempts >= 12:
            finish("the space tab never overflowed the bar")
        else:
            press(tab_point(attempts))
            attempts += 1
            advance("tab")
    elif phase == "bar":
        if not at(500): return false
        var card = find("b_Create_00")
        if card == null or not card.is_visible_in_tree() or card.disabled:
            finish("bar card 0 not buyable")
            return false
        var point = card.get_global_rect().get_center()
        var icon = icon_hash(card)
        clicks.append({"kind": "bar", "point": [point.x, point.y], "icon": icon, "before": icon_count(icon)})
        press(point)
        advance("bar_done")
    elif phase == "bar_done":
        if not at(1000) or (order_pending(clicks[clicks.size() - 1]) and not at(8000)): return false
        clicks[clicks.size() - 1]["seen"] = icon_count(clicks[clicks.size() - 1]["icon"])
        note("bar click")
        advance("tray")
    elif phase == "tray":
        if tray_clicks >= %(tray_clicks)d:
            advance("end")
            return false
        var candidates = visible_entries().filter(func(n): return not n.disabled)
        if candidates.is_empty():
            finish("no buyable tray entry")
            return false
        var chosen = candidates[tray_clicks %% candidates.size()]
        var point = chosen.get_global_rect().get_center()
        var icon = icon_hash(chosen)
        clicks.append({"kind": "tray", "point": [point.x, point.y], "icon": icon, "before": icon_count(icon)})
        press(point)
        tray_clicks += 1
        advance("tray_done")
    elif phase == "tray_done":
        if not at(1000) or (order_pending(clicks[clicks.size() - 1]) and not at(8000)): return false
        clicks[clicks.size() - 1]["seen"] = icon_count(clicks[clicks.size() - 1]["icon"])
        note("tray click")
        advance("tray")
    elif phase == "end":
        if not at(3000): return false
        finish("done")
    return false
'''


def overflow_mod(directory):
    """A mod whose campaign file gives every faction tech 5, credits and level 5 star bases."""
    from tools.inventory.corpus import Corpus

    mod = directory / "mod"
    xml = mod / "Data/XML"
    xml.mkdir(parents=True)
    (mod / "Data/MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
    source = Corpus(pathlib.Path(GAME_ROOT)).read_effective("foc", "data/xml/campaigns_underworld_gc.xml")
    tree = ET.fromstring(source.data)
    campaign = next(node for node in tree if node.get("Name") == CAMPAIGN)
    for node in campaign:
        text = (node.text or "").strip()
        faction = text.split(",")[0].strip()
        if node.tag == "Starting_Tech_Level":
            node.text = faction + ", 5"
        elif node.tag == "Starting_Credits":
            node.text = faction + ", 50000"
        elif node.tag == "Starting_Forces":
            node.text = re.sub(r"_Star_Base_\d+\b", "_Star_Base_5", text)
    (xml / "campaigns_underworld_gc.xml").write_bytes(ET.tostring(tree, encoding="utf-8"))
    # The bar binds all 26 stock slots, which the choices fit; the mod authors the slot limit (GHUD-S92) at 16, 8 per
    # row, so the space list overflows and the tray has work to do (as the paused case's fixture does).
    constants = ET.fromstring(Corpus(pathlib.Path(GAME_ROOT)).read_effective("foc", "data/xml/gameconstants.xml").data)
    for old in constants.findall("Strategic_Command_Bar_Buildable_Row_Count"):
        constants.remove(old)
    ET.SubElement(constants, "Strategic_Command_Bar_Buildable_Row_Count").text = "16"
    (xml / "GameConstants.xml").write_bytes(ET.tostring(constants, encoding="utf-8"))
    return mod


def revised_layer(directory):
    layer = directory / "revised"
    layer_xml = layer / "Data/XML"
    layer_xml.mkdir(parents=True)
    for name in ("RevisedBuildTray.xml", "RevisedBuildTrayGroups.xml"):
        shutil.copyfile(ROOT / "tools/revised_build_tray" / name, layer_xml / name)
    return layer


def free_ports(count):
    sockets, ports = [], []
    try:
        for _ in range(count):
            probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            probe.bind(("127.0.0.1", 0))
            sockets.append(probe)
            ports.append(probe.getsockname()[1])
    finally:
        for probe in sockets:
            probe.close()
    return ports


@unittest.skipUnless(RUNTIME and GAME_ROOT and os.environ.get("EAWR_GODOT_EXECUTABLE"),
                     "requires the GPU viewer and installed FoC")
class GalacticBuildTrayRunningRuntime(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-build-tray-lan-"))
        cls.layer = revised_layer(cls.directory)
        cls.mod = overflow_mod(cls.directory)

    def settings(self, name):
        path = self.directory / (name + ".cfg")
        path.write_text("\n".join(["eawr-settings 1", "revised_layer_enabled=1",
                                   "revised_layer_path=" + self.layer.as_posix()]) + "\n", encoding="utf-8")
        return path

    def driver(self, name, *, role, sp, settle):
        path = self.directory / (name + ".gd")
        path.write_text(DRIVER % {
            "role": json.dumps(role), "sp": "true" if sp else "false",
            "steps": json.dumps((self.directory / (name + "-steps.json")).as_posix()),
            "capture": json.dumps((self.directory / (name + ".png")).as_posix()),
            "settle": settle, "tray_clicks": TRAY_CLICKS}, encoding="utf-8")
        return path

    def check_peer(self, report, steps, label):
        """What one viewer showed and queued: the same checks for single player and for each multiplayer peer."""
        log = {row["what"]: row for row in steps["log"]}
        self.assertEqual(steps["log"][-1]["what"], "finish: done", (label, steps["log"]))
        # The planet's ground tab fits the bar: the handle is shown all the same, the handle and the key both open the
        # tray with the very choices the bar shows, and the key closes it.
        fit = log["fit handle seen"]
        self.assertTrue(fit["handle"] and not fit["panel"] and fit["cards"] > 0, (label, "the handle, tray closed", fit))
        for what in ("fit opened by the handle", "fit opened by Tab"):
            row = log[what]
            self.assertTrue(row["handle"] and row["panel"], (label, what, row))
            self.assertEqual(row["entries"], row["cards"], (label, what, "nothing omitted: the tray lists the bar's choices", row))
        closed = log["fit closed by Tab"]
        self.assertTrue(closed["handle"] and not closed["panel"] and closed["entries"] == 0, (label, closed))
        # The space tab overflows the bar: the open tray lists more than the bar's cards.
        overflow = log["space tab overflows"]
        self.assertTrue(overflow["handle"] and overflow["panel"] and overflow["entries"] > overflow["cards"], (label, overflow))
        tray = report["build_tray"]
        self.assertTrue(tray["enabled"], (label, tray))
        self.assertTrue(tray["handle"] and tray["open"], (label, tray))
        self.assertGreater(tray["omitted"], 0, (label, tray))
        self.assertEqual(tray["handle_text"], "+%d" % tray["omitted"], (label, tray))
        self.assertEqual((tray["handle_presses"], tray["key_presses"]), (1, 2), (label, tray))
        entries = tray["shown_entries"]
        self.assertEqual(len(entries), tray["entries"], (label, tray))
        bar = {card["type_id"]: card for card in report["command_bar"]["production_choices"]}
        self.assertTrue(bar, (label, "the bar shows cards"))
        self.assertTrue(set(bar) <= {entry["type_id"] for entry in entries},
                        (label, "the tray lists every offer the bar shows"))
        for entry in entries:
            if entry["type_id"] in bar:
                self.assertEqual(entry["price"], bar[entry["type_id"]]["price"], (label, entry))
        clicks = steps["clicks"]
        self.assertEqual([click["kind"] for click in clicks], ["bar"] + ["tray"] * TRAY_CLICKS, (label, clicks))
        # What the player saw, click by click: the harness waited for the queue to show one more slot of the clicked
        # card's icon before the next click and recorded it (`seen` over `before`), so the type is witnessed while the
        # item is still queued; the sandbox finishes items quickly, so the final queue is no witness of the earlier ones.
        for number, click in enumerate(clicks):
            self.assertTrue(click["icon"], (label, "the clicked card shows an icon", number, click))
            self.assertGreater(click["seen"], click["before"], (label, "the queue showed the clicked type", number, click))
        # And what each click bought: the tray entry under the pointer is a listed offer, and a bar offer has the bar's
        # price; every order still queued at the end is one of the clicked (type, price) pairs.
        expected = set()
        for click in clicks:
            hit = [entry for entry in entries if abs(entry["rect"][0] + entry["rect"][2] / 2 - click["point"][0]) <= 1.0
                   and abs(entry["rect"][1] + entry["rect"][3] / 2 - click["point"][1]) <= 1.0]
            if click["kind"] == "tray":
                self.assertEqual(len(hit), 1, (label, "a tray click landed on one listed entry", click))
                expected.add((hit[0]["type_id"], hit[0]["price"]))
        card = next(card for card in report["command_bar"]["production_choices"] if card["button"] == "b_Create_00")
        expected.add((card["type_id"], card["price"]))
        for row in report["production_queues"]:
            self.assertIn((row["type"], row["price"]), expected, (label, "a queued order nobody clicked", row))
        self.assertGreaterEqual(max(row["queued"] for row in steps["log"]), 2, (label, steps["log"]))
        self.assertGreaterEqual(report["live_session"]["submitted_orders"], len(clicks), (label, report["live_session"]))

    def test_single_player_running_game(self):
        name = "sp"
        report, capture = self.directory / "sp.json", self.directory / "sp.png"
        driver = self.driver(name, role="sp", sp=True, settle=4000)
        arguments = ["--eawr-galactic", CAMPAIGN, "--eawr-game-root", GAME_ROOT, "--eawr-mod-root", str(self.mod),
                     "--eawr-galactic-paused", "no", "--eawr-galactic-dismiss-story", "yes",
                     "--eawr-settings-file", str(self.settings(name)), "--eawr-report", str(report),
                     "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on"]
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                                 "--path", str(ROOT / "apps/viewer/project"), "--script", str(driver), "--", *arguments],
                                cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        log = result.stdout + result.stderr
        (self.directory / "sp.log").write_text(log, encoding="utf-8")
        self.assertTrue(report.is_file(), log)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, data.get("failure") or log)
        self.assertFalse(data["paused"], "this case runs the clock")
        steps = json.loads((self.directory / "sp-steps.json").read_text(encoding="utf-8"))
        self.check_peer(data, steps, "single player, running")

    def test_two_human_multiplayer_host_and_guest(self):
        font_cache = self.directory / "fonts"
        extract = subprocess.run([sys.executable, str(TOOL), "--game-root", GAME_ROOT, "--out", str(font_cache),
                                  "--allow-unknown-build", "--json"], capture_output=True, text=True, timeout=120, check=False)
        self.assertEqual(extract.returncode, 0, extract.stdout + extract.stderr)
        port = free_ports(1)[0]
        processes, logs, peers = {}, {}, {}
        try:
            for role in ("host", "join"):
                peer = self.directory / role
                peer.mkdir()
                (peer / "saves").mkdir()
                peers[role] = peer
                driver = self.driver(role, role=role, sp=False, settle=6000)
                # The same lobby launch the GC acceptance rig uses; the driver plays the game once it starts.
                arguments = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720", "--max-fps", "30",
                             "--path", str(ROOT / "apps/viewer/project"), "--script", str(driver), "--",
                             "--eawr-game-root", GAME_ROOT, "--eawr-mod-root", str(self.mod),
                             "--eawr-font-cache", str(font_cache),
                             "--eawr-skirmish-setup", "--eawr-lan-acceptance", "--eawr-gc-lan-acceptance", CAMPAIGN,
                             "--eawr-gc-lan-final-tick", "6000", "--eawr-gc-lan-out", str(peer),
                             "--eawr-lan-input-delay", "3", "--eawr-galactic-save-dir", str(peer / "saves"),
                             "--eawr-setup-profile", str(peer / "profile.cfg"),
                             "--eawr-settings-file", str(self.settings(role))]
                arguments += (["--eawr-lan-host", str(port)] if role == "host" else
                              ["--eawr-lan-join", "127.0.0.1:" + str(port)])
                logs[role] = (peer / "viewer.log").open("w", encoding="utf-8")
                processes[role] = subprocess.Popen(arguments, cwd=ROOT, stdout=logs[role], stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 900
            for role, process in processes.items():
                try:
                    code = process.wait(timeout=max(1.0, deadline - time.monotonic()))
                except subprocess.TimeoutExpired:
                    code = None
                tail = (peers[role] / "viewer.log").read_text(encoding="utf-8", errors="replace")[-6000:]
                self.assertEqual(code, 0, role + " viewer: " + tail)
        finally:
            for process in processes.values():
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=10)
            for log in logs.values():
                log.close()
        acceptance = {}
        for role, peer in peers.items():
            report = json.loads((peer / "report.json").read_text(encoding="utf-8"))
            steps = json.loads((self.directory / (role + "-steps.json")).read_text(encoding="utf-8"))
            acceptance[role] = json.loads((peer / "gc.json").read_text(encoding="utf-8"))
            self.assertEqual(report["mode"], "galactic", (role, report.get("failure")))
            self.assertFalse(report.get("failure"), (role, report.get("failure")))
            self.check_peer(report, steps, "multiplayer " + role)
        self.assertEqual(len({item["local_seat"] for item in acceptance.values()}), 2, acceptance)
        self.assertTrue(all(not item["desync"] for item in acceptance.values()), acceptance)
        self.assertEqual(acceptance["host"]["final_state_sha256"], acceptance["join"]["final_state_sha256"],
                         "both peers stayed in sync with the tray orders in the match")


if __name__ == "__main__":
    unittest.main()
