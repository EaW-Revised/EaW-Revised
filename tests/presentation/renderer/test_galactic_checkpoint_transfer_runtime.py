"""Two low-fps viewers load a private multiplayer checkpoint and retain wire_ms receipts.

The rig supplies EAWR_JOB_CHECKPOINT through an ExtraArtifact mapping, or generates
a private current-format substitute with EAWR_JOB_GENERATE_CHECKPOINT=1.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_lan_lobby as lobby  # noqa: E402

FIXTURE_DRIVER = '''extends SceneTree
var ready_at = -1
var requested = false
var save_phase = 0
var started = Time.get_ticks_msec()
func _initialize():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func _process(_delta):
    for node in root.find_children("*", "Button", true, false):
        if node.is_visible_in_tree() and node.text == "OK":
            node.emit_signal("pressed")
            break
    if not HOST:
        if FileAccess.file_exists(STOP): quit(0)
        return false
    if Time.get_ticks_msec() - started > 600000:
        root.get_texture().get_image().save_png(CAPTURE)
        push_error("substitute checkpoint generation timed out")
        quit(2)
        return false
    if root.find_child("GalacticWorldOverlays", true, false) == null: return false
    if ready_at < 0:
        ready_at = Time.get_ticks_msec()
        print("checkpoint-rig substitute campaign ready")
    if Time.get_ticks_msec() - ready_at > 150000 and not requested and root.find_child("CampaignSpaceBattleReady", true, false) == null:
        requested = true
        for down in [true, false]:
            var key = InputEventKey.new()
            key.keycode = KEY_ESCAPE
            key.physical_keycode = KEY_ESCAPE
            key.pressed = down
            root.push_input(key, true)
        save_phase = 1
        print("checkpoint-rig substitute ordinary save menu opened")
    var menu = root.find_child("GalacticSaveMenu", true, false)
    if requested and menu != null:
        if save_phase == 1:
            var escape = menu.find_child("IDD_GAME_OPTIONS_DIALOG", true, false)
            var open_save = escape.find_child("IDC_SAVE_GAME", true, false) if escape != null else null
            if open_save != null and open_save.is_visible_in_tree() and not open_save.disabled:
                open_save.emit_signal("pressed")
                save_phase = 2
            return false
        if save_phase == 2:
            var save = menu.find_child("IDD_SAVE_GAME_DIALOG", true, false)
            var name_edit = save.find_child("SaveName", true, false) if save != null else null
            var save_button = save.find_child("IDC_SAVE_GAME_BUTTON", true, false) if save != null else null
            if name_edit != null and save_button != null and save_button.is_visible_in_tree():
                name_edit.text = "Transfer Fixture"
                if not save_button.disabled:
                    save_button.emit_signal("pressed")
                    save_phase = 3
                    print("checkpoint-rig substitute ordinary save requested")
            return false
        var acknowledgement = menu.find_child("IDD_DIALOG_BOX1", true, false)
        if acknowledgement != null and acknowledgement.is_visible_in_tree():
            for node in acknowledgement.find_children("*", "", true, false):
                if node.has_meta("save_caption"):
                    print("checkpoint-rig save acknowledgement=", node.get_meta("save_caption"))
            var path = SAVES + "/Transfer Fixture.eawrsave"
            var bytes = FileAccess.get_file_as_bytes(path)
            if bytes.size() < 304:
                root.get_texture().get_image().save_png(CAPTURE)
                push_error("substitute save acknowledgement has no complete checkpoint")
                quit(2)
                return false
            var output = FileAccess.open(FIXTURE, FileAccess.WRITE)
            output.store_string(JSON.stringify({"eawrsave_base64":Marshalls.raw_to_base64(bytes)}))
            output.close()
            root.get_texture().get_image().save_png(CAPTURE)
            print("checkpoint-rig substitute bytes=", bytes.size(), " frame=", bytes.decode_u64(200))
            quit(0)
    return false
'''

DRIVER = '''extends SceneTree
var applied = false
var frames = 0
var last_sample = 0
var max_sample_gap = 0
var sampled_frames = 0
var total_sample_gap = 0
var disabled_reasons = {}
var summary_text = ""
func _initialize():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
    Engine.max_fps = 4
func _process(_delta):
    # The production pacing settings clamp caps below 30 fps. Delay this foreground
    # loop to exercise 250 ms frames while the native transfer worker keeps running.
    OS.delay_msec(250)
    frames += 1
    for summary in root.find_children("CheckpointSummary", "", true, false):
        if summary is ItemList:
            var rows = PackedStringArray()
            for index in range(summary.item_count):
                rows.append(summary.get_item_text(index))
            var next_text = " | ".join(rows)
            if next_text != summary_text:
                summary_text = next_text
                print("checkpoint-rig summary=", summary_text)
    if FileAccess.file_exists(SAMPLE):
        var now = Time.get_ticks_msec()
        if last_sample > 0:
            max_sample_gap = max(max_sample_gap, now - last_sample)
            total_sample_gap += now - last_sample
        last_sample = now
        sampled_frames += 1
    if FileAccess.file_exists(STOP):
        var metrics = FileAccess.open(METRICS, FileAccess.WRITE)
        metrics.store_string(JSON.stringify({"sampled_frames":sampled_frames,"max_sample_gap_ms":max_sample_gap,"total_sample_gap_ms":total_sample_gap}))
        metrics.close()
        root.get_texture().get_image().save_png(CAPTURE)
        quit(0)
        return false
    if HOST and not applied:
        for node in root.find_children("*", "", true, false):
            if node is BaseButton and node.is_visible_in_tree():
                if node.disabled:
                    var reason = node.get_tooltip_text()
                    if not reason.is_empty() and not disabled_reasons.has(reason):
                        disabled_reasons[reason] = true
                        print("checkpoint-rig disabled button=", node.name, " reason=", reason)
                    continue
                # The checkpoint panel holds its summary, one saved-seat combo per peer, and Apply.
                # Its native caption getter is not exposed to scripts; select the button by that visible panel.
                var seat_combos = 0
                var summaries = 0
                for sibling in node.get_parent().get_children():
                    seat_combos += int(sibling.get_class() == "EawrUiCombo")
                    summaries += int(sibling.name == "CheckpointSummary")
                if node.get_class() == "EawrUiButton" and seat_combos == 2 and summaries == 1:
                    applied = true
                    node.emit_signal("pressed")
                    print("checkpoint-rig applied saved seats at frame=", frames)
                    break
    return false
'''


@unittest.skipUnless(lobby.RUNTIME and lobby.GAME_ROOT and (os.environ.get("EAWR_JOB_CHECKPOINT")
                     or os.environ.get("EAWR_JOB_GENERATE_CHECKPOINT")),
                     "requires GPU viewer, installed game and a private multiplayer checkpoint artifact")
class CheckpointTransferRuntime(unittest.TestCase):
    def test_two_low_fps_viewers_transfer_checkpoint(self):
        directory = Path(tempfile.mkdtemp(prefix="checkpoint-transfer-"))
        saves = directory / "saves"
        saves.mkdir()
        font_cache = directory / "fonts"
        extract = subprocess.run([sys.executable, str(lobby.TOOL), "--game-root", lobby.GAME_ROOT,
                                  "--out", str(font_cache), "--allow-unknown-build", "--json"],
                                 capture_output=True, text=True, timeout=120, check=False)
        self.assertEqual(extract.returncode, 0, extract.stdout + extract.stderr)
        fixture_kind = "provided-private-checkpoint"
        if os.environ.get("EAWR_JOB_GENERATE_CHECKPOINT"):
            fixture_kind = "current-format-substitute"
            with socket.socket() as listener:
                listener.bind(("127.0.0.1", 0))
                fixture_port = listener.getsockname()[1]
            fixture_stop = directory / "fixture-stop"
            fixture_processes, fixture_logs = [], []
            fixture_saves = directory / "fixture-host" / "saves"
            try:
                for role in ("host", "join"):
                    peer = directory / ("fixture-" + role)
                    peer.mkdir()
                    (peer / "saves").mkdir()
                    fixture_driver = peer / "driver.gd"
                    fixture_driver.write_text(FIXTURE_DRIVER.replace("SAVES", json.dumps(str(peer / "saves")))
                                              .replace("FIXTURE", json.dumps(str(directory / "private-fixture.json")))
                                              .replace("CAPTURE", json.dumps(str(directory / "fixture.png")))
                                              .replace("STOP", json.dumps(str(fixture_stop)))
                                              .replace("HOST", "true" if role == "host" else "false"), encoding="utf-8")
                    # The existing GC LAN acceptance launcher prepares and starts a real room.
                    # Preview mode keeps single-player save classification and cannot supply this fixture.
                    arguments = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                                 "--path", str(lobby.ROOT / "apps/viewer/project"), "--script", str(fixture_driver), "--",
                                 "--eawr-game-root", lobby.GAME_ROOT, "--eawr-font-cache", str(font_cache),
                                 "--eawr-skirmish-setup", "--eawr-lan-acceptance",
                                 "--eawr-gc-lan-acceptance", "Sandbox_Equal_Footing_Empire",
                                 "--eawr-gc-lan-final-tick", "30000", "--eawr-gc-lan-out", str(peer),
                                 "--eawr-lan-input-delay", "3", "--eawr-galactic-save-dir", str(peer / "saves"),
                                 "--eawr-setup-profile", str(peer / "profile.cfg"),
                                 "--eawr-settings-file", str(peer / "settings.cfg")]
                    arguments += (["--eawr-lan-host", str(fixture_port)] if role == "host" else
                                  ["--eawr-lan-join", "127.0.0.1:" + str(fixture_port)])
                    log = (peer / "viewer.log").open("w", encoding="utf-8")
                    fixture_logs.append(log)
                    fixture_processes.append(subprocess.Popen(arguments, cwd=lobby.ROOT, stdout=log,
                                                               stderr=subprocess.STDOUT))
                code = fixture_processes[0].wait(timeout=660)
                self.assertEqual(code, 0, (directory / "fixture-host" / "viewer.log").read_text(encoding="utf-8")[-6000:])
            finally:
                fixture_stop.touch()
                for process in fixture_processes:
                    if process.poll() is None:
                        try:
                            process.wait(timeout=20)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait(timeout=10)
                for log in fixture_logs:
                    log.close()
            save = fixture_saves / "Transfer Fixture.eawrsave"
        else:
            save = Path(os.environ["EAWR_JOB_CHECKPOINT"])
        self.assertTrue(save.is_file())
        payload = save.read_bytes()
        fixture_frame = struct.unpack_from("<Q", payload, 200)[0]
        fixture_section_version = struct.unpack_from("<H", payload, 306)[0]
        fixture_multiplayer = False
        offset = 304
        while offset < len(payload):
            section, _, _, _, size = struct.unpack_from("<HHHHQ", payload, offset)
            if section == 0x8002:
                cursor = offset + 16
                for _ in range(4):
                    length = struct.unpack_from("<I", payload, cursor)[0]
                    cursor += 4 + length
                fixture_multiplayer = bool(payload[cursor + 4])
            offset += 16 + size
        if fixture_kind == "current-format-substitute":
            self.assertTrue(fixture_multiplayer, "the fixture must be captured from a real multiplayer session")
            self.assertGreaterEqual(fixture_frame, 3 * 45 * 30, "the substitute advances about three stock game days")
            self.assertGreaterEqual(len(payload), 9 * 1024 * 1024, "the substitute is near the owner's 10 MiB size")
            self.assertLess(len(payload), 13 * 1024 * 1024)
        shutil.copyfile(save, saves / "Transfer.eawrsave")
        host_port, guest_port = lobby.free_udp_ports(2)
        game = "Checkpoint transfer rig"
        scripts = {
            "host": ["host_options", "host_dropdown:game_type=TEXT_GAME_TYPE_CAMPAIGN", "host:" + game,
                     "wait:room_player:Transfer guest@180", "wait:content_ready@180", "load_game",
                     "wait:load_list@180", "sleep:10", "click_row:0", "sleep:5", "click:load:IDC_LOAD_GAME_BUTTON",
                     "wait:load_list_closed@60", "sleep:20000"],
            "guest": ["wait:game:" + game + "@180", "join:" + game,
                      "wait:room_player:Transfer guest@180", "sleep:20000"],
        }
        stop = directory / "stop"
        sample = directory / "sample-start"
        processes, logs, receipts = {}, {}, {}
        try:
            for role, port, other in (("host", host_port, guest_port), ("guest", guest_port, host_port)):
                settings = directory / (role + "-settings.cfg")
                settings.write_text("eawr-settings 1\nvideo.frame_rate_limit=4\nvideo.vsync=0\n",
                                    encoding="utf-8")
                driver = directory / (role + "-driver.gd")
                driver.write_text(DRIVER.replace("STOP", json.dumps(str(stop)))
                                  .replace("SAMPLE", json.dumps(str(sample)))
                                  .replace("METRICS", json.dumps(str(directory / (role + "-frames.json"))))
                                  .replace("CAPTURE", json.dumps(str(directory / (role + ".png"))))
                                  .replace("HOST", "true" if role == "host" else "false"), encoding="utf-8")
                actions = [item for step in scripts[role] for item in ("--eawr-lan-do", step)]
                command = lobby.command(font_cache, role, directory, directory / (role + "-final.png"),
                                        "--eawr-lan-name", "Transfer " + role,
                                        "--eawr-lan-broadcast-port", str(port),
                                        "--eawr-lan-broadcast-extra", str(other), "--eawr-lan-game-port", "0",
                                        "--eawr-settings-file", str(settings),
                                        "--eawr-save-dir", str(saves), *actions,
                                        engine=("--script", str(driver), "--max-fps", "4",
                                                "--log-file", str(directory / (role + "-engine.log"))))
                logs[role] = (directory / (role + ".log")).open("w", encoding="utf-8")
                processes[role] = subprocess.Popen(command, cwd=lobby.ROOT, stdout=logs[role],
                                                   stderr=subprocess.STDOUT, text=True)
            deadline = time.monotonic() + 420
            while len(receipts) != 2 and time.monotonic() < deadline:
                for role, process in processes.items():
                    output = (directory / (role + ".log")).read_text(encoding="utf-8", errors="replace")
                    self.assertNotIn("EAWR checkpoint refused:", output,
                                     "checkpoint preparation refused before transfer:\n" + output[-6000:])
                    if role == "guest" and "reason=chunk " in output:
                        sample.touch(exist_ok=True)
                    lines = [line for line in output.splitlines()
                             if "checkpoint-transfer" in line and "role=" + role in line and "wire_ms=" in line]
                    if lines:
                        line = lines[-1]
                        metrics = {key: int(value) for key, value in re.findall(r"(bytes|wire_ms|restore_ms|pumps|pump_ms)=(\d+)", line)}
                        receipts[role] = {"line": line, **metrics}
                    self.assertIsNone(process.poll(), role + " exited before the transfer receipt:\n" + output[-6000:])
                time.sleep(0.25)
            self.assertEqual(set(receipts), {"host", "guest"}, str(directory))
            for receipt in receipts.values():
                self.assertEqual(receipt["bytes"], save.stat().st_size)
                self.assertLess(receipt["wire_ms"], 2000, "checkpoint wire time stays under 2 s at 4 fps")
            guest_log = (directory / "guest.log").read_text(encoding="utf-8", errors="replace")
            progress = [int(value) for value in re.findall(r"reason=chunk .*? received=(\d+)", guest_log)]
            self.assertGreater(len(set(progress)), 1, "the transfer publishes incremental byte counters")
            self.assertEqual(progress, sorted(progress), "byte progress is monotonic")
            result = {"progress_samples": len(progress), "source_sha256": hashlib.sha256(save.read_bytes()).hexdigest(),
                      "bytes": save.stat().st_size, "max_fps": 4, "receipts": receipts,
                      "fixture_kind": fixture_kind, "fixture_frame": fixture_frame,
                      "fixture_section_version": fixture_section_version, "fixture_multiplayer": fixture_multiplayer}
            if fixture_kind == "current-format-substitute":
                result["fixture_campaign"] = "Sandbox_Equal_Footing_Empire"
                result["fixture_human_seats"] = 2
                result["fixture_generation"] = "real-two-peer-gc-lan-launch"
            (directory / "transfer-receipt.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
            print("checkpoint rig timing: " + json.dumps(result), flush=True)
            stop.touch()
            for role, process in processes.items():
                self.assertEqual(process.wait(timeout=30), 0, role)
                frame_metrics = json.loads((directory / (role + "-frames.json")).read_text(encoding="utf-8"))
                result[role + "_frame_metrics"] = frame_metrics
                self.assertGreater(frame_metrics["sampled_frames"], 1, "both viewers keep drawing during transfer/restore")
                self.assertGreaterEqual(frame_metrics["total_sample_gap_ms"] / (frame_metrics["sampled_frames"] - 1),
                                        240, "the viewer foreground frames remain deliberately slow")
            (directory / "transfer-receipt.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        finally:
            stop.touch()
            for process in processes.values():
                if process.poll() is None:
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=10)
            for log in logs.values():
                log.close()


if __name__ == "__main__":
    unittest.main()
