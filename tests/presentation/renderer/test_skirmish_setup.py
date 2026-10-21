"""Opt-in GUI-to-battle contract for #948; run through a private GPU desktop."""

import csv
import json
import os
import re
import shutil
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import unittest
import zipfile
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parent))
from capture_replay_cases import canonical_replay_header

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "requires the viewer GPU lane and installed game data")
class SkirmishSetupGpu(unittest.TestCase):
    def test_ninth_local_row_opens_closes_and_starts_nine_seats(self):
        from PIL import Image, ImageChops
        with tempfile.TemporaryDirectory(prefix="setup-nine-") as temporary:
            for seats in (8, 9):
                output = Path(temporary) / str(seats)
                output.mkdir()
                report = output / "battle.json"
                probe = "--eawr-setup-nine-player-test" if seats == 9 else "--eawr-setup-eight-player-test"
                result = subprocess.run([
                    os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                    "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-skirmish-setup",
                    "--eawr-setup-test", probe, "--eawr-game-root",
                    os.environ["EAWR_EAW_GAME_ROOT"], "--eawr-report", str(report),
                    "--eawr-capture", str(output / "setup.png"), "--eawr-font-cache",
                    os.environ.get("EAWR_FONT_CACHE", str(ROOT / "out/fonts")),
                    "--eawr-live-ticks", "2", "--eawr-live-ai", "off", "--eawr-audio", "off",
                    "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on"],
                    cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=240)
                (output / "setup.log").write_text(result.stdout, encoding="utf-8")
                self.assertEqual(result.returncode, 0, result.stdout[-6000:])
                states = re.findall(r"EAWR setup last row: live=(\d) controller=(\d) occupied=(\d) start=(\d)", result.stdout)
                self.assertEqual(states, [("1", "2", str(seats), "1"), ("1", "0", str(seats - 1), "1"),
                                          ("1", "2", str(seats), "1")])
                number = r"(-?\d+(?:\.\d+)?)"
                rects = {}
                for column in ("NAME", "FACTION", "COLOR", "TEAM"):
                    found = re.search(r"EAWR setup layout IDC_PLAYER_" + column + "_" + str(seats - 1)
                                      + " authored=" + ",".join([number] * 4) + r" actual=" + ",".join([number] * 4)
                                      + r" visible=1", result.stdout)
                    self.assertIsNotNone(found, column)
                    rects[column] = tuple(float(value) for value in found.groups()[4:])
                    x, y, width, height = rects[column]
                    self.assertTrue(0 <= x < x + width <= 1280 and 0 <= y < y + height <= 720, rects[column])
                for left, right in zip(rects.values(), list(rects.values())[1:]):
                    self.assertGreater(right[0], left[0] + left[2])
                status = re.search(r"EAWR setup status rect: " + ",".join([number] * 4), result.stdout)
                self.assertIsNotNone(status)
                x, y, width, height = (float(value) for value in status.groups())
                self.assertGreaterEqual(height, 20, "the status keeps a readable text band")
                self.assertGreaterEqual(y, max(rect[1] + rect[3] for rect in rects.values()))
                box = (int(x), int(y), int(x + width), int(y + height))
                visible = Image.open(output / "status-visible.png").convert("RGB").crop(box)
                hidden = Image.open(output / "status-hidden.png").convert("RGB").crop(box)
                changed = ImageChops.difference(visible, hidden).convert("L")
                self.assertGreater(sum(value > 8 for value in changed.getdata()), 30,
                                   "status text draws visible framebuffer pixels with eight and nine seats")
                live = json.loads(report.read_text(encoding="utf-8"))["live_session"]
                self.assertEqual([row["slot"] for row in live["start_slots"]], list(range(1, seats + 1)))
                self.assertEqual([row["human"] for row in live["start_slots"]], [True] + [False] * (seats - 1))
                self.assertTrue(live["headless_hashes_equal"])
                self.assertGreater((output / "setup.png").stat().st_size, 10000)

    @staticmethod
    def _command_players(path):
        """Read replay framing independently; count orders by their actual issuer."""
        data = path.read_bytes()
        if data[:8] != b"EAWRPLY\x00":
            raise ValueError("not a tactical replay")
        players, squadrons, units, commands = struct.unpack_from("<IIQQ", data, 48)
        offset = struct.unpack_from("<H", data, 10)[0] + players * 24 + units * 80
        for _ in range(squadrons):
            count = struct.unpack_from("<I", data, offset + 8)[0]
            offset += 16 + count * 8
        issued = {}
        for _ in range(commands):
            size = struct.unpack_from("<I", data, offset)[0]
            player = struct.unpack_from("<I", data, offset + 12)[0]
            issued[player] = issued.get(player, 0) + 1
            offset += 4 + size
        if offset != len(data):
            raise ValueError("unexpected replay framing")
        return issued

    def test_ffa_cli_stock_maps(self):
        with tempfile.TemporaryDirectory(prefix="skirmish-ffa-") as temporary:
            output = Path(os.environ.get("EAWR_SETUP_EYECHECK", temporary)).resolve() / "ffa-cli"
            output.mkdir(parents=True, exist_ok=True)
            for name in ("coruscant", "themaw", "bothawui", "bespin", "ryloth", "felucia", "kamino", "saleucami"):
                with self.subTest(map=name):
                    report, replay = output / (name + ".json"), output / (name + ".eawr-replay")
                    result = subprocess.run([
                        os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                        "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-live-session", "skirmish",
                        "--eawr-populate", "--eawr-map", f"data/art/maps/_mp_space_{name}.ted",
                        "--eawr-skirmish-players", "1,2,3",
                        "--eawr-skirmish-slot", "1:Empire:0:human",
                        "--eawr-skirmish-slot", "2:Empire:1:ai",
                        "--eawr-skirmish-slot", "3:Empire:2:ai",
                        "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                        "--eawr-report", str(report), "--eawr-live-replay-out", str(replay),
                        "--eawr-capture", str(output / (name + ".png")),
                        "--eawr-live-ticks", "900", "--eawr-live-ai", "on", "--eawr-audio", "off",
                        "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on"],
                        cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=600)
                    (output / (name + ".log")).write_text(result.stdout, encoding="utf-8")
                    data = json.loads(report.read_text(encoding="utf-8"))
                    if name in ("coruscant", "themaw", "bothawui", "bespin"):
                        self.assertIn("capacity" if name == "bespin" else "authored start", data["failure"])
                        continue
                    self.assertEqual(result.returncode, 0, result.stdout[-6000:])
                    live = data["live_session"]
                    self.assertEqual(live["ai"]["players"], [2, 3])
                    self.assertTrue(live["headless_hashes_equal"])
                    self.assertEqual({row["player"] for row in live["start_fleet"]}, {1, 2, 3})
                    self.assertEqual({row["player"] for row in live["rendered_colours"] if 1 <= row["player"] <= 3}, {1, 2, 3})
                    orders = self._command_players(replay)
                    (output / (name + ".orders.json")).write_text(json.dumps(orders), encoding="utf-8")
                    for player in (2, 3):
                        self.assertGreater(orders.get(player, 0), 0, f"AI player {player} issued no orders")

    def test_ffa_gui_stock_map_capacity(self):
        with tempfile.TemporaryDirectory(prefix="skirmish-ffa-gui-") as temporary:
            output = Path(os.environ.get("EAWR_SETUP_EYECHECK", temporary)).resolve() / "ffa-gui"
            output.mkdir(parents=True, exist_ok=True)
            for name in ("coruscant", "themaw", "bothawui", "bespin", "felucia"):
                with self.subTest(map=name):
                    report, replay = output / (name + ".json"), output / (name + ".eawr-replay")
                    result = subprocess.run([
                        os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                        "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-skirmish-setup",
                        "--eawr-setup-test", "--eawr-setup-three-player-test", "--eawr-setup-ffa-map-test",
                        f"data/art/maps/_mp_space_{name}.ted",
                        "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                        "--eawr-report", str(report), "--eawr-live-replay-out", str(replay),
                        "--eawr-capture", str(output / (name + ".png")),
                        "--eawr-font-cache", os.environ.get("EAWR_FONT_CACHE", str(ROOT / "out/fonts")),
                        "--eawr-live-ticks", "900", "--eawr-live-ai", "on", "--eawr-audio", "off",
                        "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on"],
                        cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=600)
                    (output / (name + ".log")).write_text(result.stdout, encoding="utf-8")
                    self.assertEqual(result.returncode, 0, result.stdout[-6000:])
                    self.assertIn(f"EAWR FFA setup map: data/art/maps/_mp_space_{name}.ted", result.stdout)
                    self.assertIn("EAWR FFA setup third row: " + ("false" if name == "bespin" else "true"), result.stdout)
                    if name != "felucia":
                        self.assertIn("EAWR FFA setup teams: 2", result.stdout)
                        self.assertNotIn("EAWR live session started", result.stdout)
                    else:
                        self.assertIn("EAWR FFA setup teams: 3", result.stdout)
                        live = json.loads(report.read_text(encoding="utf-8"))["live_session"]
                        self.assertEqual(live["start_map"], f"data/art/maps/_mp_space_{name}.ted")
                        self.assertEqual([row["team"] for row in live["start_slots"]], [0, 1, 2])
                        self.assertEqual(live["ai"]["players"], [2, 3])
                        self.assertTrue(live["headless_hashes_equal"])
                        orders = self._command_players(replay)
                        for player in (2, 3):
                            self.assertGreater(orders.get(player, 0), 0)

    def test_ffa_third_team_fog_and_contact(self):
        with tempfile.TemporaryDirectory(prefix="skirmish-ffa-contact-") as temporary:
            output = Path(os.environ.get("EAWR_SETUP_EYECHECK", temporary)).resolve() / "ffa-contact"
            output.mkdir(parents=True, exist_ok=True)
            common = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                      "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-live-session", "skirmish",
                      "--eawr-populate", "--eawr-map", "data/art/maps/_mp_space_ryloth.ted",
                      "--eawr-skirmish-players", "1,2,3", "--eawr-skirmish-slot", "1:Rebel:0:human",
                      "--eawr-skirmish-slot", "2:Empire:1:ai", "--eawr-skirmish-slot", "3:Empire:2:ai",
                      "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"], "--eawr-live-ai", "off",
                      "--eawr-audio", "off", "--eawr-lighting", "sh", "--eawr-environment", "map",
                      "--eawr-shadows", "on"]

            def run(name, ticks, extra=()):
                report = output / (name + ".json")
                result = subprocess.run([*common, "--eawr-live-ticks", str(ticks), "--eawr-report", str(report),
                                         "--eawr-capture", str(output / (name + ".png")), *extra],
                                        cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        text=True, timeout=600)
                (output / (name + ".log")).write_text(result.stdout, encoding="utf-8")
                self.assertEqual(result.returncode, 0, result.stdout[-6000:])
                live = json.loads(report.read_text(encoding="utf-8"))["live_session"]
                self.assertTrue(live["headless_hashes_equal"])
                return live

            start = run("start", 1)
            marker = next(row for row in start["start_markers"] if row["player"] == 3 and row["use"] == "station")
            x, y, _ = marker["position"]
            camera = ET.parse(ROOT / "apps/viewer/project/config/coruscant-live-session-camera.xml")
            camera.getroot().set("map_path", start["start_map"])
            camera.getroot().set("map_sha256", start["start_map_sha256"])
            camera.find("initial").set("target_x", str(x))
            camera.find("initial").set("target_y", str(y))
            for key, value in {"min_x": "-15000", "max_x": "15000", "min_y": "-15000", "max_y": "15000"}.items():
                camera.find("bounds").set(key, value)
            shutil.copyfile(ROOT / "apps/viewer/project/config/space-live-camera-bindings.json",
                            output / "space-live-camera-bindings.json")
            camera_path = output / "third-team-camera.xml"
            camera.write(camera_path, encoding="utf-8", xml_declaration=True)
            framing = ("--eawr-map-camera-config", str(camera_path))
            hidden = run("third-team-fog", 1, framing)
            revealed = run("third-team-revealed", 1, (*framing, "--eawr-live-reveal", "on"))
            third = {row["entity"] for row in start["rendered_colours"] if row["player"] == 3}
            self.assertTrue(third)
            self.assertFalse(third.intersection(row["entity"] for row in hidden["hostile_units"]))
            self.assertTrue(third.issubset(row["entity"] for row in revealed["hostile_units"]))
            # Move a human scout to the third station with display reveal disabled.
            scout = next(row["entity"] for row in start["start_fleet"]
                         if row["player"] == 1 and row["type"] == "Corellian_Corvette")
            contact = run("third-team-contact", 900, (*framing, "--eawr-live-step", "3",
                          "--eawr-live-order", f"1:move:{scout}@{x + 500},{y},0"))
            self.assertTrue(third.intersection(row["entity"] for row in contact["hostile_units"]),
                            "third-team units remain hidden after the human scout arrives")
            self.assertFalse(contact["reveal"])

    def test_ffa_underworld_third_team_issues_orders(self):
        with tempfile.TemporaryDirectory(prefix="skirmish-ffa-underworld-") as temporary:
            output = Path(os.environ.get("EAWR_SETUP_EYECHECK", temporary)).resolve() / "ffa-underworld"
            output.mkdir(parents=True, exist_ok=True)
            report, replay = output / "battle.json", output / "battle.eawr-replay"
            result = subprocess.run([
                os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-live-session", "skirmish",
                "--eawr-populate", "--eawr-map", "data/art/maps/_mp_space_ryloth.ted",
                "--eawr-skirmish-players", "1,2,3", "--eawr-skirmish-slot", "1:Rebel:0:human",
                "--eawr-skirmish-slot", "2:Empire:1:ai", "--eawr-skirmish-slot", "3:Underworld:2:ai",
                "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                "--eawr-report", str(report), "--eawr-live-replay-out", str(replay),
                "--eawr-capture", str(output / "battle.png"), "--eawr-live-ticks", "900",
                "--eawr-live-ai", "on", "--eawr-audio", "off", "--eawr-lighting", "sh",
                "--eawr-environment", "map", "--eawr-shadows", "on"],
                cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=600)
            (output / "battle.log").write_text(result.stdout, encoding="utf-8")
            self.assertEqual(result.returncode, 0, result.stdout[-6000:])
            live = json.loads(report.read_text(encoding="utf-8"))["live_session"]
            self.assertEqual(live["ai"]["players"], [2, 3])
            self.assertTrue(live["headless_hashes_equal"])
            self.assertTrue(any(row["player"] == 3 for row in live["start_fleet"]))
            self.assertTrue(any(row["player"] == 3 for row in live["rendered_colours"]))
            orders = self._command_players(replay)
            (output / "orders.json").write_text(json.dumps(orders), encoding="utf-8")
            for player in (2, 3):
                self.assertGreater(orders.get(player, 0), 0, f"AI player {player} issued no orders")

    def test_two_per_team_gui_reaches_live_battle(self):
        with tempfile.TemporaryDirectory(prefix="skirmish-2v2-") as temporary:
            output = Path(os.environ.get("EAWR_SETUP_EYECHECK", temporary)).resolve() / "2v2"
            output.mkdir(parents=True, exist_ok=True)
            report = output / "battle.json"
            result = subprocess.run([
                os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-skirmish-setup",
                "--eawr-setup-test", "--eawr-setup-2v2-test",
                "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                "--eawr-report", str(report), "--eawr-capture", str(output / "setup.png"),
                "--eawr-font-cache", os.environ.get("EAWR_FONT_CACHE", str(ROOT / "out/fonts")),
                "--eawr-live-ticks", "90", "--eawr-audio", "off",
                "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on"],
                cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=300)
            (output / "setup.log").write_text(result.stdout, encoding="utf-8")
            self.assertEqual(result.returncode, 0, result.stdout[-6000:])
            self.assertIn("EAWR live session started", result.stdout)
            self.assertIn("EAWR skirmish setup returned", result.stdout)
            live = json.loads(report.read_text(encoding="utf-8"))["live_session"]
            self.assertEqual(live["start_map"], "data/art/maps/_mp_space_coruscant.ted")
            self.assertEqual(live["start_slots"], [
                {"slot": 1, "faction": "Rebel", "team": 0, "human": True},
                {"slot": 2, "faction": "Empire", "team": 1, "human": False},
                {"slot": 3, "faction": "Rebel", "team": 0, "human": False},
                {"slot": 4, "faction": "Empire", "team": 1, "human": False}])
            self.assertTrue(live["headless_hashes_equal"])
            self.assertEqual(live["ai"]["players"], [2, 3, 4])
            self.assertGreater(live["ai"]["ticks"], 0)
            self.assertEqual([row["player"] for row in live["start_colours"]], [1, 2, 3, 4])
            for name in ("setup.png", "setup.battle.png", "setup.returned.png"):
                self.assertGreater((output / name).stat().st_size, 10000, name)

    def test_three_authored_teams_arrive_in_battle(self):
        with tempfile.TemporaryDirectory(prefix="skirmish-three-teams-") as temporary:
            output = Path(os.environ.get("EAWR_SETUP_EYECHECK", temporary)).resolve() / "three-teams"
            output.mkdir(parents=True, exist_ok=True)
            report = output / "battle.json"
            result = subprocess.run([
                os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-skirmish-setup",
                "--eawr-setup-test", "--eawr-setup-three-player-test",
                "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                "--eawr-report", str(report), "--eawr-capture", str(output / "setup.png"),
                "--eawr-live-replay-out", str(output / "battle.eawr-replay"),
                "--eawr-font-cache", os.environ.get("EAWR_FONT_CACHE", str(ROOT / "out/fonts")),
                "--eawr-live-ticks", "900", "--eawr-live-ai", "on", "--eawr-audio", "off",
                "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on"],
                cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=600)
            (output / "setup.log").write_text(result.stdout, encoding="utf-8")
            self.assertEqual(result.returncode, 0, result.stdout[-6000:])
            self.assertIn("EAWR skirmish setup returned", result.stdout)
            live = json.loads(report.read_text(encoding="utf-8"))["live_session"]
            self.assertEqual(live["start_map"], "data/art/maps/_mp_space_ryloth.ted")
            self.assertEqual(live["start_slots"], [
                {"slot": 1, "faction": "Empire", "team": 0, "human": True},
                {"slot": 2, "faction": "Empire", "team": 1, "human": False},
                {"slot": 3, "faction": "Empire", "team": 2, "human": False}])
            self.assertTrue(live["headless_hashes_equal"])
            self.assertEqual([row["rgb"] for row in live["start_colours"]],
                             [[111, 217, 224], [237, 149, 78], [119, 237, 78]])
            self.assertEqual(live["ai"]["players"], [2, 3])
            self.assertEqual({row["player"] for row in live["start_fleet"]}, {1, 2, 3})
            orders = self._command_players(output / "battle.eawr-replay")
            for player in (2, 3):
                self.assertGreater(orders.get(player, 0), 0, f"GUI AI player {player} issued no orders")

    @unittest.skipUnless(os.environ.get("EAWR_RELEASE_VIEWER_ZIP"), "requires a built release viewer zip")
    def test_relocated_launcher_records_trace_and_replay_on_close(self):
        with tempfile.TemporaryDirectory(prefix="relocated release with spaces ") as temporary:
            directory = Path(temporary)
            with zipfile.ZipFile(os.environ["EAWR_RELEASE_VIEWER_ZIP"]) as archive:
                archive.extractall(directory)
            package = next(directory.glob("*-viewer"))
            output = directory / "lag report"
            output.mkdir()
            trace, replay = output / "trace.csv", output / "battle.eawr-replay"
            launcher = ([str(package / "play-demo.cmd")] if os.name == "nt"
                        else ["sh", str(package / "play-demo.sh")])
            result = subprocess.run([
                *launcher, "--game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                "--godot", os.environ["EAWR_GODOT_EXECUTABLE"], "--godot-quit-after", "100",
                "--eawr-perf-trace", str(trace), "--eawr-live-replay-out", str(replay),
                "--eawr-setup-test", "--eawr-live-ticks", "100000",
                "--eawr-live-ai", "off", "--eawr-audio", "off"],
                cwd=directory, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, timeout=240, shell=os.name == "nt")
            evidence = Path(os.environ.get("EAWR_SETUP_EYECHECK", output))
            evidence.mkdir(parents=True, exist_ok=True)
            (evidence / "package-recording.log").write_text(result.stdout, encoding="utf-8")
            self.assertEqual(result.returncode, 0, result.stdout[-6000:])
            self.assertNotIn("ERROR:", result.stdout)
            self.assertNotIn("ObjectDB instances leaked", result.stdout)
            self.assertIn("EAWR skirmish setup ready", result.stdout)
            # Close before the driven battle's end, so teardown must flush both outputs.
            self.assertNotIn("EAWR skirmish setup returned", result.stdout)
            with trace.open(encoding="utf-8", newline="") as stream:
                rows = list(csv.DictReader(stream))
            self.assertTrue(rows)
            self.assertTrue(any(int(row["tick"]) > 0 and int(row["units"]) > 0 for row in rows))
            self.assertGreater(replay.stat().st_size, 104)
            self.assertEqual(replay.read_bytes()[:8], b"EAWRPLY\x00")
            for path in (trace, replay, Path(str(replay) + ".time.csv")):
                (evidence / ("package-" + path.name)).write_bytes(path.read_bytes())

    def test_copied_policy_cancel_defaults_accept_reaches_battle(self):
        with tempfile.TemporaryDirectory(prefix="setup-policy-") as temporary:
            output = Path(os.environ.get("EAWR_SETUP_EYECHECK", temporary)).resolve()
            output.mkdir(parents=True, exist_ok=True)
            report = output / "policy.json"
            replay = output / "policy.eawr-replay"
            hashes = output / "policy.hashes.csv"
            result = subprocess.run([
                os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-skirmish-setup",
                "--eawr-setup-test", "--eawr-setup-policy-test",
                "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"], "--eawr-report", str(report),
                "--eawr-live-replay-out", str(replay),
                "--eawr-live-hashes", str(hashes),
                "--eawr-capture", str(output / "policy.png"), "--eawr-font-cache",
                os.environ.get("EAWR_FONT_CACHE", str(ROOT / "out/fonts")),
                "--eawr-live-ticks", "90", "--eawr-live-ai", "off", "--eawr-audio", "off",
                "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on"],
                cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=240)
            (output / "policy.log").write_text(result.stdout, encoding="utf-8")
            self.assertEqual(result.returncode, 0, result.stdout[-6000:])
            self.assertIn("EAWR setup policy frame 90: true,true,true; custom: false", result.stdout)
            self.assertIn("EAWR setup policy frame 102: true,true,true; custom: false", result.stdout)
            self.assertIn("EAWR setup policy frame 112: false,false,false; custom: true", result.stdout)
            live = json.loads(report.read_text(encoding="utf-8"))["live_session"]
            self.assertEqual(live["match_policy"], {"heroes": False, "superweapons": False, "free_starting_units": False})
            self.assertEqual(live["free_starting_forces"], [])
            self.assertTrue(live["headless_hashes_equal"])
            self.assertIn("EAWR skirmish setup returned", result.stdout)
            for name in ("policy.png", "policy.options.png", "policy.battle.png"):
                self.assertGreater((output / name).stat().st_size, 10000, name)
            self.assertIn(int.from_bytes(replay.read_bytes()[8:10], "little"), (4, 5))
            replay_report = output / "policy.playback.json"
            replay_hashes = output / "policy.playback.hashes.csv"
            reproduced = output / "policy.reproduced.eawr-replay"
            replay_ticks = int.from_bytes(replay.read_bytes()[40:48], "little")
            # Driven frames simulate floor(presented_tick) + 1 for interpolation.
            # End on the last recorded tick's preceding display tick so playback
            # records exactly the source duration, including its lookahead.
            self.assertGreater(replay_ticks, 1)
            result = subprocess.run([
                os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-live-session", "replay",
                "--eawr-populate",
                "--eawr-live-replay", str(replay), "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                "--eawr-map", live["start_map"], "--eawr-live-ticks", str(replay_ticks - 1),
                "--eawr-report", str(replay_report), "--eawr-live-hashes", str(replay_hashes),
                "--eawr-live-replay-out", str(reproduced),
                "--eawr-audio", "off"], cwd=ROOT, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, timeout=240)
            (output / "policy.playback.log").write_text(result.stdout, encoding="utf-8")
            self.assertEqual(result.returncode, 0, result.stdout[-6000:])
            playback = json.loads(replay_report.read_text(encoding="utf-8"))["live_session"]
            self.assertEqual(playback["match_policy"], live["match_policy"])
            self.assertTrue(playback["headless_hashes_equal"])
            self.assertEqual(replay_hashes.read_bytes(), hashes.read_bytes())
            self.assertEqual(reproduced.read_bytes(), replay.read_bytes())

    def test_staging_columns_keep_authored_rectangles_and_advanced_dialog(self):
        """#3167: the player columns keep the resource's rectangles, the row list and the advanced
        dialog (credits slider, win condition) draw, and each state is captured for the eye check."""
        with tempfile.TemporaryDirectory(prefix="setup-layout-") as temporary:
            output = Path(os.environ.get("EAWR_SETUP_EYECHECK", temporary)).resolve()
            output.mkdir(parents=True, exist_ok=True)
            result = subprocess.run([
                os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-skirmish-setup",
                "--eawr-setup-test", "--eawr-setup-layout-test",
                "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                "--eawr-capture", str(output / "layout.png"), "--eawr-font-cache",
                os.environ.get("EAWR_FONT_CACHE", str(ROOT / "out/fonts")), "--eawr-audio", "off"],
                cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=240)
            (output / "layout.log").write_text(result.stdout, encoding="utf-8")
            self.assertEqual(result.returncode, 0, result.stdout[-6000:])
            for name in ("layout.png", "layout.popup.png", "layout.options.png", "layout.wincondition.png"):
                self.assertGreater((output / name).stat().st_size, 10000, name)
            number = r"(-?\d+(?:\.\d+)?)"
            pattern = re.compile(r"EAWR setup layout (IDC_\w+) authored=" + ",".join([number] * 4)
                                 + r" actual=" + ",".join([number] * 4) + r" visible=(\d)")
            staging = {}
            options = {}
            for line in result.stdout.splitlines():
                found = pattern.search(line)
                if not found:
                    continue
                values = [float(item) for item in found.groups()[1:9]]
                (staging if found.group(1).startswith("IDC_PLAYER_") else options)[found.group(1)] = (
                    values[:4], values[4:], found.group(10) == "1")
            # Every row's four controls sit on the rectangle the resource gives them (no growth to their
            # longest item), and the columns keep the resource's gaps.
            self.assertEqual(len(staging), 36, sorted(staging))
            for name, (authored, actual, visible) in staging.items():
                self.assertEqual(visible, not name.endswith("_8"), name)
                for index, axis in enumerate(("x", "y", "width", "height")):
                    self.assertAlmostEqual(authored[index], actual[index], delta=0.75, msg=name + " " + axis)
            for row in range(9):
                columns = [staging["IDC_PLAYER_%s_%d" % (kind, row)][1]
                           for kind in ("NAME", "FACTION", "COLOR", "TEAM")]
                for left, right in zip(columns, columns[1:]):
                    self.assertGreaterEqual(right[0] - (left[0] + left[2]), 2.0, "row %d columns touch" % row)
            # The advanced dialog: the disabled credits slider displays the authored value, is
            # disabled with its reason, and the win condition offers the two space choices.
            for name in ("IDC_SKIRMISH_CREDITS_SLIDER", "IDC_SKIRMISH_CREDITS_VALUE", "IDC_WIN_CONDITION_COMBO"):
                self.assertIn(name, options)
                self.assertTrue(options[name][2], name)
                authored, actual, _ = options[name]
                self.assertTrue(all(abs(a - b) <= 0.75 for a, b in zip(authored, actual)), (name, authored, actual))
            credits = re.search(r"EAWR setup credits: min=" + number + " max=" + number + " step=" + number
                                + " value=" + number + " caption=" + number + " enabled=(\d)", result.stdout)
            self.assertIsNotNone(credits, result.stdout[-3000:])
            low, high, step, value = (float(credits.group(i)) for i in range(1, 5))
            self.assertLess(low, high)
            self.assertEqual(step, 0.0, "displaying data must not round it to a slider-edit bucket")
            self.assertTrue(low <= value <= high, (low, value, high))
            self.assertEqual(float(credits.group(5)), value)
            self.assertEqual(credits.group(6), "0", "the credits slider is disabled until a battle applies it")
            slots = re.findall(r"EAWR setup slot (\d+): active=(\d) faction=(.*?) team=(.*)", result.stdout)
            open_rows = [row for row in slots if row[1] == "0"]
            self.assertTrue(open_rows, result.stdout[-6000:])
            self.assertTrue(all(not faction and not team for _, _, faction, team in open_rows), open_rows)
            reopened = [row for row in slots if row[0] == "1"]
            self.assertEqual([row[1] for row in reopened], ["1", "0", "0", "1"], reopened)
            self.assertTrue(reopened[-1][2] and reopened[-1][3], reopened)
            alpha = re.search(r"EAWR setup popup alpha: " + number, result.stdout)
            self.assertIsNotNone(alpha)
            self.assertTrue(0 < float(alpha.group(1)) < 1, alpha.group(1))
            self.assertIn("EAWR setup win condition: items=2 selected=1", result.stdout)
            popup = re.search(r"EAWR setup popup: visible=1 rect=" + ",".join([number] * 4) + " combo="
                              + ",".join([number] * 4), result.stdout)
            self.assertIsNotNone(popup, result.stdout[-3000:])
            list_width, combo_width = float(popup.group(3)), float(popup.group(7))
            # A window's width is whole pixels, so the list may be a pixel narrower than its box.
            self.assertGreaterEqual(list_width, combo_width - 1.0, "the row list is as wide as its box")
            self.assertRegex(result.stdout, r"EAWR setup win list: visible=1 width=" + number + " combo=" + number)

            # Derive the fixture from the loaded constants, with a value halfway between edit buckets.
            from tools.inventory.corpus import Corpus
            constants = ET.fromstring(Corpus(os.environ["EAWR_EAW_GAME_ROOT"]).read_effective(
                "foc", "data/xml/gameconstants.xml").data)
            authored = int(low) + 501
            self.assertLess(authored, high)
            node = constants.find(".//MP_Default_Credits")
            self.assertIsNotNone(node)
            node.text = str(authored)
            xml = output / "mod/Data/XML"
            xml.mkdir(parents=True)
            (xml.parent / "MegaFiles.xml").write_text(
                "<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
            ET.ElementTree(constants).write(xml / "GameConstants.xml", encoding="utf-8")
            command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1600x900",
                       "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-skirmish-setup",
                       "--eawr-setup-test", "--eawr-setup-layout-test", "--eawr-game-root",
                       os.environ["EAWR_EAW_GAME_ROOT"], "--eawr-mod-root", str(xml.parents[1]),
                       "--eawr-capture", str(output / "modded.png"), "--eawr-font-cache",
                       os.environ.get("EAWR_FONT_CACHE", str(ROOT / "out/fonts")), "--eawr-audio", "off"]
            modded = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, timeout=240)
            (output / "modded.log").write_text(modded.stdout, encoding="utf-8")
            self.assertEqual(modded.returncode, 0, modded.stdout[-6000:])
            mod_credits = re.search(r"EAWR setup credits: .* value=" + number + " caption=" + number
                                    + r" enabled=0", modded.stdout)
            self.assertIsNotNone(mod_credits, modded.stdout[-6000:])
            self.assertEqual(float(mod_credits.group(1)), authored)
            self.assertEqual(float(mod_credits.group(2)), authored)
            scale = re.search(r"EAWR setup popup scale: " + number + " combo=" + number, modded.stdout)
            self.assertIsNotNone(scale, modded.stdout[-6000:])
            self.assertGreater(float(scale.group(2)), 1.0)
            self.assertAlmostEqual(float(scale.group(1)), float(scale.group(2)), places=3)

            node.text = str(int(high) + 500)
            ET.ElementTree(constants).write(xml / "GameConstants.xml", encoding="utf-8")
            bounded_capture = output / "bounded.png"
            command[command.index("--eawr-capture") + 1] = str(bounded_capture)
            bounded = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, timeout=240)
            (output / "bounded.log").write_text(bounded.stdout, encoding="utf-8")
            self.assertEqual(bounded.returncode, 0, bounded.stdout[-6000:])
            bound_credits = re.search(r"EAWR setup credits: .* value=" + number + " caption=" + number
                                      + r" enabled=0", bounded.stdout)
            self.assertIsNotNone(bound_credits, bounded.stdout[-6000:])
            self.assertEqual(float(bound_credits.group(1)), high)
            self.assertEqual(float(bound_credits.group(2)), high)
            self.assertGreater((output / "bounded.options.png").stat().st_size, 10000)

    def test_nondefault_policy_replay_binds_recorded_purchase_content(self):
        with tempfile.TemporaryDirectory(prefix="replay-policy-") as temporary:
            output = Path(temporary)
            replay = output / "policy.eawr-replay"
            recorded = (ROOT / "tests/skirmish/fixtures/m2-start.eawr-replay").read_bytes()
            header, setup_start = canonical_replay_header(recorded, 3)
            source = bytearray(header + recorded[setup_start:])
            # Keep recorded setup metadata, with matching flags in both policy records.
            self.assertEqual(int.from_bytes(source[8:10], "little"), 3)
            source[8:10] = (5).to_bytes(2, "little")
            disabled_flags = 3
            extensions = [struct.pack("<HHI", 1, 4, disabled_flags)]
            if setup_start > len(header):
                count = struct.unpack_from("<I", recorded, len(header))[0]
                offset = len(header) + 4
                for _ in range(count):
                    tag, length = struct.unpack_from("<HH", recorded, offset)
                    end = offset + 4 + length
                    if tag != 1:
                        extension = bytearray(recorded[offset:end])
                        if tag == 2:  # SKSU: map and map digest precede the match flags.
                            flags_offset = 4
                            for _ in range(2):
                                text_length = struct.unpack_from("<H", extension, flags_offset)[0]
                                flags_offset += 2 + text_length
                            flags = struct.unpack_from("<I", extension, flags_offset)[0]
                            struct.pack_into("<I", extension, flags_offset, (flags & ~15) | disabled_flags)
                        extensions.append(extension)
                    offset = end
            metadata = struct.pack("<I", len(extensions)) + b"".join(extensions)
            source[10:12] = (len(header) + len(metadata)).to_bytes(2, "little")
            source[len(header):len(header)] = metadata
            replay.write_bytes(source)
            report = output / "policy-replay.json"
            result = subprocess.run([
                os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-live-session", "replay",
                "--eawr-populate",
                "--eawr-live-replay", str(replay), "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                "--eawr-map", "data/art/maps/_mp_space_coruscant.ted", "--eawr-live-ticks", "30",
                "--eawr-report", str(report), "--eawr-audio", "off"],
                cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=240)
            (output / "policy-replay.log").write_text(result.stdout, encoding="utf-8")
            self.assertEqual(result.returncode, 0, result.stdout[-6000:])
            self.assertTrue(json.loads(report.read_text(encoding="utf-8"))["live_session"]["headless_hashes_equal"])

    def test_campaign_bound_marker_replay_reproduces_every_hash(self):
        # A campaign recording can use the ordinary marker map path without a
        # native-scene selection. Its accounts and rules still come from CAMP.
        recorded = (ROOT / "tests/skirmish/fixtures/m2-start.eawr-replay").read_bytes()
        header, setup_start = canonical_replay_header(recorded, 3)
        header = bytearray(header)
        players, squadrons, units = struct.unpack_from("<IIQ", header, 48)
        accounts = [struct.unpack_from("<I", recorded, setup_start + index * 24)[0]
                    for index in range(players)
                    if struct.unpack_from("<I", recorded, setup_start + index * 24 + 16)[0] & 1]
        unit_start = setup_start + players * 24
        identities = [struct.unpack_from("<Q", recorded, unit_start + index * 80)[0] for index in range(units)]
        registered = [identities[index] for index in range(units)
                      if struct.unpack_from("<I", recorded, unit_start + index * 80 + 16)[0] in accounts]
        commands_start = unit_start + units * 80
        for _ in range(squadrons):
            identities.append(struct.unpack_from("<Q", recorded, commands_start)[0])
            members = struct.unpack_from("<I", recorded, commands_start + 8)[0]
            commands_start += 16 + 8 * members
        one = 1 << 24
        campaign = bytearray(struct.pack("<IqI", 5, one, len(accounts)))
        for index, player in enumerate(accounts):
            campaign += struct.pack("<IqIIqIIqqqq", player, (1234 + index) * one,
                                    1000, int(index != 0), 0, 1, 5, one, one, 1000000 * one, 0)
        campaign += struct.pack("<III", 0, 0, len(registered))  # reserves, upgrades, population
        campaign += b"".join(struct.pack("<Q", entity) for entity in registered)
        campaign += struct.pack("<IQ", 0, 0)  # no victory leaders or station
        extensions = [struct.pack("<HHI", 1, 4, 4),  # campaign has no free starting templates
                      struct.pack("<HHQ", 6, 8, max(identities) + 1),
                      struct.pack("<HH", 10, len(campaign)) + campaign]
        if setup_start > len(header):
            offset = len(header) + 4
            for _ in range(struct.unpack_from("<I", recorded, len(header))[0]):
                tag, length = struct.unpack_from("<HH", recorded, offset)
                end = offset + 4 + length
                if tag not in (1, 5, 6, 10, 15, 16):
                    extension = bytearray(recorded[offset:end])
                    if tag == 2:
                        flags_offset = 4
                        for _ in range(2):
                            size = struct.unpack_from("<H", extension, flags_offset)[0]
                            flags_offset += 2 + size
                        flags = struct.unpack_from("<I", extension, flags_offset)[0]
                        struct.pack_into("<I", extension, flags_offset, (flags & ~15) | 4)
                    extensions.append(extension)
                offset = end
        extensions.sort(key=lambda extension: struct.unpack_from("<H", extension)[0])
        metadata = struct.pack("<I", len(extensions)) + b"".join(extensions)
        struct.pack_into("<HH", header, 8, 5, len(header) + len(metadata))
        struct.pack_into("<Q", header, 40, 60)
        struct.pack_into("<Q", header, 64, 0)  # this focused recording has no player commands
        with tempfile.TemporaryDirectory(prefix="campaign-marker-replay-") as temporary:
            directory = Path(temporary)
            replay = directory / "campaign.eawr-replay"
            replay.write_bytes(header + metadata + recorded[setup_start:commands_start])
            expected = None
            for workers in (1, 2, 4, 8):
                report = directory / f"campaign-{workers}.json"
                hashes = directory / f"campaign-{workers}.csv"
                result = subprocess.run([
                    os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                    "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-live-session", "replay",
                    "--eawr-populate", "--eawr-live-replay", str(replay),
                    "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                    "--eawr-map", "data/art/maps/_mp_space_coruscant.ted", "--eawr-live-ticks", "29",
                    "--eawr-map-timed-frames", "30", "--eawr-map-particle-frames", "1", "--eawr-live-step", "1",
                    "--eawr-live-workers", str(workers), "--eawr-live-hashes", str(hashes),
                    "--eawr-report", str(report), "--eawr-audio", "off"],
                    cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=240)
                (directory / f"campaign-{workers}.log").write_text(result.stdout, encoding="utf-8")
                self.assertEqual(result.returncode, 0, result.stdout[-6000:])
                live = json.loads(report.read_text(encoding="utf-8"))["live_session"]
                self.assertTrue(live["headless_hashes_equal"])
                self.assertEqual(live["completed_ticks"], 30)
                self.assertFalse(live["rejected"])
                actual = hashes.read_bytes()
                if expected is None:
                    expected = actual
                self.assertEqual(actual, expected)

    def test_gui_selects_map_faction_starts_and_returns(self):
        self._starts_and_returns(1)

    def test_second_battle_recreates_session_and_input(self):
        self._starts_and_returns(2)

    def _starts_and_returns(self, starts):
        # An explicit artifact directory lets the same test retain the owner's lit eye-check sheet.
        with tempfile.TemporaryDirectory(prefix="skirmish-setup-") as temporary:
            output = Path(os.environ.get("EAWR_SETUP_EYECHECK", temporary)).resolve()
            output.mkdir(parents=True, exist_ok=True)
            report = output / "setup-battle.json"
            trace, replay = output / "setup-trace.csv", output / "setup.eawr-replay"
            for name in ("setup-battle.json", "setup.png", "setup.battle.png", "setup.returned.png",
                         trace.name, replay.name):
                (output / name).unlink(missing_ok=True)
            result = subprocess.run([
                os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-skirmish-setup",
                "--eawr-setup-test", "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                "--eawr-load-bench-starts", str(starts),
                *(["--eawr-load-bench-map", "data/art/maps/_mp_space_kessel.ted"] if starts > 1 else []),
                "--eawr-report", str(report), "--eawr-capture", str(output / "setup.png"),
                "--eawr-perf-trace", str(trace), "--eawr-live-replay-out", str(replay),
                "--eawr-font-cache", os.environ.get("EAWR_FONT_CACHE", str(ROOT / "out/fonts")),
                "--eawr-live-ticks", "90", "--eawr-live-input", "10:click:unit=1",
                "--eawr-live-ai", "off", "--eawr-audio", "off", "--eawr-lighting", "sh",
                "--eawr-environment", "map", "--eawr-shadows", "on"],
                cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=240)
            (output / "setup.log").write_text(result.stdout, encoding="utf-8")
            self.assertEqual(result.returncode, 0, result.stdout[-6000:])
            self.assertIn("EAWR skirmish setup returned", result.stdout)
            self.assertEqual(result.stdout.count("EAWR skirmish setup returned"), starts)
            # Every caption the screen sets itself resolves through the installed text database.
            self.assertRegex(result.stdout, r"EAWR setup captions: \d+ keys; missing: 0(?!\d)")
            if starts == 1:
                self.assertIn("EAWR setup custom map list: true; empty: true; start disabled: true", result.stdout)
                self.assertIn("EAWR setup official map list: true; maps: 24; preview starts: 2", result.stdout)
            data = json.loads(report.read_text(encoding="utf-8"))
            live = data["live_session"]
            self.assertEqual(live["start_map"], "data/art/maps/_mp_space_kessel.ted")
            self.assertEqual(live["start_slots"], [
                {"slot": 1, "faction": "Rebel" if starts > 1 else "Empire", "team": 0, "human": True},
                {"slot": 2, "faction": "Empire", "team": 1, "human": False}])
            self.assertTrue(live["headless_hashes_equal"])
            self.assertEqual((live["ready_tick"], live["begin_tick"]), (0, 0))
            self.assertIsNone(live["battle_end"])
            expected_colours = ([
                {"player": 1, "constant": "MP_Color_Cyan", "rgb": [111, 217, 224]},
                {"player": 2, "constant": "MP_Color_Orange", "rgb": [237, 149, 78]}] if starts == 1 else [
                {"player": 1, "constant": "MP_Color_Blue", "rgb": [78, 150, 237]},
                {"player": 2, "constant": "MP_Color_Red", "rgb": [237, 78, 78]}])
            self.assertEqual(live["start_colours"], expected_colours)
            for colour in expected_colours:
                player, rgb = colour["player"], colour["rgb"]
                rendered = [row for row in live["rendered_colours"] if row["player"] == player]
                self.assertTrue(rendered, f"player {player} has visible coloured units")
                self.assertTrue(all(row["rgb"] == rgb for row in rendered))
            self.assertEqual(data["battle_input"]["scripted_fired"], 1)
            self.assertIn(1, data["battle_input"]["selected"])
            with trace.open(encoding="utf-8", newline="") as stream:
                self.assertTrue(list(csv.DictReader(stream)))
            self.assertEqual(replay.read_bytes()[:8], b"EAWRPLY\x00")
            for name in ("setup.png", "setup.battle.png", "setup.returned.png"):
                self.assertGreater((output / name).stat().st_size, 10000, name)


if __name__ == "__main__":
    unittest.main()
