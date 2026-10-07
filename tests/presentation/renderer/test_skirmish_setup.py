"""Opt-in GUI-to-battle contract for #948; run through a private GPU desktop."""

import csv
import json
import os
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


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "requires the viewer GPU lane and installed game data")
class SkirmishSetupGpu(unittest.TestCase):
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

    def test_nondefault_policy_replay_binds_recorded_purchase_content(self):
        with tempfile.TemporaryDirectory(prefix="replay-policy-") as temporary:
            output = Path(temporary)
            replay = output / "policy.eawr-replay"
            recorded = (ROOT / "tests/skirmish/fixtures/m2-start.eawr-replay").read_bytes()
            header, setup_start = canonical_replay_header(recorded, 3)
            source = bytearray(header + recorded[setup_start:])
            # Keep recorded setup metadata while replacing the four-flag policy record.
            self.assertEqual(int.from_bytes(source[8:10], "little"), 3)
            source[8:10] = (5).to_bytes(2, "little")
            extensions = [struct.pack("<HHI", 1, 4, 3)]
            if setup_start > len(header):
                count = struct.unpack_from("<I", recorded, len(header))[0]
                offset = len(header) + 4
                for _ in range(count):
                    tag, length = struct.unpack_from("<HH", recorded, offset)
                    end = offset + 4 + length
                    if tag != 1:
                        extensions.append(recorded[offset:end])
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
