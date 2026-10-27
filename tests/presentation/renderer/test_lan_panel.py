"""LAN facade presentation through the ordinary skirmish setup screen."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "requires the viewer GPU lane and installed game data")
class LanPanelGpu(unittest.TestCase):
    def test_joiner_result_notice_stays_above_populated_loss_panes(self):
        import json
        with tempfile.TemporaryDirectory(prefix="lan-result-join-") as temporary:
            output = Path(os.environ.get("EAWR_LAN_EYECHECK", str(Path(temporary).parent))) / "lan-result-join"
            output.mkdir(parents=True, exist_ok=True)
            report = output / "join-result.json"
            result = subprocess.run([
                os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-live-session", "m2",
                "--eawr-map", "data/art/maps/_mp_space_coruscant.ted", "--eawr-populate",
                "--eawr-live-lan-test", "result_join", "--eawr-live-player", "2",
                "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                "--eawr-report", str(report), "--eawr-capture", str(output / "join-result.png"),
                "--eawr-live-order", "5:damage:11@1000000", "--eawr-live-order", "15:damage:1@1000000",
                "--eawr-live-ticks", "300", "--eawr-live-step", "3", "--eawr-live-capture-frames", "80",
                "--eawr-live-ai", "off", "--eawr-audio", "off",
                "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on"],
                cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600)
            (output / "join-result.log").write_text(result.stdout, encoding="utf-8")
            self.assertEqual(result.returncode, 0, result.stdout[-6000:])
            data = json.loads(report.read_text(encoding="utf-8"))
            live = data["live_session"]
            self.assertEqual(live["local_player"], 2)
            self.assertEqual(live["phase"], "results")
            self.assertGreater(sum(side["total"] for side in live["results"]["sides"]), 0)
            panel = data["hud"]["battle_overlay"]["end_panel"]
            self.assertTrue(panel["authored_dialog"])
            self.assertEqual(panel["status"], "Victory - opponent left.")
            x, y, width, height = panel["status_rect"]
            self.assertGreater(width, 0)
            self.assertGreater(height, 0)
            self.assertLessEqual(y + height, panel["losses_top"] + 0.01)
            self.assertGreater((output / "join-result_f0080.png").stat().st_size, 10000)

    def test_pending_battle_leave_controls(self):
        import json
        with tempfile.TemporaryDirectory(prefix="lan-abort-") as temporary:
            output = Path(os.environ.get("EAWR_LAN_EYECHECK", temporary)).resolve() / "abort"
            output.mkdir(parents=True, exist_ok=True)
            for state in ("barrier", "peer_left", "failed_peer"):
                for gesture in ("click:screen=640,406", "key:Escape"):
                    with self.subTest(state=state, gesture=gesture):
                        name = state + ("-button" if gesture.startswith("click") else "-escape")
                        report = output / (name + ".json")
                        capture_args = (["--eawr-camera-interactive"] if state == "failed_peer" else
                            ["--eawr-capture", str(output / (name + ".png")), "--eawr-live-capture-frames", "10"])
                        result = subprocess.run([
                            os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                            "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-live-session", "m2",
                            "--eawr-map", "data/art/maps/_mp_space_coruscant.ted", "--eawr-populate",
                            "--eawr-live-lan-test", state, "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                            "--eawr-report", str(report), *capture_args, "--eawr-live-input", "f30:" + gesture,
                            "--eawr-live-ticks", "10000", "--eawr-live-ai", "off", "--eawr-audio", "off",
                            "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on"],
                            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600)
                        (output / (name + ".log")).write_text(result.stdout, encoding="utf-8")
                        self.assertEqual(result.returncode, 0, result.stdout[-6000:])
                        live = json.loads(report.read_text(encoding="utf-8"))["live_session"]
                        self.assertEqual(live["phase"], "returning", "Leave returns without an agreed quit tick")
                        if state == "failed_peer":
                            self.assertIn("EAWR-NET-0305", live["error"])
                            self.assertEqual(live["completed_ticks"], 0)
                        else:
                            self.assertTrue((output / (name + "_f0010.png")).exists())

            for state, title in (("result", "You have won the battle!"), ("loss", "You have lost the battle!")):
                with self.subTest(state=state, gesture="key:Escape"):
                    report = output / (state + "-escape.json")
                    result = subprocess.run([
                        os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                        "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-live-session", "m2",
                        "--eawr-map", "data/art/maps/_mp_space_coruscant.ted", "--eawr-populate",
                        "--eawr-live-lan-test", state, "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                        "--eawr-report", str(report), "--eawr-capture", str(output / (state + "-escape.png")),
                        "--eawr-live-capture-frames", "60", "--eawr-live-input", "f30:key:Escape",
                        "--eawr-live-ticks", "1", "--eawr-live-ai", "off", "--eawr-audio", "off",
                        "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on"],
                        cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600)
                    (output / (state + "-escape.log")).write_text(result.stdout, encoding="utf-8")
                    self.assertEqual(result.returncode, 0, result.stdout[-6000:])
                    data = json.loads(report.read_text(encoding="utf-8"))
                    self.assertNotEqual(data["live_session"]["phase"], "returning", "decided Escape keeps the battle view")
                    self.assertEqual(data["hud"]["battle_overlay"]["end_panel"]["title"], title)

    def test_facade_states(self):
        with tempfile.TemporaryDirectory(prefix="lan-panel-") as temporary:
            output = Path(os.environ.get("EAWR_LAN_EYECHECK", temporary)).resolve()
            output.mkdir(parents=True, exist_ok=True)
            cases = {"host_waiting": "Waiting for a player to join", "joined": "Connected - waiting for host setup", "review": "Review the host setup",
                     "refused": "different game build", "waiting": "Waiting for Opponent",
                     "desync": "Desync at tick 120", "result": "opponent left", "loss": "Defeat"}
            for state, expected in cases.items():
                with self.subTest(state=state):
                    capture = output / (state + ".png")
                    result = subprocess.run([
                        os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                        "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-skirmish-setup",
                        "--eawr-lan-panel-test", state, "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                        "--eawr-capture", str(capture)], cwd=ROOT, text=True,
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
                    (output / (state + ".log")).write_text(result.stdout, encoding="utf-8")
                    self.assertEqual(result.returncode, 0, result.stdout[-6000:])
                    self.assertIn("EAWR LAN panel: " + state, result.stdout)
                    self.assertIn(expected, result.stdout)
                    self.assertTrue(capture.exists())
                    self.assertGreater(capture.stat().st_size, 1000)

    def test_multiplayer_hud_states(self):
        import json
        with tempfile.TemporaryDirectory(prefix="lan-hud-") as temporary:
            output = Path(os.environ.get("EAWR_LAN_EYECHECK", temporary)).resolve() / "hud"
            output.mkdir(parents=True, exist_ok=True)
            for state in ("waiting", "desync", "result", "loss"):
                with self.subTest(state=state):
                    report = output / (state + ".json")
                    result = subprocess.run([
                        os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                        "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-live-session", "m2",
                        "--eawr-map", "data/art/maps/_mp_space_coruscant.ted", "--eawr-populate",
                        "--eawr-live-lan-test", state, "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                        "--eawr-report", str(report), "--eawr-capture", str(output / (state + ".png")),
                        "--eawr-live-ticks", "1", "--eawr-live-ai", "off", "--eawr-audio", "off",
                        "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on"],
                        cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600)
                    (output / (state + ".log")).write_text(result.stdout, encoding="utf-8")
                    self.assertEqual(result.returncode, 0, result.stdout[-6000:])
                    data = json.loads(report.read_text(encoding="utf-8"))
                    live = data["live_session"]
                    self.assertTrue(live["multiplayer"])
                    self.assertTrue(live["lan_message"])
                    for button in ("pause", "fast_forward"):
                        control = data["hud"]["time_panel"][button]
                        if button == "fast_forward":
                            self.assertTrue(control["disabled"])
                            self.assertIn("unavailable in multiplayer", control["tooltip"])
                        self.assertIn("Any active human", control["tooltip"])
                        self.assertIn("Shared speed: 2", control["tooltip"])

    def test_shared_pause_actor_and_resume_button(self):
        import json
        with tempfile.TemporaryDirectory(prefix="mp-pause-") as temporary:
            output = Path(os.environ.get("EAWR_LAN_EYECHECK", temporary)).resolve() / "pause"
            output.mkdir(parents=True, exist_ok=True)
            for resume in (False, True):
                name = "resumed" if resume else "paused"
                report = output / (name + ".json")
                command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                    "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-live-session", "m2",
                    "--eawr-map", "data/art/maps/_mp_space_coruscant.ted", "--eawr-populate",
                    "--eawr-live-lan-test", "paused", "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                    "--eawr-report", str(report), "--eawr-capture", str(output / (name + ".png")),
                    "--eawr-live-capture-frames", "90", "--eawr-live-ticks", "60", "--eawr-live-ai", "off",
                    "--eawr-audio", "off", "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on"]
                if resume:
                    command += ["--eawr-live-input", "f30:click:hud=resume"]
                result = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, timeout=600)
                (output / (name + ".log")).write_text(result.stdout, encoding="utf-8")
                self.assertEqual(result.returncode, 0, result.stdout[-6000:])
                data = json.loads(report.read_text(encoding="utf-8"))
                banner = data["hud"]["battle_overlay"]["paused_banner"]
                if resume:
                    self.assertIsNone(banner, "the authored Resume button releases the shared hold")
                else:
                    self.assertEqual(banner["text"], "Paused by Leia")
                    self.assertFalse(data["hud"]["time_panel"]["pause"]["disabled"])
                    self.assertTrue(data["hud"]["time_panel"]["fast_forward"]["disabled"])

    def test_two_viewer_loopback_with_ai(self):
        import csv
        import hashlib
        import json
        import socket
        import time
        with tempfile.TemporaryDirectory(prefix="lan-loopback-") as temporary:
            output = Path(os.environ.get("EAWR_LAN_EYECHECK", temporary)).resolve() / "loopback"
            output.mkdir(parents=True, exist_ok=True)
            # Ask the OS for a free port, then use the real viewer Host and Join paths.
            with socket.socket() as reserve:
                reserve.bind(("127.0.0.1", 0))
                port = reserve.getsockname()[1]
            processes = []
            logs = []
            try:
                for side in ("host", "join"):
                    log = (output / (side + ".log")).open("w", encoding="utf-8")
                    logs.append(log)
                    command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720", "--max-fps", "30",
                        "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-skirmish-setup",
                        "--eawr-lan-smoke", "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                        "--eawr-report", str(output / (side + ".json")),
                        "--eawr-capture", str(output / (side + ".png")),
                        "--eawr-live-hashes", str(output / (side + ".csv")),
                        "--eawr-live-replay-out", str(output / (side + ".eawr-replay")),
                        "--eawr-live-workers", "2", "--eawr-audio", "off",
                        "--eawr-live-lan-quit-tick", "3637" if side == "host" else "0"]
                    command += ["--eawr-lan-host", str(port)] if side == "host" else ["--eawr-lan-join", "127.0.0.1:" + str(port)]
                    processes.append(subprocess.Popen(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT))
                    if side == "host":
                        # The setup capture follows the first-frame Host action. Wait for
                        # that observable state so cold asset/shader loading cannot race Join.
                        ready = time.monotonic() + 180
                        while not (output / "host.png").exists():
                            self.assertIsNone(processes[0].poll(), "host exited before its setup capture")
                            self.assertLess(time.monotonic(), ready, "host setup/listener readiness deadline")
                            time.sleep(0.1)
                for process in processes: process.wait(timeout=900)
            finally:
                for process in processes:
                    if process.poll() is None: process.kill(); process.wait(timeout=30)
                for log in logs: log.close()
            reports = []
            hashes = []
            for index, side in enumerate(("host", "join")):
                log = (output / (side + ".log")).read_text(encoding="utf-8")
                self.assertEqual(processes[index].returncode, 0, log[-10000:])
                report = json.loads((output / (side + ".json")).read_text(encoding="utf-8"))["live_session"]
                reports.append(report)
                self.assertTrue(report["multiplayer"])
                self.assertEqual(report["local_player"], index + 1)
                self.assertEqual(report["ai"]["players"], [3])
                self.assertGreaterEqual(report["ai"]["ticks"], 3637)
                self.assertIn("left", report["lan_message"])
                self.assertTrue((output / (side + ".battle.png")).exists())
                with (output / (side + ".csv")).open(newline="", encoding="utf-8") as source:
                    hashes.append(list(csv.DictReader(source)))
            self.assertGreaterEqual(len(hashes[0]), 3637)
            self.assertEqual(hashes[0], hashes[1], "each peer's full world hash stream, including AI and agreed quit")
            self.assertEqual(reports[0]["outcome"]["local_result"], "defeat")
            self.assertEqual(reports[1]["outcome"]["local_result"], "victory")
            # Rig packaging retains JSON evidence after temporary CSV/replay cleanup.
            (output / "paired-evidence.json").write_text(json.dumps({
                "hash_rows": len(hashes[0]), "full_hash_streams_equal": True,
                "sides": {side: {
                    "hash_csv_sha256": hashlib.sha256((output / (side + ".csv")).read_bytes()).hexdigest(),
                    "replay_sha256": hashlib.sha256((output / (side + ".eawr-replay")).read_bytes()).hexdigest(),
                    "local_player": reports[index]["local_player"],
                    "ai": reports[index]["ai"], "outcome": reports[index]["outcome"],
                    "lan_message": reports[index]["lan_message"],
                } for index, side in enumerate(("host", "join"))},
            }, indent=2) + "\n", encoding="utf-8")
