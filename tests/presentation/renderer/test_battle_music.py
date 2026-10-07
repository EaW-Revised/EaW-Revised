"""Tactical faction result selection and failed stream replacement (SND-54/72)."""

import os
import pathlib
import sys
import struct
import zlib
import tempfile
import unittest
import xml.etree.ElementTree as ET

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from live_session_test_support import LiveSessionRunner


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST")
                     and os.environ.get("EAWR_EAW_GAME_ROOT"), "requires viewer GPU host and game data")
class BattleMusicGpu(LiveSessionRunner, unittest.TestCase):
    def test_allied_community_combat_triggers_music_independent_of_fog_and_camera(self):
        from space_hazard_cases import place_ship
        from tools.inventory.corpus import Corpus

        for shared, reveal in ((False, "off"), (True, "off"), (True, "on")):
            with self.subTest(shared=shared, reveal=reveal), tempfile.TemporaryDirectory(prefix="battle-music-community-") as temporary:
                directory = pathlib.Path(temporary)
                xml = directory / "mod/Data/XML"
                xml.mkdir(parents=True)
                (xml.parent / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
                tree = ET.fromstring(Corpus(os.environ["EAWR_EAW_GAME_ROOT"]).read_effective("foc", "data/xml/starbases.xml").data)
                station_type = next(node for node in tree if node.get("Name") == "Skirmish_Rebel_Star_Base_1")
                field = station_type.find("Is_Community_Property")
                if field is None: field = ET.SubElement(station_type, "Is_Community_Property")
                field.text = "Yes" if shared else "No"
                (xml / "StarBases.xml").write_bytes(ET.tostring(tree, encoding="utf-8"))
                constants = ET.fromstring(Corpus(os.environ["EAWR_EAW_GAME_ROOT"]).read_effective("foc", "data/xml/gameconstants.xml").data)
                constants.find("MP_Default_Free_Starting_Units").text = "No"
                (xml / "GameConstants.xml").write_bytes(ET.tostring(constants, encoding="utf-8"))
                replay = directory / "seed.eawr-replay"
                setup = ("--eawr-skirmish-players", "1,2,3", "--eawr-skirmish-slot", "1:Rebel:0:ai",
                    "--eawr-skirmish-slot", "2:Empire:1:ai", "--eawr-skirmish-slot", "3:Rebel:0:human",
                    "--eawr-skirmish-fleet", "1:none", "--eawr-skirmish-fleet", "2:Tartan_Patrol_Cruiser",
                    "--eawr-skirmish-fleet", "3:none", "--eawr-live-player", "3", "--eawr-live-ai", "off",
                    "--eawr-mod-root", str(directory / "mod"), "--eawr-audio", "off")
                code, seed = self._run(directory, "seed", (*setup, "--eawr-live-ticks", "1",
                    "--eawr-live-replay-out", str(replay)), session=("--eawr-live-session", "skirmish"))
                self.assertEqual(code, 0, seed.get("failure"))
                _, units = self._setup(replay)
                station = next(entity for entity, kind, owner in units
                    if owner == 1 and kind == zlib.crc32(b"SKIRMISH_REBEL_STAR_BASE_1"))
                enemy = next(row["entity"] for row in seed["live_session"]["start_fleet"]
                    if row["player"] == 2 and row["type"] == "Tartan_Patrol_Cruiser")
                position = next(row["position"] for row in seed["live_session"]["start_markers"]
                    if row["player"] == 1 and row["use"] == "station")
                place_ship(replay, enemy, position[0] + 800, position[1], position[2])
                # Default camera stays on the distant local fleet marker; trigger ignores framing.
                code, report = self._run(directory, "combat", ("--eawr-live-replay", str(replay),
                    "--eawr-live-player", "3", "--eawr-mod-root", str(directory / "mod"),
                    "--eawr-live-order", f"1:attack:{enemy}@{station}", "--eawr-live-ticks", "120",
                    "--eawr-live-step", "1", "--eawr-live-ai", "off", "--eawr-audio", "off",
                    "--eawr-live-reveal", reveal), session=("--eawr-live-session", "replay"))
                self.assertEqual(code, 0, report.get("failure"))
                self.assertTrue(report["live_session"]["headless_hashes_equal"])
                sound = report["battle_audio"]
                self.assertGreater(sum(value for key, value in sound["requested"].items() if key.startswith("fire:")), 0)
                battles = [row for row in sound["music"] if " battle " in row]
                self.assertEqual(len(battles), 1 if shared else 0, sound["music"])

    @staticmethod
    def _setup(replay):
        data = replay.read_bytes()
        header = struct.unpack_from("<H", data, 10)[0]
        players = struct.unpack_from("<I", data, 48)[0]
        count = struct.unpack_from("<Q", data, 56)[0]
        parties = [struct.unpack_from("<IIII", data, header + i * 24) for i in range(players)]
        units = [struct.unpack_from("<QQI", data, header + players * 24 + i * 80) for i in range(count)]
        return parties, units

    def test_allied_winner_uses_lose_music_and_win_hud_sound(self):
        with tempfile.TemporaryDirectory(prefix="battle-music-ally-") as temporary:
            directory = pathlib.Path(temporary)
            replay = directory / "seed.eawr-replay"
            setup = ("--eawr-skirmish-players", "1,2,3", "--eawr-skirmish-slot", "1:Rebel:0:ai",
                "--eawr-skirmish-slot", "2:Empire:1:ai", "--eawr-skirmish-slot", "3:Rebel:0:human",
                "--eawr-skirmish-fleet", "1:none", "--eawr-skirmish-fleet", "2:none",
                "--eawr-skirmish-fleet", "3:none", "--eawr-live-player", "3", "--eawr-live-ai", "off",
                "--eawr-audio", "off")
            code, report = self._run(directory, "seed", (*setup, "--eawr-live-ticks", "1",
                "--eawr-live-replay-out", str(replay)), session=("--eawr-live-session", "skirmish"))
            self.assertEqual(code, 0, report.get("failure"))
            _, units = self._setup(replay)
            station = next(entity for entity, kind, owner in units
                if owner == 2 and kind == zlib.crc32(b"SKIRMISH_EMPIRE_STAR_BASE_1"))
            code, report = self._run(directory, "ally", (*setup, "--eawr-live-ticks", "240",
                "--eawr-live-step", "1", "--eawr-live-order", f"15:damage:{station}@1000000"),
                session=("--eawr-live-session", "skirmish"))
            self.assertEqual(code, 0, report.get("failure"))
            self.assertEqual(report["live_session"]["outcome"]["winner"], 1)
            self.assertTrue(report["live_session"]["headless_hashes_equal"])
            sound = report["battle_audio"]
            self.assertTrue(any(" defeat Lose_To_Rebel_Event " in row for row in sound["music"]), sound["music"])
            self.assertEqual(sound["requested"].get("battle_victory:RHD_Battle_End"), 1)

    def test_neutral_observer_uses_lose_music_without_hud_outcome(self):
        from tools.inventory.corpus import Corpus

        with tempfile.TemporaryDirectory(prefix="battle-music-observer-") as temporary:
            directory = pathlib.Path(temporary)
            replay = directory / "seed.eawr-replay"
            code, report = self._run(directory, "seed", ("--eawr-live-ticks", "1", "--eawr-live-ai", "off",
                "--eawr-audio", "off", "--eawr-live-replay-out", str(replay)))
            self.assertEqual(code, 0, report.get("failure"))
            parties, _ = self._setup(replay)
            observer = next(player for player, _, faction, _ in parties if faction == zlib.crc32(b"NEUTRAL"))
            xml = directory / "mod/Data/XML"
            xml.mkdir(parents=True)
            (xml.parent / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
            tree = ET.fromstring(Corpus(os.environ["EAWR_EAW_GAME_ROOT"]).read_effective("foc", "data/xml/factions.xml").data)
            neutral = next(node for node in tree if node.get("Name") == "Neutral")
            ET.SubElement(neutral, "Music_Event_Tactical_Lose").text = "Lose_To_Rebel_Event"
            ET.SubElement(neutral, "SFXEvent_HUD_Lost_Space_Battle").text = "RHD_Defeated"
            (xml / "Factions.xml").write_bytes(ET.tostring(tree, encoding="utf-8"))
            code, report = self._battle_end(directory, "observer", 8, "defeat", (
                "--eawr-live-player", str(observer), "--eawr-mod-root", str(directory / "mod"),
                "--eawr-live-ai", "off", "--eawr-audio", "off"))
            self.assertEqual(code, 0, report.get("failure"))
            self.assertTrue(report["live_session"]["headless_hashes_equal"])
            sound = report["battle_audio"]
            self.assertTrue(any(" defeat Lose_To_Rebel_Event " in row for row in sound["music"]), sound["music"])
            self.assertFalse(any(key.startswith(("battle_victory:", "battle_defeat:")) for key in sound["requested"]))

    def test_stock_result_music_is_distinct_from_blank_summary(self):
        for station, mode, event in ((8, "victory", "Rebel_Win_Tactical_Event"),
                                      (1, "defeat", "Lose_To_Empire_Event")):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(prefix="battle-music-") as temporary:
                code, report = self._battle_end(pathlib.Path(temporary), mode, station, mode,
                    ("--eawr-live-ai", "off", "--eawr-audio", "off"))
                self.assertEqual(code, 0, report.get("failure"))
                self.assertTrue(report["live_session"]["headless_hashes_equal"])
                sound = report["battle_audio"]
                selected = [row for row in sound["music"] if f" {mode} " in row]
                self.assertEqual(len(selected), 1, sound["music"])
                self.assertIn(event, selected[0])
                self.assertEqual(sound["music_mode"], mode)
                self.assertFalse(any(" summary " in row for row in sound["music"]))
                self.assertTrue(any("retire " in row for row in sound["music_streams"]))

    def test_missing_result_file_retires_previous_before_failed_open(self):
        from tools.inventory.corpus import Corpus

        with tempfile.TemporaryDirectory(prefix="battle-music-missing-") as temporary:
            directory = pathlib.Path(temporary)
            xml = directory / "mod/Data/XML"
            xml.mkdir(parents=True)
            (xml.parent / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
            authored = Corpus(os.environ["EAWR_EAW_GAME_ROOT"]).read_effective("foc", "data/xml/musicevents.xml")
            tree = ET.fromstring(authored.data)
            result = next(node for node in tree if node.get("Name") == "Rebel_Win_Tactical_Event")
            result.find("Files").text = "Missing_Result_Music.MP3"
            (xml / "MusicEvents.xml").write_bytes(ET.tostring(tree, encoding="utf-8"))
            code, report = self._battle_end(directory, "missing", 8, "victory",
                ("--eawr-mod-root", str(directory / "mod"), "--eawr-live-ai", "off", "--eawr-audio", "off",
                 "--eawr-live-audio-pace", "on"))
            self.assertEqual(code, 0, report.get("failure"))
            self.assertTrue(report["live_session"]["headless_hashes_equal"])
            streams = report["battle_audio"]["music_streams"]
            retired = next(i for i, row in enumerate(streams) if "retire " in row)
            failed = next(i for i, row in enumerate(streams) if "open_failed Missing_Result_Music.MP3" in row)
            self.assertLess(retired, failed, streams)
            self.assertFalse(any("start " in row for row in streams[failed + 1:]), streams)
            silent = next(row for row in streams[failed + 1:] if " silent " in row)
            duration = float(silent.split()[0]) - float(streams[retired].split()[0])
            # Retirement is logged at the frame boundary before that frame's fade update.
            # The paced step-three frame can contribute 0.15 s immediately.
            self.assertGreaterEqual(duration, 1.75, streams)
            self.assertLess(duration, 2.2, streams)

    def test_third_event_closes_older_ending_stream_immediately(self):
        from tools.inventory.corpus import Corpus

        with tempfile.TemporaryDirectory(prefix="battle-music-third-") as temporary:
            directory = pathlib.Path(temporary)
            xml = directory / "mod/Data/XML"
            xml.mkdir(parents=True)
            (xml.parent / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
            corpus = Corpus(os.environ["EAWR_EAW_GAME_ROOT"])
            tree = ET.fromstring(corpus.read_effective("foc", "data/xml/musicevents.xml").data)
            result = next(node for node in tree if node.get("Name") == "Rebel_Win_Tactical_Event")
            # Keep the first ending stream audible when the summary becomes the third event.
            result.find("Fade_Out_Previous_Seconds").text = "100"
            third = ET.SubElement(tree, "MusicEvent", Name="Third_Music_Test")
            for key, value in (("Files", "Rebel_Victory.MP3"), ("Volume_Percent", "75"),
                               ("Fade_In_Seconds", "0"), ("Fade_Out_Previous_Seconds", "2"), ("Loop", "Yes")):
                ET.SubElement(third, key).text = value
            (xml / "MusicEvents.xml").write_bytes(ET.tostring(tree, encoding="utf-8"))
            audio = ET.fromstring(corpus.read_effective("foc", "data/xml/audio.xml").data)
            audio.find("Music_Event_Battle_End_Summary_Screen_Win").text = "Third_Music_Test"
            (xml / "Audio.xml").write_bytes(ET.tostring(audio, encoding="utf-8"))
            code, report = self._battle_end(directory, "third", 8, "victory", (
                "--eawr-mod-root", str(directory / "mod"), "--eawr-live-ai", "off", "--eawr-audio", "off"))
            self.assertEqual(code, 0, report.get("failure"))
            self.assertTrue(report["live_session"]["headless_hashes_equal"])
            streams = report["battle_audio"]["music_streams"]
            closes = [i for i, row in enumerate(streams) if " close " in row]
            self.assertEqual(len(closes), 1, streams)
            self.assertEqual(sum(" start " in row for row in streams), 3, streams)
            self.assertIn("retire Rebel_Victory.MP3", streams[closes[0] + 1])
            self.assertIn("start Rebel_Victory.MP3", streams[closes[0] + 2])

    def test_matching_blank_override_preserves_ambient(self):
        from tools.inventory.corpus import Corpus

        with tempfile.TemporaryDirectory(prefix="battle-music-blank-") as temporary:
            directory = pathlib.Path(temporary)
            xml = directory / "mod/Data/XML"
            xml.mkdir(parents=True)
            (xml.parent / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
            authored = Corpus(os.environ["EAWR_EAW_GAME_ROOT"]).read_effective("foc", "data/xml/factions.xml")
            tree = ET.fromstring(authored.data)
            rebel = next(node for node in tree if node.get("Name") == "Rebel")
            for override in rebel.findall("Music_Event_Tactical_Win_Vs_Faction"):
                if override.text.split(",")[0].strip() == "Empire":
                    override.text = "Empire, "
            (xml / "Factions.xml").write_bytes(ET.tostring(tree, encoding="utf-8"))
            code, report = self._battle_end(directory, "blank", 8, "victory",
                ("--eawr-mod-root", str(directory / "mod"), "--eawr-live-ai", "off", "--eawr-audio", "off"))
            self.assertEqual(code, 0, report.get("failure"))
            self.assertTrue(report["live_session"]["headless_hashes_equal"])
            self.assertFalse(any(" victory " in row for row in report["battle_audio"]["music"]))
            self.assertEqual(report["battle_audio"]["requested"].get("battle_victory:RHD_Battle_End"), 1)


if __name__ == "__main__":
    unittest.main()
