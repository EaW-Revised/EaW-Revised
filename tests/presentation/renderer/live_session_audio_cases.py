"""Cases for test_live_session; collected by its legacy facade."""

from live_session_test_support import (
    ACCLAMATOR, CAMERA, CAPTURE_TICKS, CORUSCANT,
    CORVETTE, DUEL, DUEL_ARGS, DUEL_CAMERA,
    DUEL_CAPTURE_TICKS, DUEL_DEATH_TICK, EMPIRE_STATION, HIT_REASONS,
    LIVE_QUIT_BATTLE_SECONDS, LIVE_QUIT_EXIT_BOUND_SECONDS, LiveSessionRunner, MC80,
    NEBULON, NEBULON_ENGINES, NEBULON_FL, ORDERS,
    ROOT, S28, S28_LIVE_TICKS, STATION,
    decode_png, hit_rows_per_tick, json, os,
    pathlib, re, read, shutil,
    source_text, strict_json, subprocess, sys,
    tempfile, time, unittest,
)


class LiveSessionAudioCases:
    def test_fighter_flybys_require_a_path_and_use_stock_cadence_and_pitch(self):
        with tempfile.TemporaryDirectory(prefix="eawr-fighter-flyby-") as temporary:
            directory = pathlib.Path(temporary)
            common = ("--eawr-live-ai", "off", "--eawr-live-ticks", "600", "--eawr-hud", "off",
                      "--eawr-map-timed-frames", "1")
            code, idle = self._run(directory, "idle", (*common, "--eawr-live-step", "30"))
            self.assertEqual(code, 0, idle.get("failure"))
            self.assertTrue(idle["live_session"]["headless_hashes_equal"])
            self.assertGreater(idle["battle_audio"]["ambient"]["due"], 0)
            self.assertFalse(any(row["moving"] for row in idle["battle_audio"]["ambient"]["rows"]))
            self.assertFalse(any(key.startswith("ambient_moving:") for key in idle["battle_audio"]["requested"]))
            for name, player, unit, destination, event, pitch in (
                    ("x-wing", 1, 2, "3000,4700,0", "Unit_X_Wing_Fly_By", (0.95, 1.05)),
                    ("tie", 2, 9, "-3000,-4393,0", "Unit_TIE_Fighter_Fly_By", (1.0, 1.1))):
                with self.subTest(fighter=name):
                    code, report = self._run(directory, name, (*common,
                        "--eawr-live-player", str(player), "--eawr-live-step", "1",
                        "--eawr-live-audio-pace", "on", "--eawr-live-follow-group", f"0:{unit}",
                        "--eawr-live-order", f"1:move:{unit}@{destination}"))
                    self.assertEqual(code, 0, report.get("failure"))
                    self.assertTrue(report["live_session"]["headless_hashes_equal"])
                    self.assertEqual(report["live_session"]["rejected"], [])
                    audio = report["battle_audio"]
                    self.assertGreater(audio["requested"].get("ambient_moving:" + event, 0), 0, audio)
                    starts = [row for row in audio["ability_starts"] if row["reason"] == "ambient_moving"
                              and row["event"] == event]
                    self.assertTrue(starts, audio)
                    for row in starts:
                        self.assertLessEqual(pitch[0], row["pitch"])
                        self.assertLessEqual(row["pitch"], pitch[1])
                        self.assertAlmostEqual(row["volume"], 0.7)
                    by_unit = {}
                    births = {member: squadron["seen_tick"] for squadron in report["live_session"]["squadrons"]
                              for member in squadron["members"]}
                    for row in audio["ambient"]["rows"]:
                        previous = by_unit.get(row["unit"], births.get(row["unit"], 0))
                        self.assertGreaterEqual(row["tick"] - previous, 150)
                        self.assertLessEqual(row["tick"] - previous, 300)
                        by_unit[row["unit"]] = row["tick"]
                    self.assertEqual(audio["voices_3d"], 32)
                    self.assertLessEqual(audio["max_voices"], 48)
                    self.assertGreater(audio["ambient"]["attached_updates"], 0, audio)
                    self.assertGreater(audio["attached_moves"], 0, audio)
                    self.assertGreater(audio["ambient"]["follower_skips"], 0, audio)
                    self.assertEqual(audio["ambient"]["initialized"], audio["ambient"]["timers"])

    def test_fighter_flyby_timers_match_across_workers_and_presentation_steps(self):
        with tempfile.TemporaryDirectory(prefix="eawr-flyby-determinism-") as temporary:
            directory = pathlib.Path(temporary)
            baseline = None
            for workers, step in ((1, 1), (4, 30), (8, 30)):
                code, report = self._run(directory, f"workers-{workers}", (
                    "--eawr-live-ai", "off", "--eawr-live-ticks", "600", "--eawr-hud", "off",
                    "--eawr-map-timed-frames", "1",
                    "--eawr-live-workers", str(workers), "--eawr-live-step", str(step),
                    "--eawr-live-order", "1:move:2@3000,4700,0"))
                self.assertEqual(code, 0, report.get("failure"))
                self.assertTrue(report["live_session"]["headless_hashes_equal"])
                audio = report["battle_audio"]["ambient"]
                deterministic = {key: audio[key] for key in (
                    "visits", "initialized", "retired", "timers", "peak_timers", "due", "stationary",
                    "follower_skips", "rows")}
                receipt = (report["live_session"]["final_state_sha256"], deterministic)
                if baseline is None: baseline = receipt
                else: self.assertEqual(receipt, baseline)

    def test_fighter_flyby_promotes_live_leader_and_detaches_removed_craft(self):
        import xml.etree.ElementTree as ET
        from tools.inventory.corpus import Corpus

        with tempfile.TemporaryDirectory(prefix="eawr-flyby-leader-") as temporary:
            directory = pathlib.Path(temporary)
            xml = directory / "mod/Data/XML"
            xml.mkdir(parents=True)
            (xml.parent / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
            corpus = Corpus(os.environ["EAWR_EAW_GAME_ROOT"])
            source = corpus.read_effective("foc", "data/xml/spaceunitsfighters.xml")
            self.assertIsNotNone(source)
            tree = ET.fromstring(source.data)
            craft = next(node for node in tree if node.get("Name") == "Y-Wing")
            for key in ("SFXEvent_Ambient_Moving_Min_Delay_Seconds", "SFXEvent_Ambient_Moving_Max_Delay_Seconds"):
                for item in list(craft):
                    if item.tag == key: craft.remove(item)
                ET.SubElement(craft, key).text = "1"
            ET.ElementTree(tree).write(xml / "spaceunitsfighters.xml", encoding="utf-8")
            code, roster = self._run(directory, "roster", (
                "--eawr-mod-root", str(xml.parents[1]), "--eawr-live-ai", "off",
                "--eawr-map-timed-frames", "1", "--eawr-live-ticks", "8"))
            self.assertEqual(code, 0, roster.get("failure"))
            members = next(squadron["members"] for squadron in roster["live_session"]["squadrons"]
                           if squadron["container"] == 4)
            self.assertEqual(len(members), 3)
            code, report = self._run(directory, "leader", (
                "--eawr-mod-root", str(xml.parents[1]), "--eawr-live-ai", "off", "--eawr-live-player", "1",
                "--eawr-map-timed-frames", "1",
                "--eawr-live-step", "1", "--eawr-live-ticks", "100", "--eawr-live-audio-pace", "on",
                "--eawr-live-order", "1:move:4@3000,4700,0", "--eawr-live-follow-group", "0:4",
                "--eawr-live-order", f"35:damage:{members[0]}@100000",
                "--eawr-live-order", f"36:damage:{members[1]}@100000",
                "--eawr-live-order", f"75:damage:{members[2]}@100000"))
            self.assertEqual(code, 0, report.get("failure"))
            self.assertTrue(report["live_session"]["headless_hashes_equal"])
            self.assertEqual(report["live_session"]["rejected"], [])
            ambient = report["battle_audio"]["ambient"]
            rows = [row for row in ambient["rows"] if row["unit"] in members]
            # Followers retain their birth timers while service is skipped.
            # Each promoted leader therefore services its overdue timer on
            # promotion, then schedules its next service from that frame.
            self.assertEqual([(row["tick"], row["unit"]) for row in rows],
                             [(30, members[0]), (35, members[1]),
                              (36, members[2]), (66, members[2])], rows)
            self.assertTrue(all(row["moving"] and not row["hidden"] for row in rows), rows)
            self.assertGreater(ambient["detached"], 0, ambient)
            self.assertGreaterEqual(ambient["retired"], 3, ambient)
            self.assertEqual(ambient["initialized"] - ambient["retired"], ambient["timers"])

    def test_fighter_flyby_attachment_mutes_when_the_craft_enters_fog(self):
        import struct
        import xml.etree.ElementTree as ET
        from tools.inventory.corpus import Corpus

        with tempfile.TemporaryDirectory(prefix="eawr-flyby-fog-") as temporary:
            directory = pathlib.Path(temporary)
            xml = directory / "mod/Data/XML"
            xml.mkdir(parents=True)
            (xml.parent / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
            corpus = Corpus(os.environ["EAWR_EAW_GAME_ROOT"])
            for filename in ("spaceunitsfighters.xml", "units_space_empire_tie_interceptor.xml"):
                source = corpus.read_effective("foc", "data/xml/" + filename)
                self.assertIsNotNone(source)
                tree = ET.fromstring(source.data)
                for craft in (node for node in tree if node.get("Name") in ("TIE_Fighter", "TIE_Interceptor")):
                    for key in ("SFXEvent_Ambient_Moving_Min_Delay_Seconds", "SFXEvent_Ambient_Moving_Max_Delay_Seconds"):
                        for item in list(craft):
                            if item.tag == key: craft.remove(item)
                        ET.SubElement(craft, key).text = "1"
                ET.ElementTree(tree).write(xml / filename, encoding="utf-8")
            seed = directory / "seed.eawr-replay"
            code, roster = self._run(directory, "roster", (
                "--eawr-mod-root", str(xml.parents[1]), "--eawr-live-ai", "off", "--eawr-live-ticks", "8",
                "--eawr-map-timed-frames", "1",
                "--eawr-live-replay-out", str(seed)))
            self.assertEqual(code, 0, roster.get("failure"))
            replay = bytearray(seed.read_bytes())
            self.assertEqual(replay[:8], b"EAWRPLY\0")
            self.assertEqual(struct.unpack_from("<Q", replay, 64)[0], 0)
            header = struct.unpack_from("<H", replay, 10)[0]
            players = struct.unpack_from("<I", replay, 48)[0]
            count = struct.unpack_from("<Q", replay, 56)[0]
            fighter_ids = next(squadron["members"] for squadron in roster["live_session"]["squadrons"]
                               if squadron["container"] == 9)
            self.assertGreater(len(fighter_ids), 0)
            # Stage the enemy fighter team in the Rebel station's stock sensor
            # circle, then order it out. Drawing reveal is deliberately off.
            for index in range(count):
                offset = header + players * 24 + index * 80
                entity, kind, owner = struct.unpack_from("<QQI", replay, offset)
                fighter = entity == 9 or entity in fighter_ids
                x, y = (600, -1500) if fighter else (0, -1500) if entity == 1 else (
                    (-9000 if owner == 1 else 9000), -9000)
                struct.pack_into("<7q", replay, offset + 24, x << 24, y << 24, 0, 0, 0, 0, 1 << 24)
            struct.pack_into("<Q", replay, 40, 650)
            path = directory / "fog.eawr-replay"
            path.write_bytes(replay)
            code, report = self._run(directory, "fog", (
                "--eawr-mod-root", str(xml.parents[1]), "--eawr-live-replay", str(path),
                "--eawr-live-player", "1", "--eawr-live-ai", "off", "--eawr-live-step", "1",
                "--eawr-live-ticks", "600", "--eawr-live-audio-pace", "on", "--eawr-hud", "off",
                "--eawr-live-order", "1:move:9@8000,-1500,0", "--eawr-live-follow-group", "0:9"),
                session=("--eawr-live-session", "replay"))
            self.assertEqual(code, 0, report.get("failure"))
            self.assertTrue(report["live_session"]["headless_hashes_equal"])
            self.assertEqual(report["live_session"]["rejected"], [])
            audio = report["battle_audio"]
            moving = [row for row in audio["ambient"]["rows"] if row["moving"]]
            self.assertTrue(any(not row["hidden"] for row in moving), moving)
            self.assertTrue(any(row["hidden"] for row in moving), moving)
            self.assertGreater(audio["ambient"]["hidden_updates"], 0, audio)
            self.assertGreater(audio["results"].get("ambient_moving", {}).get("hidden", 0), 0, audio)

    def test_sighting_announcements_are_once_per_type_and_battle(self):
        import xml.etree.ElementTree as ET
        from tools.inventory.corpus import Corpus

        with tempfile.TemporaryDirectory(prefix="eawr-sighting-audio-") as temporary:
            directory = pathlib.Path(temporary)
            xml = directory / "mod/Data/XML"
            xml.mkdir(parents=True)
            (xml.parent / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
            corpus = Corpus(os.environ["EAWR_EAW_GAME_ROOT"])
            for filename in ("factions.xml", "spaceunitsfrigates.xml"):
                source = corpus.read_effective("foc", "data/xml/" + filename)
                self.assertIsNotNone(source)
                tree = ET.fromstring(source.data)
                if filename == "factions.xml":
                    faction = next(node for node in tree if node.get("Name") == "Rebel")
                    ET.SubElement(faction, "SFXEvent_Unit_Type_Spotted").text = (
                        "Nebulon_B_Frigate, GUI_Toggle_Shields_On")
                else:
                    ship = next(node for node in tree if node.get("Name") == "Nebulon_B_Frigate")
                    for item in list(ship):
                        if item.tag == "Play_SFXEvent_On_Sighting": ship.remove(item)
                    ET.SubElement(ship, "Play_SFXEvent_On_Sighting").text = "True"
                    # M2 debug reveal affects drawing only. Use authored sensor
                    # data to make the enemy logically visible to this fixture.
                    for item in list(ship):
                        if item.tag == "Space_FOW_Reveal_Range": ship.remove(item)
                    ET.SubElement(ship, "Space_FOW_Reveal_Range").text = "50000"
                ET.ElementTree(tree).write(xml / filename, encoding="utf-8")
            code, report = self._run(directory, "sighting", (
                "--eawr-mod-root", str(xml.parents[1]), "--eawr-live-reveal", "on",
                "--eawr-live-step", "1", "--eawr-live-ticks", "180", "--eawr-live-audio-pace", "on"))
            self.assertEqual(code, 0, report.get("failure"))
            self.assertTrue(report["live_session"]["headless_hashes_equal"])
            audio = report["battle_audio"]
            self.assertEqual(audio["requested"].get("sighting_type:GUI_Toggle_Shields_On"), 1, audio)
            self.assertEqual(sum(n for key, n in audio["requested"].items() if key.startswith("sighting_enemy:")), 1, audio)
            starts = [row for row in audio["announcement_starts"] if row["reason"].startswith("sighting_")]
            self.assertEqual([row["reason"] for row in starts], ["sighting_type", "sighting_enemy"], starts)
            # A flagged friendly type is eligible too. The enemy on a later
            # roster entry announces independently, including in the same tick.
            self.assertLessEqual(starts[0]["tick"], starts[1]["tick"])
            self.assertFalse(audio["announcements"]["standalone_intro"])
            self.assertFalse(audio["announcements"]["skirmish_hero_respawn"])
            self.assertFalse(any("intro" in key or "respawn" in key for key in audio["requested"]))
            # An exact blank must remain silent even with an authored generic
            # cue. Dropping the blank token would incorrectly announce it.
            factions = ET.parse(xml / "factions.xml")
            faction = next(node for node in factions.getroot() if node.get("Name") == "Rebel")
            next(node for node in faction if node.tag == "SFXEvent_Unit_Type_Spotted"
                 and (node.text or "").startswith("Nebulon_B_Frigate,")).text = "Nebulon_B_Frigate,"
            ET.SubElement(faction, "SFXEvent_Generic_Unit_Spotted").text = "GUI_Toggle_Shields_On"
            factions.write(xml / "factions.xml", encoding="utf-8")
            code, blank = self._run(directory, "blank-sighting", (
                "--eawr-mod-root", str(xml.parents[1]), "--eawr-live-reveal", "on",
                "--eawr-live-step", "1", "--eawr-live-ticks", "180", "--eawr-live-audio-pace", "on"))
            self.assertEqual(code, 0, blank.get("failure"))
            self.assertTrue(blank["live_session"]["headless_hashes_equal"])
            self.assertFalse(any(key.startswith("sighting_type:") for key in blank["battle_audio"]["requested"]), blank["battle_audio"])
            self.assertEqual(sum(n for key, n in blank["battle_audio"]["requested"].items()
                                 if key.startswith("sighting_enemy:")), 1, blank["battle_audio"])

    def test_production_speech_fifo_precedes_sfx_and_ducks_wav_responses(self):
        import xml.etree.ElementTree as ET
        from tools.inventory.corpus import Corpus

        with tempfile.TemporaryDirectory(prefix="eawr-speech-audio-") as temporary:
            directory = pathlib.Path(temporary)
            xml = directory / "mod/Data/XML"
            xml.mkdir(parents=True)
            (xml.parent / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
            corpus = Corpus(os.environ["EAWR_EAW_GAME_ROOT"])
            # The first M2 card buys X-wings. A synthetic overlay uses existing
            # authored PCM speech through the stream backend, never the WAV VO pool.
            source = corpus.read_effective("foc", "data/xml/squadrons.xml")
            self.assertIsNotNone(source)
            tree = ET.fromstring(source.data)
            squadron = next(node for node in tree if node.get("Name") == "Rebel_X-Wing_Squadron")
            for item in list(squadron):
                if item.tag in ("Build_Speech_Underway", "Build_Speech_Stopped"): squadron.remove(item)
            ET.SubElement(squadron, "Build_Speech_Underway").text = "Speech_Death_Star_Build_Underway"
            ET.SubElement(squadron, "Build_Speech_Stopped").text = "Speech_Death_Star_Build_Stopped"
            ET.ElementTree(tree).write(xml / "squadrons.xml", encoding="utf-8")
            code, report = self._run(directory, "speech", (
                "--eawr-mod-root", str(xml.parents[1]), "--eawr-live-ai", "off", "--eawr-live-step", "1",
                "--eawr-live-input", "10:click:unit=1", "--eawr-live-input", "20:click:card=0",
                "--eawr-live-input", "30:rclick:hud=tqueue05", "--eawr-live-input", f"50:click:unit={NEBULON}",
                "--eawr-live-ticks", "600", "--eawr-live-audio-pace", "on"))
            self.assertEqual(code, 0, report.get("failure"))
            self.assertTrue(report["live_session"]["headless_hashes_equal"])
            self.assertEqual(report["live_session"]["rejected"], [])
            audio = report["battle_audio"]
            speech = audio["announcements"]
            self.assertEqual(speech["speech_queued"], 2, audio)
            self.assertEqual(speech["speech_completed"], 2, audio)
            self.assertEqual(speech["speech_failed"], 0, audio)
            self.assertEqual(speech["speech_overflow"], 0, audio)
            self.assertEqual(speech["speech_pending"], 0, audio)
            self.assertGreater(speech["ducked_updates"], 0, audio)
            starts = [row for row in audio["announcement_starts"] if row["reason"].startswith("speech_")]
            self.assertEqual([row["event"] for row in starts],
                             ["Speech_Death_Star_Build_Underway", "Speech_Death_Star_Build_Stopped"], starts)
            self.assertGreater(starts[1]["tick"] - starts[0]["tick"], 30, starts)
            self.assertFalse(any(key.startswith(("economy_build_started:", "economy_build_cancelled:"))
                                 for key in audio["requested"]), audio)

    def test_station_hardpoint_lines_cover_shields_and_missing_hangar_fallback(self):
        with tempfile.TemporaryDirectory(prefix="eawr-station-callout-") as temporary:
            code, report = self._run(pathlib.Path(temporary), "station-callouts", (
                "--eawr-live-ai", "off", "--eawr-live-reveal", "on", "--eawr-live-step", "1",
                "--eawr-live-audio-pace", "on", "--eawr-hud", "off",
                "--eawr-live-follow-group", f"1:{STATION}",
                "--eawr-live-input", f"10:click:unit={STATION}",
                "--eawr-live-follow-group", f"150:{EMPIRE_STATION}",
                "--eawr-live-input", f"300:hover:unit={EMPIRE_STATION}",
                "--eawr-live-input", f"310:rclick:reticle={EMPIRE_STATION}:5",
                "--eawr-live-input", f"420:hover:unit={EMPIRE_STATION}",
                "--eawr-live-input", f"430:rclick:reticle={EMPIRE_STATION}:3",
                "--eawr-live-input", f"540:hover:unit={EMPIRE_STATION}",
                "--eawr-live-input", f"550:rclick:reticle={EMPIRE_STATION}:6",
                "--eawr-live-ticks", "620"))
            self.assertEqual(code, 0, report.get("failure"))
            self.assertTrue(report["live_session"]["headless_hashes_equal"])
            self.assertEqual(report["live_session"]["rejected"], [])
            self.assertEqual(report["battle_input"]["hardpoint_orders"], 3, report["battle_input"])
            # Event lookup is case insensitive; the catalog retains its authored spelling.
            requested = {key.lower(): count for key, count in report["battle_audio"]["requested"].items()}
            self.assertEqual(requested.get("response_attack_hardpoint:unit_hp_shields_rebel_space_station"), 1, requested)
            self.assertEqual(requested.get("response_attack_hardpoint:unit_hp_laser_rebel_space_station"), 1, requested)
            # Stock M2 speakers do not author a fighter-bay response. The aimed bay uses ordinary attack.
            self.assertEqual(requested.get("response_attack:unit_attack_rebel_space_station"), 1, requested)

    def test_empire_speaker_uses_the_targets_ion_hardpoint_kind(self):
        with tempfile.TemporaryDirectory(prefix="eawr-ion-callout-") as temporary:
            code, report = self._run(pathlib.Path(temporary), "ion-callout", (
                "--eawr-live-player", "2", "--eawr-live-ai", "off", "--eawr-live-reveal", "on",
                "--eawr-live-step", "1", "--eawr-live-audio-pace", "on", "--eawr-hud", "off",
                "--eawr-live-follow-group", f"1:{EMPIRE_STATION}",
                "--eawr-live-input", f"120:click:unit={EMPIRE_STATION}",
                "--eawr-live-follow-group", f"220:{MC80}",
                "--eawr-live-input", f"390:hover:unit={MC80}",
                "--eawr-live-input", f"400:rclick:reticle={MC80}:0",
                "--eawr-live-ticks", "470"))
            self.assertEqual(code, 0, report.get("failure"))
            self.assertTrue(report["live_session"]["headless_hashes_equal"])
            self.assertEqual(report["live_session"]["rejected"], [])
            self.assertEqual(report["battle_input"]["hardpoint_orders"], 1, report["battle_input"])
            requested = {key.lower(): count for key, count in report["battle_audio"]["requested"].items()}
            self.assertEqual(requested.get("response_attack_hardpoint:unit_hp_ion_empire_space_station"), 1, requested)

    def test_stop_and_guard_play_the_ranked_unit_response(self):
        with tempfile.TemporaryDirectory(prefix="eawr-order-audio-") as temporary:
            code, report = self._run(pathlib.Path(temporary), "stop-guard", (
                "--eawr-live-ai", "off", "--eawr-live-step", "1", "--eawr-live-audio-pace", "on",
                "--eawr-live-order", f"1:move:{NEBULON}@-4700,5400,0",
                "--eawr-live-follow-group", f"1:{NEBULON}",
                "--eawr-live-input", f"300:click:unit={NEBULON}",
                "--eawr-live-input", "420:key:S",
                "--eawr-live-input", "540:key:G",
                "--eawr-live-input", "550:rclick:@-4500,5600,0",
                "--eawr-live-ticks", "650"))
            self.assertEqual(code, 0, report.get("failure"))
            self.assertTrue(report["live_session"]["headless_hashes_equal"])
            self.assertEqual(report["live_session"]["rejected"], [])
            requested = report["battle_audio"]["requested"]
            self.assertEqual(requested.get("response_stop:Unit_Stop_Nebulon"), 1, requested)
            self.assertEqual(requested.get("response_guard:Unit_Guard_Nebulon"), 1, requested)
            starts = report["battle_audio"]["ability_starts"]
            for reason, tick in (("response_stop", 420), ("response_guard", 550)):
                rows = [row for row in starts if row["reason"] == reason]
                self.assertEqual(len(rows), 1, starts)
                self.assertAlmostEqual(rows[0]["tick"], tick, delta=2)

    def test_reticle_attack_plays_the_speakers_hardpoint_line(self):
        # The established M2 reticle route; selection settles before the attack response.
        with tempfile.TemporaryDirectory(prefix="eawr-hardpoint-audio-") as temporary:
            code, report = self._run(pathlib.Path(temporary), "hardpoint", (
                "--eawr-live-ai", "off", "--eawr-live-reveal", "on", "--eawr-live-step", "1",
                "--eawr-live-audio-pace", "on", "--eawr-hud", "off",
                "--eawr-live-order", f"1:move:{CORVETTE}@-3300,3300,0",
                "--eawr-live-order", f"1:move:{ACCLAMATOR}@-1192,1054,0",
                "--eawr-live-follow-group", f"1:{CORVETTE}",
                "--eawr-live-input", f"900:click:unit={CORVETTE}",
                "--eawr-live-follow-group", f"2900:{ACCLAMATOR}",
                "--eawr-live-input", f"2980:hover:unit={ACCLAMATOR}",
                "--eawr-live-input", f"3000:rclick:reticle={ACCLAMATOR}:first",
                "--eawr-live-input", f"3110:hover:unit={ACCLAMATOR}",
                "--eawr-live-input", f"3120:rclick:reticle={ACCLAMATOR}:6",
                "--eawr-live-ticks", "3210"))
            self.assertEqual(code, 0, report.get("failure"))
            self.assertTrue(report["live_session"]["headless_hashes_equal"])
            self.assertEqual(report["live_session"]["rejected"], [])
            self.assertEqual(report["battle_input"]["hardpoint_orders"], 2, report["battle_input"])
            requested = {key.lower(): count for key, count in report["battle_audio"]["requested"].items()}
            self.assertEqual(requested.get("response_attack_hardpoint:unit_hp_engines_corvette"), 1, requested)
            self.assertEqual(sum(n for key, n in requested.items() if key.startswith("response_attack_hardpoint:")), 2, requested)
            self.assertFalse(any(key.startswith("response_attack:") for key in requested), requested)
            starts = [row for row in report["battle_audio"]["ability_starts"] if row["reason"] == "response_attack_hardpoint"]
            self.assertEqual(len(starts), 2, report["battle_audio"])
            self.assertAlmostEqual(starts[0]["tick"], 3000, delta=2)
            self.assertAlmostEqual(starts[1]["tick"], 3120, delta=2)

    def test_production_and_hyperspace_emit_authored_cues_once(self):
        # BA-60/62: ordinary unit production and every arriving craft, at frame 35.
        with tempfile.TemporaryDirectory(prefix="eawr-production-audio-") as temporary:
            directory = pathlib.Path(temporary)
            code, report = self._run(directory, "arrival", (
                "--eawr-live-ai", "off", "--eawr-live-step", "1",
                "--eawr-live-input", "10:click:unit=1",
                "--eawr-live-input", "20:click:card=0",
                "--eawr-live-input", "500:click:hud=b_reinforcement",
                "--eawr-live-input", "510:press:hud=r_0000",
                "--eawr-live-input", "515:hover:@-3111,3960,0",
                "--eawr-live-input", "520:release:@-3111,3960,0",
                "--eawr-live-follow", "1", "--eawr-live-reveal", "on",
                "--eawr-live-ticks", "720", "--eawr-live-audio-pace", "on"))
            self.assertEqual(code, 0, report.get("failure"))
            self.assertTrue(report["live_session"]["headless_hashes_equal"])
            self.assertEqual(report["live_session"]["rejected"], [])
            audio = report["battle_audio"]
            requested = audio["requested"]
            for reason in ("economy_build_started:", "economy_build_complete:"):
                self.assertEqual(sum(n for key, n in requested.items() if key.startswith(reason)), 1, requested)
            arrivals = [row for row in report["live_session"]["arrivals"] if row["owner"] == 1]
            self.assertGreater(len(arrivals), 1, report["live_session"])
            # The report includes a team container: only its five physical craft sound.
            self.assertEqual(sum(n for key, n in requested.items() if key.startswith("hyperspace_arrival:")), 5, requested)
            starts = [row for row in audio["ability_starts"] if row["reason"] == "hyperspace_arrival"]
            self.assertTrue(starts, audio)
            for row in starts:
                self.assertAlmostEqual(row["tick"], min(a["first_tick"] for a in arrivals) + 35, delta=2)
            self.assertEqual(audio["missing_samples"], {})

    def test_cancelled_production_emits_no_complete_cue(self):
        with tempfile.TemporaryDirectory(prefix="eawr-cancel-audio-") as temporary:
            code, report = self._run(pathlib.Path(temporary), "cancel", (
                "--eawr-live-ai", "off", "--eawr-live-step", "1",
                "--eawr-live-input", "10:click:unit=1", "--eawr-live-input", "20:click:card=0",
                "--eawr-live-input", "100:rclick:hud=tqueue05", "--eawr-live-ticks", "550"))
            self.assertEqual(code, 0, report.get("failure"))
            self.assertTrue(report["live_session"]["headless_hashes_equal"])
            requested = report["battle_audio"]["requested"]
            self.assertEqual(sum(n for key, n in requested.items() if key.startswith("economy_build_started:")), 1, requested)
            self.assertEqual(sum(n for key, n in requested.items() if key.startswith("economy_build_cancelled:")), 1, requested)
            self.assertFalse(any(key.startswith("economy_build_complete:") for key in requested), requested)

    def test_spin_sound_replaces_death_until_the_copy_finishes(self):
        # SP-03/SP-08: seeded X-wing 54 and TIE interceptor 67 spin; Y-wing 64 explodes.
        # Stop once during the spin and once after it to distinguish the two cues.
        with tempfile.TemporaryDirectory(prefix="eawr-spin-audio-") as temporary:
            directory = pathlib.Path(temporary)
            for ticks, deaths in ((190, 1), (230, 3)):
                code, report = self._run(directory, f"spin-audio-{ticks}", (
                    "--eawr-live-ai", "off", "--eawr-live-order", "1:move:2@-1000,4700,0",
                    "--eawr-live-order", "1:move:9@1000,-4393,0",
                    "--eawr-live-order", "150:damage:54@1000000",
                    "--eawr-live-order", "150:damage:67@1000000",
                    "--eawr-live-order", "152:damage:64@1000000",
                    "--eawr-live-follow", "54", "--eawr-live-reveal", "on",
                    "--eawr-live-ticks", str(ticks), "--eawr-live-step", "1",
                    "--eawr-live-audio-pace", "on"))
                self.assertEqual(code, 0, report.get("failure"))
                self.assertTrue(report["live_session"]["headless_hashes_equal"])
                self.assertEqual(report["live_session"]["rejected"], [])
                audio = report["battle_audio"]
                requested = audio["requested"]
                self.assertEqual(requested.get("spin_death:Unit_X_Wing_Spinning_By"), 1, requested)
                self.assertEqual(requested.get("spin_death:Unit_TIE_Fighter_Spinning_By"), 1, requested)
                self.assertEqual(sum(n for key, n in requested.items() if key.startswith("death:")), deaths, requested)
                self.assertEqual(audio["results"]["spin_death"], {"playing": 2})
                self.assertGreater(audio["attached_moves"], 5)
                self.assertEqual(audio["missing_samples"], {})

    def test_lane_mute_keeps_the_audio_event_log(self):
        with tempfile.TemporaryDirectory(prefix="eawr-lane-mute-") as temporary:
            directory = pathlib.Path(temporary)
            results = []
            for name, flag, mute in (("explicit", "off", "0"), ("lane", "on", "1")):
                code, report = self._run(directory, name, (
                    *DUEL_ARGS, "--eawr-live-ticks", "750", "--eawr-live-audio-pace", "on",
                    "--eawr-audio", flag, "--eawr-live-input", "30:click:unit=1",
                    "--eawr-live-input", "60:rclick:unit=2"),
                    session=("--eawr-live-session", "replay"), camera=DUEL_CAMERA,
                    env=dict(os.environ, EAWR_AUDIO_MUTE=mute))
                self.assertEqual(code, 0, report.get("failure"))
                self.assertIs(report["battle_audio"]["muted"], True)
                self.assertIs(report["live_session"]["headless_hashes_equal"], True)
                results.append(report)
            before, after = results
            self.assertEqual(before["live_session"]["final_state_sha256"],
                             after["live_session"]["final_state_sha256"])
            for field in ("requested", "responses", "music", "abilities", "ability_starts"):
                self.assertEqual(before["battle_audio"][field], after["battle_audio"][field], field)
            self.assertTrue(after["battle_audio"]["requested"])
            self.assertTrue(after["battle_audio"]["responses"])


    def test_duel_plays_its_sounds(self):
        # #84: the duel's shots, hits and the Tartan's death start FoC's SFXEvents, the player's
        # selection and attack order the Nebulon-B's responses, and the first Rebel shot the battle
        # music; the capture run is muted by default and the session's hashes stay headless.
        # #474: --eawr-live-audio-pace on caps the render rate to the battle's own tick rate, so
        # Godot's real audio engine schedules the same sound on every host regardless of how fast
        # it can otherwise draw frames (docs/behaviour/battle-audio.md#474).
        with tempfile.TemporaryDirectory(prefix="eawr-live-audio-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "audio", (
                *DUEL_ARGS, "--eawr-live-ticks", "750", "--eawr-live-capture-ticks", "720",
                "--eawr-live-audio-pace", "on",
                "--eawr-live-input", "30:click:unit=1", "--eawr-live-input", "60:rclick:unit=2"),
                session=("--eawr-live-session", "replay"), camera=DUEL_CAMERA)
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            sound = result["battle_audio"]
            self.assertIs(sound["muted"], True)
            self.assertGreater(sound["sfx_events"], 3000)
            self.assertEqual((sound["space_3d"]["saturation_factor"], sound["space_3d"]["rolloff_factor"],
                              sound["space_3d"]["listener_z"]), (1.5, 2.0, 60.0))
            # Nothing the duel asks for is missing or undecodable.
            self.assertEqual(sound["missing_samples"], {})
            self.assertEqual(sound["problems"], [])
            requested = sound["requested"]
            self.assertGreater(sum(n for key, n in requested.items() if key.startswith("fire:")), 0, requested)
            self.assertGreater(sum(n for key, n in requested.items() if key.startswith("hit:")), 0, requested)
            self.assertEqual(requested.get("death:Unit_Corvette_Death_SFX"), 1, requested)
            results = sound["results"]
            self.assertGreater(results["fire"].get("playing", 0), 0, results)
            self.assertGreater(results["hit"].get("playing", 0), 0, results)
            self.assertEqual(results["death"], {"playing": 1}, results)
            self.assertNotIn("sample_missing", results["fire"])
            self.assertLessEqual(sound["max_voices"], 48)
            # #443: at the duel camera the shots and hits are heard as FoC balances them against
            # the music (its data volumes, BA-11 falloff and the 0.75 sliders, BA-45): the mixer
            # meters sound on the SFX bus for most of the fight, within a sane band of the music,
            # and the mix never reaches full scale. Before the fix the 3D players had no listener
            # and the SFX bus stayed silent (no frame above -60 dBFS).
            levels = sound["levels"]
            sfx, music, mix = levels["EAWR_SFX"], levels["EAWR_Music"], levels["EAWR_Mix"]
            self.assertGreater(sfx["frames"], sound["frames"] // 2, levels)
            self.assertGreater(music["frames"], sound["frames"] // 2, levels)
            self.assertGreaterEqual(sfx["rms_db"], music["rms_db"] - 6.0, levels)
            self.assertLessEqual(sfx["rms_db"], music["rms_db"] + 12.0, levels)
            self.assertLessEqual(mix["max_peak_db"], 0.0, levels)
            self.assertEqual(mix["clipped_frames"], 0, levels)
            self.assertLess(sound["start_gain_db"]["fire"]["mean"], 0.0)
            self.assertGreater(sound["start_gain_db"]["fire"]["mean"], -20.0)
            # BA-20 to BA-22: the Nebulon-B answers the click and the attack order on the Tartan.
            self.assertEqual(sound["responses"], ["select Nebulon_B_Frigate Unit_Select_Nebulon",
                                                  "attack Nebulon_B_Frigate Unit_Attack_Nebulon"])
            self.assertEqual(results["response_select"], {"playing": 1}, results)
            # BA-41 to BA-43: Rebel ambient first, then battle music with the first Rebel shot.
            music = sound["music"]
            self.assertIn(" ambient Space_Map_Rebel_Ambient_Music_Event Imperial_Attack_1.MP3", music[0])
            self.assertTrue(any(" battle Space_Map_Rebel_Battle_Music_Event UND_Unexpected_Forces.MP3" in row
                                for row in music), music)
            self.assertEqual(sound["music_mode"], "battle")


    def test_squadron_selection_survives_its_leader_dying(self):
        # #499 review: the squadron's leading craft must be resolved by liveness, not simply the
        # first roster member the audio system last stood next to -- a dead craft stays in that
        # position map until its destruction event reaches the report, a tick or more after it
        # actually leaves the tactical snapshot. This run kills the Y-wing squadron's first two
        # craft (#424, test_battle_input.py: squadron 4, craft 46-48 in roster order) before the
        # icon is clicked. Every M2 squadron is one craft type, so a leader-agnostic roster walk
        # would happen to land on the same sound anyway; the regression this guards is a dropped
        # response (the resolution falling through to the team container's own, missing fields), not
        # a wrong one.
        y_wing_squadron = 4
        y_wings = (46, 47, 48)
        spread = ("--eawr-live-order", "1:move:5@-4450,5050,0", "--eawr-live-order", "1:move:2@-5450,5250,0",
                  "--eawr-live-order", "1:move:3@-4650,4250,0")
        with tempfile.TemporaryDirectory(prefix="eawr-live-squadron-leader-death-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "y-wing-leader-death", (
                *spread, "--eawr-hud", "off", "--eawr-live-ticks", "90",
                "--eawr-live-order", f"15:damage:{y_wings[0]}@100000",
                "--eawr-live-order", f"16:damage:{y_wings[1]}@100000",
                "--eawr-live-input", f"30:click:icon={y_wing_squadron}"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            sound = result["battle_audio"]
            results = sound["results"]
            self.assertEqual(results.get("death"), {"playing": 2}, results)
            self.assertEqual(sound["responses"], ["select Y-Wing Unit_Select_Y_Wing"])
            self.assertEqual(results.get("response_select"), {"playing": 1}, results)


    def test_ability_presses_play_their_sounds(self):
        # #559 (docs/behaviour/battle-audio.md BA-50 to BA-52): switching an ability on or off plays
        # its faction's toggle sound ("swhoong" of the S-foils, the shields) and, from the command
        # bar, the unit's own voice line; a natural end and the enemy's switches play nothing. The
        # box selects the whole Rebel fleet and each modelled ability's button is pressed on, then
        # off (the cut HUNT and ION_CANNON_SHOT are refused, AB-03).
        width, height = 1280, 720
        box = f"screen={0.05 * width:.3f},{0.05 * height:.3f}/screen={0.95 * width:.3f},{0.95 * height:.3f}"
        boxed = ("--eawr-live-input", f"20:box:{box}")
        with tempfile.TemporaryDirectory(prefix="eawr-live-ability-audio-") as temporary:
            directory = pathlib.Path(temporary)
            code, shown = self._run(directory, "buttons", (*boxed, "--eawr-live-ticks", "60"))
            self.assertEqual(code, 0, shown.get("failure"))
            buttons = shown["hud"]["ability_buttons"]["buttons"]
            modelled = [index for index, button in enumerate(buttons)
                        if button["name"] in ("DEFEND", "TURBO", "SPOILER_LOCK")]
            self.assertEqual(sorted(buttons[index]["name"] for index in modelled), ["DEFEND", "SPOILER_LOCK", "TURBO"])
            presses = []
            for step, index in enumerate(modelled):
                presses += ["--eawr-live-input", f"{30 + 6 * step}:click:ability={index}",
                            "--eawr-live-input", f"{120 + 6 * step}:click:ability={index}"]
            code, result = self._run(directory, "presses", (*boxed, *presses, "--eawr-live-ticks", "200"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            sound = result["battle_audio"]
            self.assertEqual(sound["missing_samples"], {})
            self.assertEqual(sound["problems"], [])
            log = sound["abilities"]
            for row in (
                "voice Nebulon_B_Frigate DEFEND on Unit_Defend_Nebulon",
                "voice Nebulon_B_Frigate DEFEND off <none>",
                "voice Corellian_Corvette TURBO on Unit_Speed_Corvette",
                "voice Corellian_Corvette TURBO off <none>",
                "voice X-Wing SPOILER_LOCK on Unit_Ability_On_X_Wing",
                "voice X-Wing SPOILER_LOCK off Unit_Ability_Off_X_Wing",
                "toggle Rebel DEFEND on GUI_Toggle_Shields_On",
                "toggle Rebel DEFEND off GUI_Toggle_Shields_Off",
                "toggle Rebel TURBO on GUI_Toggle_Turbo_On",
                "toggle Rebel TURBO off GUI_Toggle_Turbo_Off",
                "toggle Rebel SPOILER_LOCK on GUI_Toggle_Turbo_On",
                "toggle Rebel SPOILER_LOCK off GUI_Toggle_Turbo_Off",
            ):
                self.assertIn(row, log)
            # One toggle sound per switch, however many craft a squadron has (BA-50).
            self.assertEqual(sum(1 for row in log if row.startswith("toggle Rebel SPOILER_LOCK on")), 1, log)
            requested = sound["requested"]
            self.assertEqual(requested.get("ability_toggle:GUI_Toggle_Turbo_On"), 2, requested)
            self.assertEqual(requested.get("ability_voice:Unit_Speed_Corvette"), 1, requested)
            self.assertEqual(requested.get("ability_voice:Unit_Ability_Off_X_Wing"), 1, requested)
            # The enemy's own switches (the Tartan's POWER_TO_WEAPONS has none, and an enemy's
            # toggle entries are blank) never play.
            self.assertFalse(any(" enemy " in row and not row.endswith("<none>") for row in log), log)


    def test_timed_ability_dial_stays_visible_at_zero_remaining(self):
        # AB-05, #642: the last active snapshot has zero remaining duration; it
        # must still draw the full dial before the recharge countdown takes over.
        box = "screen=64,36/screen=1216,684"
        with tempfile.TemporaryDirectory(prefix="eawr-live-ability-dial-expiry-") as temporary:
            directory = pathlib.Path(temporary)
            for tick, completion in ((649, 1 / 600), (650, 0), (651, 1 / 1500)):
                code, result = self._run(directory, f"dial-{tick}", (
                    "--eawr-live-order", "2:move:5@4000,4700,0",
                    "--eawr-live-input", f"20:box:{box}",
                    "--eawr-live-order", "50:ability:5@TURBO,on",
                    "--eawr-live-step", "1", "--eawr-live-ticks", str(tick)))
                self.assertEqual(code, 0, result.get("failure"))
                self.assertIs(result["live_session"]["headless_hashes_equal"], True)
                # The simulation can publish one tick ahead of the displayed frame.
                self.assertEqual(result["live_session"]["presented_tick"], tick)
                drawn = result["hud"]["ability_buttons"]
                turbo = [button for button in drawn["buttons"] if button["name"] == "TURBO"]
                self.assertEqual(len(turbo), 1, drawn)
                self.assertAlmostEqual(turbo[0]["recharge"], completion, places=6)
                marks = [mark for mark in drawn["marks"] if mark["icon"] == turbo[0]["icon"]]
                self.assertEqual(len(marks), 1, drawn)
                self.assertIsNotNone(marks[0]["dial"], drawn)
                self.assertAlmostEqual(marks[0]["dial"], completion, places=6)
                self.assertGreaterEqual(drawn["dials_drawn"], 2, drawn)

    def test_timed_abilities_that_run_out_play_no_off_sound(self):
        # #559 (BA-51): a timed ability that ends by itself starts no sound. TURBO (20 s, 600 ticks)
        # and DEFEND (15 s, 450 ticks) are switched on by script and left to expire. The snapshot of
        # a timed ability's last active tick reads zero frames left, but retains its duration.
        # BA-51 still distinguishes this natural expiration from a manual switch-off.
        with tempfile.TemporaryDirectory(prefix="eawr-live-ability-expiry-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "expiry", (
                "--eawr-live-order", "2:move:5@4000,4700,0", "--eawr-live-order", "50:ability:5@TURBO,on",
                "--eawr-live-order", "60:ability:6@DEFEND,on", "--eawr-live-ticks", "700"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            sound = result["battle_audio"]
            log = sound["abilities"]
            self.assertIn("toggle Rebel TURBO on GUI_Toggle_Turbo_On", log)
            self.assertIn("toggle Rebel DEFEND on GUI_Toggle_Shields_On", log)
            self.assertIn("expired TURBO", log)
            self.assertIn("expired DEFEND", log)
            self.assertFalse(any(row.startswith("toggle") and " off " in row for row in log), log)
            starts = sound["ability_starts"]
            self.assertFalse(any(row["event"].endswith("_Off") for row in starts), starts)
            self.assertEqual(sorted(row["event"] for row in starts), ["GUI_Toggle_Shields_On", "GUI_Toggle_Turbo_On"])
            requested = sound["requested"]
            self.assertIsNone(requested.get("ability_toggle:GUI_Toggle_Turbo_Off"), requested)
            self.assertIsNone(requested.get("ability_toggle:GUI_Toggle_Shields_Off"), requested)


    def test_switching_a_timed_ability_off_plays_its_off_sound(self):
        # #559 (BA-50): TURBO switched off by script before its time is out is a switch-off, not a
        # natural end: the faction's off toggle plays once, at the tick of the switch, and nothing
        # is logged as expired.
        with tempfile.TemporaryDirectory(prefix="eawr-live-ability-off-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "off", (
                "--eawr-live-order", "2:move:5@4000,4700,0", "--eawr-live-order", "50:ability:5@TURBO,on",
                "--eawr-live-order", "170:ability:5@TURBO,off", "--eawr-live-ticks", "260"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            sound = result["battle_audio"]
            log = sound["abilities"]
            self.assertEqual([row for row in log if row.startswith("toggle")],
                             ["toggle Rebel TURBO on GUI_Toggle_Turbo_On", "toggle Rebel TURBO off GUI_Toggle_Turbo_Off"], log)
            self.assertFalse(any(row.startswith("expired") for row in log), log)
            starts = sound["ability_starts"]
            self.assertEqual([row["event"] for row in starts], ["GUI_Toggle_Turbo_On", "GUI_Toggle_Turbo_Off"], starts)
            self.assertAlmostEqual(starts[1]["tick"], 170, delta=4)


    def test_enemy_switches_play_the_enemy_row(self):
        # #559 (BA-50): with the Empire as the local player the Rebel corvette's TURBO is an enemy's
        # switch: the faction's enemy rows are blank, so nothing plays (the log names the blank row).
        with tempfile.TemporaryDirectory(prefix="eawr-live-ability-enemy-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "enemy", (
                "--eawr-live-player", "2", "--eawr-live-order", "50:ability:5@TURBO,on",
                "--eawr-live-order", "110:ability:5@TURBO,off", "--eawr-live-ticks", "160"))
            self.assertEqual(code, 0, result.get("failure"))
            sound = result["battle_audio"]
            self.assertEqual([row for row in sound["abilities"] if row.startswith("toggle")],
                             ["toggle Rebel TURBO on enemy <none>", "toggle Rebel TURBO off enemy <none>"], sound["abilities"])
            self.assertEqual(sound["ability_starts"], [])
