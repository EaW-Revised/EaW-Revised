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
    source_text, strict_json, subprocess, sys, squadron_members,
    tempfile, time, unittest,
)


class LiveSessionAudioCases:
    def test_station_upgrade_announces_owner_faction_once(self):
        import struct
        import xml.etree.ElementTree as ET
        import zlib
        from tools.inventory.corpus import Corpus
        from test_live_team_colour import command
        from test_pad_capture import replay_units

        # WPR-52 / EUS-25: mixed-faction allies are covered by the isolated
        # announcement contract; stock teams and replay loading allow one faction per team.
        with tempfile.TemporaryDirectory(prefix="eawr-upgrade-announcer-") as temporary:
            directory = pathlib.Path(temporary)
            xml = directory / "mod/Data/XML"
            xml.mkdir(parents=True)
            (xml.parent / "MegaFiles.xml").write_text(
                "<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
            corpus = Corpus(os.environ["EAWR_EAW_GAME_ROOT"])
            upgrades = ET.fromstring(corpus.read_effective("foc", "data/xml/upgradeobjects.xml").data)
            upgrade = next(node for node in upgrades if node.get("Name") == "RS_Level_Two_Starbase_Upgrade")
            upgrade.find("Tactical_Build_Time_Seconds").text = "1"
            ET.ElementTree(upgrades).write(xml / "upgradeobjects.xml", encoding="utf-8")
            factions = ET.fromstring(corpus.read_effective("foc", "data/xml/factions.xml").data)
            rebel = next(node for node in factions if node.get("Name") == "Rebel")
            # Distinct authored events make the owner and relationship observable,
            # including the allied field which stock data may leave empty.
            fields = {"own": "SFXEvent_Starbase_Upgraded", "enemy": "SFXEvent_Starbase_Enemy_Upgraded"}
            events = {"own": "RHD_Upgrade_Complete", "enemy": "RHD_Upgrade_Progress"}
            for relation, field in fields.items():
                node = rebel.find(field)
                if node is None: node = ET.SubElement(rebel, field)
                node.text = events[relation]
            ET.ElementTree(factions).write(xml / "factions.xml", encoding="utf-8")
            common = ("--eawr-mod-root", str(xml.parents[1]), "--eawr-live-ai", "off",
                      "--eawr-live-step", "3", "--eawr-live-reveal", "on")
            seed = directory / "seed.eawr-replay"
            code, initial = self._run(directory, "seed", (*common, "--eawr-live-ticks", "1",
                "--eawr-live-replay-out", str(seed)))
            self.assertEqual(code, 0, initial.get("failure"))
            station = next(row for row in replay_units(seed)
                           if row[1] == zlib.crc32(b"SKIRMISH_REBEL_STAR_BASE_1"))
            data = bytearray(seed.read_bytes())
            self.assertEqual(struct.unpack_from("<Q", data, 64)[0], 0)
            struct.pack_into("<Q", data, 40, 120)
            struct.pack_into("<Q", data, 64, 1)
            data += command(10, station[2], 1, 9,
                            struct.pack("<Q", zlib.crc32(b"RS_LEVEL_TWO_STARBASE_UPGRADE")), (station[0],))
            upgraded_hash = None
            for relation, local in (("own", 1), ("enemy", 2), ("empty", 2)):
                with self.subTest(relation=relation):
                    replay = directory / f"{relation}.eawr-replay"
                    fixture = bytearray(data)
                    if relation == "empty":
                        rebel.find(fields["enemy"]).text = ""
                        ET.ElementTree(factions).write(xml / "factions.xml", encoding="utf-8")
                    replay.write_bytes(fixture)
                    code, result = self._run(directory, relation, (*common, "--eawr-live-player", str(local),
                        "--eawr-live-ticks", "120"),
                        session=("--eawr-live-session", "replay", "--eawr-live-replay", str(replay)))
                    self.assertEqual(code, 0, result.get("failure"))
                    live = result["live_session"]
                    self.assertTrue(live["headless_hashes_equal"])
                    self.assertEqual(live["rejected"], [])
                    if relation == "own":
                        self.assertEqual(live["economy"]["tech_level"], 2)
                        upgraded_hash = live["final_state_sha256"]
                    else:
                        self.assertEqual(live["final_state_sha256"], upgraded_hash)
                    requested = result["battle_audio"]["requested"]
                    upgrades_heard = {key: value for key, value in requested.items()
                                      if key.startswith("station_upgraded:")}
                    expected = {} if relation == "empty" else {"station_upgraded:" + events[relation]: 1}
                    self.assertEqual(upgrades_heard, expected)

    def test_sfx_lifecycle_delays_repeats_and_authored_chain(self):
        import math
        import struct
        import wave
        import xml.etree.ElementTree as ET
        from tools.inventory.corpus import Corpus

        with tempfile.TemporaryDirectory(prefix="eawr-sfx-lifecycle-") as temporary:
            directory = pathlib.Path(temporary)
            xml = directory / "mod/Data/XML"
            xml.mkdir(parents=True)
            (xml.parent / "MegaFiles.xml").write_text(
                "<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
            corpus = Corpus(os.environ["EAWR_EAW_GAME_ROOT"])
            tree = ET.fromstring(corpus.read_effective("foc", "data/xml/sfxeventsgui.xml").data)
            for name, fields in (
                ("LifecycleStages", {
                    "Is_3D": "No", "Play_Count": "2", "Max_Instances": "1", "Play_Sequentially": "Yes",
                    "Pre_Samples": "life-pre.wav", "Samples": "life-main.wav", "Post_Samples": "life-post.wav",
                    "Min_Predelay": "80", "Max_Predelay": "80", "Min_Postdelay": "100", "Max_Postdelay": "100",
                    "Min_Volume": "70", "Max_Volume": "70", "Min_Pan2D": "25", "Max_Pan2D": "25",
                    "Chained_SFXEvent": "LifecycleChain"}),
                ("LifecycleChain", {"Is_3D": "No", "Samples": "life-chain.wav", "Max_Instances": "1"}),
            ):
                node = ET.SubElement(tree, "SFXEvent", Name=name)
                for tag, value in fields.items():
                    ET.SubElement(node, tag).text = value
            ET.ElementTree(tree).write(xml / "sfxeventsgui.xml", encoding="utf-8")
            tree = ET.fromstring(corpus.read_effective("foc", "data/xml/spaceunitscorvettes.xml").data)
            corvette = next(node for node in tree if node.get("Name") == "Corellian_Corvette")
            corvette.find("SFXEvent_Move").text = "LifecycleStages"
            ET.ElementTree(tree).write(xml / "spaceunitscorvettes.xml", encoding="utf-8")
            for stage, hz in (("pre", 440), ("main", 660), ("post", 880), ("chain", 1100)):
                sound = xml.parent / f"Audio/SFX/life-{stage}.wav"
                sound.parent.mkdir(parents=True, exist_ok=True)
                with wave.open(str(sound), "wb") as stream:
                    stream.setparams((1, 2, 22050, 0, "NONE", "not compressed"))
                    stream.writeframes(b"".join(struct.pack("<h", int(3000 * math.sin(2 * math.pi * hz * i / 22050)))
                                                for i in range(2205)))
            code, report = self._run(directory, "lifecycle", (
                "--eawr-mod-root", str(xml.parents[1]), "--eawr-live-ai", "off", "--eawr-live-step", "1",
                "--eawr-live-ticks", "200", "--eawr-live-audio-pace", "on", "--eawr-map-timed-frames", "1",
                "--eawr-live-input", f"10:click:unit={CORVETTE}",
                "--eawr-live-input", "30:rclick:@-5000,5900,0", "--eawr-live-input", "31:rclick:@-5100,5900,0"))
            self.assertEqual(code, 0, report.get("failure"))
            self.assertTrue(report["live_session"]["headless_hashes_equal"])
            audio = report["battle_audio"]
            self.assertEqual(audio["requested"].get("response_move:LifecycleStages"), 2, audio)
            self.assertEqual(audio["results"].get("response_move"), {"playing": 1, "instance_limit": 1}, audio)
            rows = [row for row in audio["ability_starts"] if row["event"] in ("LifecycleStages", "LifecycleChain")]
            self.assertEqual([row["sample"] for row in rows],
                             ["life-pre.wav", "life-main.wav", "life-post.wav"] * 2 + ["life-chain.wav"], audio)
            for earlier, later in ((rows[2], rows[3]), (rows[5], rows[6])):
                self.assertGreaterEqual(later["tick"] - earlier["tick"], 6, audio)
            self.assertEqual(audio["allocations"]["response_move"]["admitted"], 1, audio)
            self.assertEqual(audio["allocations"]["response_move"]["allocated"], 6, audio)
            self.assertEqual(audio["requested"].get("authored_chain:LifecycleChain"), 1, audio)

    def test_game_pause_preserves_spatial_loops_2d_loops_and_speech(self):
        import math
        import struct
        import wave
        import xml.etree.ElementTree as ET
        from tools.inventory.corpus import Corpus

        with tempfile.TemporaryDirectory(prefix="eawr-audio-pause-") as temporary:
            directory = pathlib.Path(temporary)
            xml = directory / "mod/Data/XML"
            xml.mkdir(parents=True)
            (xml.parent / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
            corpus = Corpus(os.environ["EAWR_EAW_GAME_ROOT"])
            tree = ET.fromstring(corpus.read_effective("foc", "data/xml/sfxeventsgui.xml").data)
            for name, spatial, localized, loop in (("PauseSpatial", True, True, True),
                                                    ("PauseSpatialIdle", True, True, True),
                                                    ("PauseLoop2D", False, False, True),
                                                    ("PauseLocalized2D", False, True, False),
                                                    ("PauseOneShot", True, False, False)):
                node = ET.SubElement(tree, "SFXEvent", Name=name)
                for tag, value in (("Is_3D", "Yes" if spatial else "No"),
                                   ("Localize", "Yes" if localized else "No"),
                                   ("Play_Count", "-1" if loop else "1"), ("Max_Instances", "1"),
                                   ("Samples", "pause-tone.wav")):
                    ET.SubElement(node, tag).text = value
            ET.ElementTree(tree).write(xml / "sfxeventsgui.xml", encoding="utf-8")
            tree = ET.fromstring(corpus.read_effective("foc", "data/xml/spaceunitscorvettes.xml").data)
            corvette = next(node for node in tree if node.get("Name") == "Corellian_Corvette")
            # SND-03: idle/moving use distinct events so switching never requests a duplicate attached loop.
            fields = {"SFXEvent_Engine_Idle_Loop": "PauseSpatialIdle", "SFXEvent_Engine_Moving_Loop": "PauseSpatial",
                      "SFXEvent_Select": "PauseLoop2D", "SFXEvent_Move": "PauseLocalized2D", "SFXEvent_Ambient_Moving": "PauseOneShot",
                      "SFXEvent_Ambient_Moving_Min_Delay_Seconds": "1", "SFXEvent_Ambient_Moving_Max_Delay_Seconds": "1"}
            for tag, value in fields.items():
                node = corvette.find(tag)
                if node is None: node = ET.SubElement(corvette, tag)
                node.text = value
            ET.ElementTree(tree).write(xml / "spaceunitscorvettes.xml", encoding="utf-8")
            tree = ET.fromstring(corpus.read_effective("foc", "data/xml/squadrons.xml").data)
            squadron = next(node for node in tree if node.get("Name") == "Rebel_X-Wing_Squadron")
            node = squadron.find("Build_Speech_Underway")
            if node is None: node = ET.SubElement(squadron, "Build_Speech_Underway")
            node.text = "Speech_Death_Star_Build_Underway"
            ET.ElementTree(tree).write(xml / "squadrons.xml", encoding="utf-8")
            sound = xml.parent / "Audio/SFX/pause-tone.wav"
            sound.parent.mkdir(parents=True)
            with wave.open(str(sound), "wb") as stream:
                stream.setparams((1, 2, 22050, 0, "NONE", "not compressed"))
                stream.writeframes(b"".join(struct.pack("<h", int(1500 * math.sin(2 * math.pi * 440 * i / 22050)))
                                            for i in range(22050 * 10)))
            arguments = (
                "--eawr-mod-root", str(xml.parents[1]), "--eawr-live-ai", "off", "--eawr-live-step", "1",
                "--eawr-live-audio-pace", "on", "--eawr-map-timed-frames", "1", "--eawr-live-ticks", "150",
                "--eawr-live-order", f"15:move:{CORVETTE}@-4500,5600,0",
                "--eawr-live-input", f"f5:click:unit={CORVETTE}", "--eawr-live-input", "f12:click:unit=1",
                "--eawr-live-input", "f20:click:card=0", "--eawr-live-input", f"f25:click:unit={CORVETTE}",
                "--eawr-live-input", "f30:rclick:@-4500,5600,0", "--eawr-live-input", "f40:click:hud=pause",
                "--eawr-live-input", "f60:click:hud=resume", "--eawr-live-input", "f75:click:hud=pause",
                "--eawr-live-input", "f90:click:hud=resume")
            code, report = self._run(directory, "pause", arguments, engine_args=("--quit-after", "600"))
            self.assertEqual(code, 0, report.get("failure"))
            self.assertTrue(report["live_session"]["headless_hashes_equal"])
            audio = report["battle_audio"]
            pause = audio["pause"]
            self.assertEqual(pause["transitions"], 4, pause)
            self.assertFalse(pause["active"], pause)
            self.assertGreater(pause["spatial"], 0, pause)
            self.assertGreater(pause["loops_2d"], 0, pause)
            self.assertGreater(pause["speech"], 0, pause)
            self.assertGreater(pause["voice_frames"], pause["spatial"] + pause["loops_2d"], pause)
            self.assertGreater(pause["speech_frames"], pause["speech"], pause)
            self.assertLess(pause["position_drift"], 0.05, pause)
            starts = audio["ability_starts"]
            loop = [row for row in starts if row["event"] == "PauseLoop2D"]
            self.assertEqual(len(loop), 1, loop)
            self.assertEqual(loop[0]["bus"], "EAWR_SFX")
            spatial = [row for row in starts if row["event"] == "PauseSpatial"]
            self.assertTrue(spatial, starts)
            self.assertTrue(all(row["bus"] == "EAWR_Voice" and row["position"] for row in spatial), spatial)
            one_shots = [row for row in starts if row["event"] == "PauseOneShot"]
            self.assertTrue(one_shots, starts)
            self.assertTrue(all(row["bus"] == "EAWR_SFX" and row["position"] for row in one_shots), one_shots)
            localized = [row for row in starts if row["event"] == "PauseLocalized2D"]
            self.assertEqual(len(localized), 1, localized)
            self.assertEqual(localized[0]["bus"], "EAWR_Voice")
            self.assertIsNone(localized[0]["position"])
            speech = [row for row in audio["announcement_starts"] if row["reason"].startswith("speech_")]
            self.assertEqual(len(speech), 1, speech)
            for row in audio["allocations"].values():
                self.assertEqual(row["requested"], row["admitted"] + row["refused"], row)
                self.assertEqual(row["samples_requested"], row["allocated"] + row["samples_failed"], row)
                self.assertLessEqual(row["audible"], row["allocated"], row)
            # Engine frame-limit exit exercises paused teardown and emits a lifecycle
            # trace; it does not write the ordinary completed-capture report.
            completed = subprocess.run([
                os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                "--path", str(ROOT / "apps/viewer/project"), "--quit-after", "320", "--",
                "--eawr-map", CORUSCANT, "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                "--eawr-populate", "--eawr-map-camera-config", str(CAMERA), "--eawr-live-session", "m2",
                *arguments, "--eawr-live-input", "f120:click:hud=pause"],
                cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=180, check=False)
            (directory / "paused-teardown.log").write_text(completed.stdout, encoding="utf-8")
            self.assertEqual(completed.returncode, 0, completed.stdout[-4000:])
            self.assert_map_teardown_before_host_destruction(completed.stdout)
            ticks = re.search(r"finish: (\d+) ticks", completed.stdout)
            self.assertIsNotNone(ticks, completed.stdout[-4000:])
            self.assertGreater(int(ticks.group(1)), 60)
            self.assertLess(int(ticks.group(1)), 150, "final pause held the simulation before teardown")
            self.assertNotIn("ObjectDB instances leaked", completed.stdout)

    def test_ability_payload_barrage_confirmation(self):
        self._ability_payload_case("Broadside_Class_Cruiser", "Empire", "BARRAGE", "Unit_Barrage_Interdictor", point_cursor=True)

    def test_ability_payload_concentrate_confirmation(self):
        self._ability_payload_case("Home_One", "Rebel", "CONCENTRATE_FIRE", "Unit_Barrage_Ackbar")

    def test_ability_payload_stock_selection_overlap_refuses_confirmation(self):
        self._ability_payload_case("Home_One", "Rebel", "CONCENTRATE_FIRE", "Unit_Barrage_Ackbar", isolated=False)

    def test_ability_payload_energy_confirmation(self):
        self._ability_payload_case("Accuser_Star_Destroyer", "Empire", "ENERGY_WEAPON", "Unit_Energy_Blast_Piett")

    def test_ability_payload_tractor_confirmation(self):
        self._ability_payload_case("Admonitor_Star_Destroyer", "Empire", "TRACTOR_BEAM", "Unit_Tractor_Beam_Thrawn")

    def test_ability_payload_weaken_confirmation_does_not_double_spawn(self):
        self._ability_payload_case("Sundered_Heart", "Rebel", "WEAKEN_ENEMY", "Unit_Energy_Flux_Antilles", point_cursor=True)

    def test_ability_payload_scripted_weaken_attaches_to_source(self):
        self._ability_payload_case("Sundered_Heart", "Rebel", "WEAKEN_ENEMY", "Unit_Energy_Flux_Antilles", scripted=True)

    def test_ability_payload_harmonic_authored_spatial_cue(self):
        # Stock harmonic bomb has no cue; an overlay proves BA-54 without inventing a default.
        self._ability_payload_case("Slave_I", "Empire", "HARMONIC_BOMB", "Unit_Point_Laser_Fire", scripted=True, overlay=True)

    def _ability_payload_case(self, hero, faction, kind, cue, scripted=False, overlay=False, isolated=True, point_cursor=False):
        import struct
        import xml.etree.ElementTree as ET
        from tools.inventory.corpus import Corpus

        with tempfile.TemporaryDirectory(prefix="eawr-ability-payload-") as temporary:
            directory = pathlib.Path(temporary)
            mod_args = ()
            if overlay or (not scripted and isolated):
                xml = directory / "mod/Data/XML"
                xml.mkdir(parents=True)
                (xml.parent / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
                filename = ("spaceunitscorvettes.xml" if kind == "BARRAGE" else
                            "units_hero_empire_thrawn.xml" if kind == "TRACTOR_BEAM" else "uniqueunits.xml")
                authored = Corpus(os.environ["EAWR_EAW_GAME_ROOT"]).read_effective("foc", "data/xml/" + filename)
                tree = ET.fromstring(authored.data)
                obj = next(node for node in tree if node.get("Name") == hero)
                ability = next(node for node in obj.findall("Unit_Abilities_Data/Unit_Ability")
                               if node.findtext("Type", "").strip() == kind)
                if overlay:
                    node = ability.find("SFXEvent_Target_Ability")
                    if node is None: node = ET.SubElement(ability, "SFXEvent_Target_Ability")
                    node.text = cue
                if not scripted:
                    # Selection speech shares the stock ability cue's overlap channel.
                    # Isolate ability admission without changing any ability/event policy.
                    node = obj.find("SFXEvent_Select")
                    if node is None: node = ET.SubElement(obj, "SFXEvent_Select")
                    node.text = ""
                ET.ElementTree(tree).write(xml / filename, encoding="utf-8")
                mod_args = ("--eawr-mod-root", str(xml.parent.parent))
            seed = directory / "seed.eawr-replay"
            target_type = "Nebulon_B_Frigate"
            args = ("--eawr-skirmish-slot", f"1:{faction}:0:human", "--eawr-skirmish-slot", "2:Rebel:1:human",
                    "--eawr-skirmish-fleet", f"1:{hero}", "--eawr-skirmish-fleet", f"2:{target_type}",
                    "--eawr-live-ai", "off", "--eawr-live-reveal", "on", "--eawr-live-player", "1",
                    "--eawr-live-step", "1", "--eawr-live-ticks", "8", "--eawr-live-replay-out", str(seed), *mod_args)
            code, roster = self._run(directory, "roster", args, session=("--eawr-live-session", "skirmish"))
            self.assertEqual(code, 0, roster.get("failure"))
            fleet = roster["live_session"]["start_fleet"]
            source = next(row["entity"] for row in fleet if row["type"] == hero)
            target = next(row["entity"] for row in fleet if row["type"] == target_type)
            data = bytearray(seed.read_bytes())
            self.assertEqual(data[:8], b"EAWRPLY\0")
            self.assertEqual(struct.unpack_from("<Q", data, 64)[0], 0)
            header = struct.unpack_from("<H", data, 10)[0]
            players = struct.unpack_from("<I", data, 48)[0]
            count = struct.unpack_from("<Q", data, 56)[0]
            source_x = -800 if kind == "BARRAGE" else -400
            for index in range(count):
                offset = header + players * 24 + index * 80
                entity = struct.unpack_from("<Q", data, offset)[0]
                if entity not in (source, target): continue
                struct.pack_into("<7q", data, offset + 24, (source_x if entity == source else 400) << 24,
                                 -1500 << 24, 0, 0, 0, 0, 1 << 24)
            struct.pack_into("<Q", data, 40, 100)
            replay = directory / "payload.eawr-replay"
            replay.write_bytes(data)
            if scripted:
                destination = ",on,400,-1500,0" if kind == "WEAKEN_ENEMY" else ",on"
                inputs = ("--eawr-live-order", f"35:ability:{source}@{kind}{destination}")
            else:
                # Read the actual displayed button instead of depending on hotkey bindings.
                # Initial roster is unselected; select once to query authored buttons.
                code, selected = self._run(directory, "selected", (
                    "--eawr-live-replay", str(replay), "--eawr-live-ai", "off", "--eawr-live-reveal", "on",
                    "--eawr-live-player", "1", "--eawr-live-step", "1", "--eawr-live-ticks", "25",
                    "--eawr-live-input", f"20:click:unit={source}", *mod_args),
                    session=("--eawr-live-session", "replay"),
                    camera=ROOT / "apps/viewer/project/config/coruscant-hero-beams-camera.xml")
                self.assertEqual(code, 0, selected.get("failure"))
                buttons = selected["hud"]["ability_buttons"]["buttons"]
                button = next(i for i, row in enumerate(buttons) if row["name"] == kind)
                self.assertFalse(buttons[button]["disabled"], buttons[button])
                aim = "@400,-1500,0" if kind in ("WEAKEN_ENEMY", "BARRAGE") else f"unit={target}"
                if point_cursor: aim = "@0,-1500,0"
                inputs = ("--eawr-live-input", f"20:click:unit={source}",
                          "--eawr-live-input", f"30:click:ability={button}",
                          *(("--eawr-live-input", "35:hover:@0,-1500,0") if point_cursor else ()),
                          "--eawr-live-input", f"{45 if point_cursor else 35}:click:{aim}")
            code, result = self._run(directory, "payload", (
                "--eawr-live-replay", str(replay), "--eawr-live-ai", "off", "--eawr-live-reveal", "on",
                "--eawr-live-player", "1", "--eawr-live-step", "1", "--eawr-live-ticks", "90",
                "--eawr-live-audio-pace", "on", *inputs, *mod_args), session=("--eawr-live-session", "replay"),
                camera=ROOT / "apps/viewer/project/config/coruscant-hero-beams-camera.xml")
            self.assertEqual(code, 0, result.get("failure"))
            self.assertEqual(result["live_session"]["rejected"], [])
            self.assertTrue(result["live_session"]["headless_hashes_equal"])
            if point_cursor:
                samples = result["battle_input"]["cursor_samples"]
                self.assertTrue(any(row["id"] == "POINTER_TARGET_SPECIAL_ABILITY_TO_SPACE_POSITION"
                                    and row["hover"] == "empty" for row in samples), samples)
                # CU-12: logical fog/radius refusal agrees with hover, even with reveal art on.
                invalid = "@0,4000,0" if kind == "BARRAGE" else "@2000,-1500,0"
                for name, point in (("invalid-point", invalid), ("outside-map", "@100000,-1500,0")):
                    code, refused = self._run(directory, name, (
                        "--eawr-live-replay", str(replay), "--eawr-live-ai", "off", "--eawr-live-reveal", "on",
                        "--eawr-live-player", "1", "--eawr-live-step", "1", "--eawr-live-ticks", "60",
                        "--eawr-live-input", f"20:click:unit={source}",
                        "--eawr-live-input", f"30:click:ability={button}",
                        "--eawr-live-input", f"35:hover:{point}",
                        "--eawr-live-input", f"45:click:{point}", *mod_args),
                        session=("--eawr-live-session", "replay"),
                        camera=ROOT / "apps/viewer/project/config/coruscant-hero-beams-camera.xml")
                    self.assertEqual(code, 0, refused.get("failure"))
                    invalid_samples = refused["battle_input"]["cursor_samples"]
                    self.assertTrue(any(row["id"] == "POINTER_TARGET_SPECIAL_ABILITY_TO_SPACE_POSITION_INVALID"
                                        for row in invalid_samples), invalid_samples)
                    self.assertEqual(refused["battle_input"]["ability_bar"]["targeted"], 0)
                    self.assertFalse(refused["battle_input"]["ability_bar"]["targeting"])
                    self.assertTrue(any("invalid ability point" in line for line in refused["battle_input"]["log"]))
                    self.assertEqual(refused["live_session"]["rejected"], [])
                    self.assertTrue(refused["live_session"]["headless_hashes_equal"])
                    self.assertNotIn("ability_target:" + cue, refused["battle_audio"]["requested"])
            audio = result["battle_audio"]
            rows = [row for row in audio["ability_starts"] if row["event"] == cue
                    and row["reason"] in ("ability_target", "ability_spawn")]
            if not isolated:
                self.assertEqual(audio["requested"].get("ability_target:" + cue), 1)
                self.assertEqual(audio["results"].get("ability_target"), {"overlap": 1})
                self.assertEqual(rows, [])
                self.assertTrue(any(row["reason"] == "response_select" for row in audio["ability_starts"]))
                return
            self.assertEqual(len(rows), 1, {"starts": rows, "requested": audio["requested"], "results": audio["results"]})
            self.assertEqual(rows[0]["reason"], "ability_spawn" if scripted else "ability_target")
            self.assertEqual(rows[0]["attached"], source)
            if kind != "HARMONIC_BOMB": self.assertLess(abs(rows[0]["position"][0] - source_x), 50)
            self.assertGreater(rows[0]["volume"], 0)
            self.assertTrue(rows[0]["sample"])
            self.assertGreaterEqual(rows[0]["tick"], 34)

    def test_base_under_attack_uses_mode_cooldown_and_authored_radar(self):
        import struct
        import xml.etree.ElementTree as ET
        from tools.inventory.corpus import Corpus

        with tempfile.TemporaryDirectory(prefix="eawr-base-warning-") as temporary:
            directory = pathlib.Path(temporary)
            corpus = Corpus(os.environ["EAWR_EAW_GAME_ROOT"])
            seed_xml = directory / "seed-mod/Data/XML"
            seed_xml.mkdir(parents=True)
            (seed_xml.parent / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
            for filename in ("spaceunitsfrigates.xml", "starbases.xml"):
                tree = ET.fromstring(corpus.read_effective("foc", "data/xml/" + filename).data)
                for field in tree.iter():
                    if field.tag.startswith(("Starting_Spawned_Units_Tech_", "Reserve_Spawned_Units_Tech_")):
                        field.text = " "
                ET.ElementTree(tree).write(seed_xml / filename, encoding="utf-8")
            seed = directory / "seed.eawr-replay"
            code, roster = self._run(directory, "roster", (
                "--eawr-mod-root", str(seed_xml.parents[1]),
                "--eawr-live-ai", "off", "--eawr-live-ticks", "8", "--eawr-map-timed-frames", "1",
                "--eawr-live-replay-out", str(seed)))
            self.assertEqual(code, 0, roster.get("failure"))
            replay = bytearray(seed.read_bytes())
            self.assertEqual(replay[:8], b"EAWRPLY\0")
            self.assertEqual(struct.unpack_from("<Q", replay, 64)[0], 0)
            header = struct.unpack_from("<H", replay, 10)[0]
            players = struct.unpack_from("<I", replay, 48)[0]
            count = struct.unpack_from("<Q", replay, 56)[0]
            # Isolate a hostile ship firing at an unselected base. No damage injection
            # is required to announce the attack; remove its attacker later to expire radar.
            for index in range(count):
                offset = header + players * 24 + index * 80
                entity, kind, owner = struct.unpack_from("<QQI", replay, offset)
                x, y = (0, -1500) if entity == STATION else (600, -1500) if entity == ACCLAMATOR else (
                    (-9000 if owner == 1 else 9000), -9000)
                struct.pack_into("<7q", replay, offset + 24, x << 24, y << 24, 0, 0, 0, 0, 1 << 24)
            struct.pack_into("<Q", replay, 40, 750)
            path = directory / "base.eawr-replay"
            path.write_bytes(replay)
            for name, delay, authored in (("stock", 60, True), ("short", 2, True), ("blank", 2, False)):
                xml = directory / name / "Data/XML"
                xml.mkdir(parents=True)
                (xml.parent / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
                tree = ET.fromstring(corpus.read_effective("foc", "data/xml/audio.xml").data)
                tree.find("Delay_Between_Space_Base_Attack_Announcement_Seconds").text = str(delay)
                ET.ElementTree(tree).write(xml / "audio.xml", encoding="utf-8")
                # Keep carrier/station replenishment from adding new attackers after
                # the staged ship is removed; their initial replay teams stay distant.
                for filename in ("spaceunitsfrigates.xml", "starbases.xml"):
                    shutil.copyfile(seed_xml / filename, xml / filename)
                if not authored:
                    tree = ET.fromstring(corpus.read_effective("foc", "data/xml/factions.xml").data)
                    rebel = next(node for node in tree if node.get("Name") == "Rebel")
                    rebel.find("SFXEvent_Space_Base_Under_Attack_Announcement").text = " "
                    ET.ElementTree(tree).write(xml / "factions.xml", encoding="utf-8")
                code, report = self._run(directory, name, (
                    "--eawr-mod-root", str(xml.parents[1]), "--eawr-live-replay", str(path),
                    "--eawr-live-player", "1", "--eawr-live-ai", "off", "--eawr-live-step", "1",
                    "--eawr-live-ticks", "710", "--eawr-live-audio-pace", "on", "--eawr-map-timed-frames", "1",
                    "--eawr-live-order", f"1:attack:{ACCLAMATOR}@{STATION}",
                    "--eawr-live-order", f"450:damage:{ACCLAMATOR}@1000000"),
                    session=("--eawr-live-session", "replay"))
                self.assertEqual(code, 0, report.get("failure"))
                self.assertTrue(report["live_session"]["headless_hashes_equal"])
                audio = report["battle_audio"]
                warnings = audio["base_warning"]
                self.assertEqual(warnings["delay_frames"], delay * 30)
                rows = warnings["rows"]
                self.assertTrue(rows, audio)
                self.assertTrue(all(row["target"] == STATION and row["radar"] == authored for row in rows), rows)
                self.assertTrue(all(b["tick"] - a["tick"] >= delay * 30 for a, b in zip(rows, rows[1:])), rows)
                if name == "stock": self.assertEqual(len(rows), 1, rows)
                else: self.assertGreater(len(rows), 1, rows)
                self.assertEqual(audio["requested"].get("base_under_attack:RHD_Space_Station_Under_Attack", 0), len(rows) if authored else 0)
                requested = sum(value for key, value in audio["requested"].items() if key.startswith("base_under_attack:"))
                self.assertEqual(requested, len(rows) if authored else 0, audio)
                self.assertEqual(warnings["radar_active"], 0, warnings)
                self.assertEqual(report["hud"]["minimap"]["warning_icon"], "i_radar_focus.tga")
                self.assertTrue(report["hud"]["minimap"]["warning_texture_available"])
                self.assertEqual(report["hud"]["minimap"]["warnings"], 0)

            # The attack ends before the first post-stall frame; snapshot history
            # cannot recover its original warning tick. The bounded journal must.
            paired = []
            for name, stall in (("paced", ()), ("stalled", ("--eawr-live-stall", "start:500"))):
                code, report = self._run(directory, name, (
                    "--eawr-mod-root", str(directory / "stock"), "--eawr-live-replay", str(path),
                    "--eawr-live-player", "1", "--eawr-live-ai", "off", "--eawr-live-step", "1",
                    "--eawr-live-ticks", "710", "--eawr-live-audio-pace", "off", "--eawr-map-timed-frames", "1",
                    "--eawr-live-order", f"1:attack:{ACCLAMATOR}@{STATION}",
                    "--eawr-live-order", f"450:damage:{ACCLAMATOR}@1000000", *stall),
                    session=("--eawr-live-session", "replay"))
                self.assertEqual(code, 0, report.get("failure"))
                self.assertTrue(report["live_session"]["headless_hashes_equal"])
                self.assertEqual(report["live_session"]["event_gaps"], [])
                paired.append(report)
            self.assertEqual(paired[0]["live_session"]["completed_ticks"], paired[1]["live_session"]["completed_ticks"])
            first = paired[0]["battle_audio"]["base_warning"]
            second = paired[1]["battle_audio"]["base_warning"]
            self.assertTrue(first["rows"], first)
            self.assertLess(first["rows"][0]["tick"], 450)
            self.assertEqual(first["rows"], second["rows"])
            self.assertEqual(first["countdown"], second["countdown"])

    def test_reinforcement_feedback_tracks_open_place_invalid_drop_and_submit(self):
        self._reinforcement_feedback_case(False)

    def test_reinforcement_feedback_prefers_authored_fleet_move(self):
        self._reinforcement_feedback_case(True)

    def _reinforcement_feedback_case(self, fleet_move):
        import xml.etree.ElementTree as ET
        from tools.inventory.corpus import Corpus

        with tempfile.TemporaryDirectory(prefix="eawr-reinforcement-audio-") as temporary:
            directory = pathlib.Path(temporary)
            xml = directory / "mod/Data/XML"
            xml.mkdir(parents=True)
            (xml.parent / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
            source = Corpus(os.environ["EAWR_EAW_GAME_ROOT"]).read_effective("foc", "data/xml/factions.xml")
            self.assertIsNotNone(source)
            tree = ET.fromstring(source.data)
            rebel = next(node for node in tree if node.get("Name") == "Rebel")
            cancelled = rebel.find("Reinforcements_Cancelled_SFXEvent").text.strip()
            # Stock pick-land-zone is blank; exercise the consumer with an existing GUI event.
            rebel.find("Reinforcements_Pick_Landing_Zone_SFXEvent").text = "GUI_Bad_Sound"
            ET.ElementTree(tree).write(xml / "factions.xml", encoding="utf-8")
            if fleet_move:
                source = Corpus(os.environ["EAWR_EAW_GAME_ROOT"]).read_effective("foc", "data/xml/squadrons.xml")
                self.assertIsNotNone(source)
                tree = ET.fromstring(source.data)
                # Build slot zero is the X-wing squadron in the staged Rebel station.
                squadron = next(node for node in tree if node.get("Name") == "Rebel_X-Wing_Squadron")
                for field in list(squadron):
                    if field.tag == "SFXEvent_Command_Fleet_Move": squadron.remove(field)
                ET.SubElement(squadron, "SFXEvent_Command_Fleet_Move").text = "GUI_Toggle_Shields_On"
                ET.ElementTree(tree).write(xml / "squadrons.xml", encoding="utf-8")
            gestures = (
                "10:click:unit=1", "20:click:card=0", "500:click:hud=b_reinforcement",
                "510:press:hud=r_0000", "515:hover:@-3811,4460,0", "520:release:@-3811,4460,0",
                "535:press:hud=r_0000", "540:rclick:@-3111,3960,0",
                "550:click:hud=r_close", "600:click:hud=b_reinforcement", "750:press:hud=r_0000",
                "755:hover:@-3111,3960,0", "760:release:@-3111,3960,0")
            # Let the authored pane line finish: stock en-route uses ordinary HUD overlap admission.
            code, report = self._run(directory, "feedback", (
                "--eawr-mod-root", str(xml.parents[1]), "--eawr-live-ai", "off", "--eawr-live-ticks", "850",
                "--eawr-live-step", "1", "--eawr-live-audio-pace", "on", "--eawr-map-timed-frames", "1",
                *(part for row in gestures for part in ("--eawr-live-input", row))))
            self.assertEqual(code, 0, report.get("failure"))
            live = report["live_session"]
            self.assertTrue(live["headless_hashes_equal"])
            self.assertEqual(live["economy_requests"]["reinforcements"], 1, live)
            self.assertEqual(live["economy_requests"]["refused"], 1, live)
            self.assertEqual(live["rejected"], [])
            audio = report["battle_audio"]
            self.assertTrue(audio["muted"])
            self.assertEqual(audio["requested"].get("reinforcement_pane:RHD_Choose_Reinforcements"), 2, audio)
            self.assertEqual(audio["requested"].get("reinforcement_placement:GUI_Bad_Sound"), 3, audio)
            # A failed drop and an explicit right-click cancel each request the authored cue.
            # Ordinary HUD overlap may refuse playback while the pane line is still playing.
            self.assertEqual(audio["requested"].get("reinforcement_cancelled:" + cancelled), 2, audio)
            enroute = "GUI_Toggle_Shields_On" if fleet_move else "RHD_Reinforcements_En_Route"
            self.assertEqual(audio["requested"].get("reinforcement_enroute:" + enroute), 1, audio)
            if fleet_move:
                self.assertNotIn("reinforcement_enroute:RHD_Reinforcements_En_Route", audio["requested"])
            starts = [row for row in audio["ability_starts"] if row["reason"].startswith("reinforcement_")
                      and row["reason"] != "reinforcement_cancelled"]
            self.assertEqual([row["reason"] for row in starts], [
                "reinforcement_pane", "reinforcement_placement", "reinforcement_placement", "reinforcement_pane",
                "reinforcement_placement", "reinforcement_enroute"], starts)
            self.assertTrue(all(row["tick"] > 760 for row in starts if row["reason"] == "reinforcement_enroute"))

    def test_ship_engine_loops_use_stock_zero_threshold_and_do_not_retry(self):
        with tempfile.TemporaryDirectory(prefix="eawr-engine-stock-") as temporary:
            directory = pathlib.Path(temporary)
            code, report = self._run(directory, "stock", (
                "--eawr-live-ai", "off", "--eawr-live-ticks", "150", "--eawr-live-step", "1",
                "--eawr-live-audio-pace", "on", "--eawr-map-timed-frames", "1"))
            self.assertEqual(code, 0, report.get("failure"))
            self.assertTrue(report["live_session"]["headless_hashes_equal"])
            audio = report["battle_audio"]
            self.assertTrue(audio["muted"])
            self.assertEqual(audio["engines"]["idle_speed"], 0)
            # SND-67: ordinary stopped service selects moving with the stock zero threshold.
            for kind in ("nebulon", "corvette", "calamari", "acclamator"):
                starts = {key: count for key, count in audio["requested"].items()
                          if key.startswith("engine_moving:") and kind in key.lower()}
                self.assertTrue(starts, (kind, audio))
                self.assertTrue(all(count == 1 for count in starts.values()), starts)
            self.assertGreater(audio["engines"]["stops"], 0)
            self.assertGreater(audio["engines"]["fade_updates"], 0)
            self.assertLessEqual(audio["engines"]["sources"], len(report["live_session"]["start_fleet"]))
            self.assertEqual(audio["voices_3d"], 32)
            self.assertLessEqual(audio["max_voices"], 48)

    def test_ship_engine_switches_follow_authored_threshold_and_attached_pose(self):
        import xml.etree.ElementTree as ET
        from tools.inventory.corpus import Corpus

        with tempfile.TemporaryDirectory(prefix="eawr-engine-switch-") as temporary:
            directory = pathlib.Path(temporary)
            xml = directory / "mod/Data/XML"
            xml.mkdir(parents=True)
            (xml.parent / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
            source = Corpus(os.environ["EAWR_EAW_GAME_ROOT"]).read_effective("foc", "data/xml/gameconstants.xml")
            self.assertIsNotNone(source)
            tree = ET.fromstring(source.data)
            tree.find("SpaceIdleMovementSpeed").text = "0.5"
            ET.ElementTree(tree).write(xml / "gameconstants.xml", encoding="utf-8")
            code, report = self._run(directory, "switch", (
                "--eawr-mod-root", str(xml.parents[1]), "--eawr-live-ai", "off", "--eawr-live-ticks", "300",
                "--eawr-live-step", "1", "--eawr-live-audio-pace", "on", "--eawr-map-timed-frames", "1",
                "--eawr-live-order", f"45:move:{NEBULON}@-4500,5600,0",
                "--eawr-live-order", f"180:stop:{NEBULON}"))
            self.assertEqual(code, 0, report.get("failure"))
            self.assertTrue(report["live_session"]["headless_hashes_equal"])
            self.assertEqual(report["live_session"]["rejected"], [])
            audio = report["battle_audio"]
            self.assertEqual(audio["engines"]["idle_speed"], 0.5)
            self.assertEqual(audio["requested"].get("engine_idle:Unit_Nebulon_Idle_Engine_Loop"), 2, audio)
            self.assertEqual(audio["requested"].get("engine_moving:Unit_Nebulon_Moving_Engine_Loop"), 1, audio)
            self.assertGreater(audio["attached_moves"], 0)
            self.assertGreater(audio["engines"]["fade_updates"], 0)

    def test_fogged_ship_engine_loops_are_admitted_and_muted(self):
        with tempfile.TemporaryDirectory(prefix="eawr-engine-fog-") as temporary:
            directory = pathlib.Path(temporary)
            code, report = self._run(directory, "fog", (
                "--eawr-live-ai", "off", "--eawr-live-reveal", "off", "--eawr-live-ticks", "150",
                "--eawr-live-step", "1", "--eawr-live-audio-pace", "on", "--eawr-map-timed-frames", "1"))
            self.assertEqual(code, 0, report.get("failure"))
            self.assertTrue(report["live_session"]["headless_hashes_equal"])
            audio = report["battle_audio"]
            self.assertGreater(audio["engines"]["hidden_updates"], 0, audio)
            self.assertTrue(any(row["reason"].startswith("engine_") and row["volume"] == 0
                                for row in audio["ability_starts"]), audio)
            self.assertFalse(any(results.get("hidden", 0) for reason, results in audio["results"].items()
                                 if reason.startswith("engine_")), audio)

    def test_offscreen_enemy_ai_ability_toggles_are_silent(self):
        # BA-50/52: faction effects use the enemy table; unit voices require a local press.
        with tempfile.TemporaryDirectory(prefix="eawr-enemy-ai-toggle-") as temporary:
            directory = pathlib.Path(temporary)
            code, report = self._run(directory, "enemy-ai", (
                "--eawr-skirmish-slot", "1:Rebel:0:ai",
                "--eawr-skirmish-slot", "2:Empire:1:human",
                "--eawr-live-player", "2", "--eawr-live-ai", "on",
                "--eawr-live-reveal", "off", "--eawr-live-step", "1",
                "--eawr-live-ticks", "600"), session=("--eawr-live-session", "skirmish"),
                camera=ROOT / "apps/viewer/project/config/coruscant-empire-live-session-camera.xml")
            self.assertEqual(code, 0, report.get("failure"))
            live = report["live_session"]
            self.assertIn(1, live["ai"]["players"], live["ai"])
            self.assertTrue(live["headless_hashes_equal"])
            self.assertGreater(live["hidden_units"], 0, live)
            self.assertEqual(live["ability_requests"]["issued"], 0, live)
            audio = report["battle_audio"]
            toggles = [row for row in audio["abilities"] if row.startswith("toggle Rebel ")]
            self.assertTrue(toggles, audio)
            self.assertTrue(all(" enemy <none>" in row for row in toggles), toggles)
            self.assertFalse(any(key.startswith(("ability_toggle:", "ability_voice:"))
                                 for key in audio["requested"]), audio)

    def _command_audio_overlay(self, directory, assists=False):
        import xml.etree.ElementTree as ET
        from tools.inventory.corpus import Corpus

        xml = directory / "mod/Data/XML"
        xml.mkdir(parents=True)
        (xml.parent / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
        corpus = Corpus(os.environ["EAWR_EAW_GAME_ROOT"])
        source = corpus.read_effective("foc", "data/xml/audio.xml")
        self.assertIsNotNone(source)
        audio = ET.fromstring(source.data)
        # The stock six fields are blank. Prove each consumer using an existing GUI sample.
        for suffix in ("Attack", "Attack_Move", "Move", "Stop", "Guard"):
            audio.find("SFXEvent_Command_Bar_" + suffix).text = "GUI_Bad_Sound"
        ET.ElementTree(audio).write(xml / "audio.xml", encoding="utf-8")
        if assists:
            for filename, name, fields in (
                ("spaceunitsfrigates.xml", "Nebulon_B_Frigate",
                 {"SFXEvent_Group_Move": "Unit_Move_Nebulon", "SFXEvent_Group_Attack": "Unit_Attack_Nebulon"}),
                ("spaceunitscorvettes.xml", "Corellian_Corvette",
                 {"SFXEvent_Assist_Move": "Unit_Move_Corvette", "SFXEvent_Assist_Attack": "Unit_Attack_Corvette"}),
            ):
                source = corpus.read_effective("foc", "data/xml/" + filename)
                self.assertIsNotNone(source)
                tree = ET.fromstring(source.data)
                ship = next(node for node in tree if node.get("Name") == name)
                for key, value in fields.items():
                    for item in list(ship):
                        if item.tag == key: ship.remove(item)
                    ET.SubElement(ship, key).text = value
                ET.ElementTree(tree).write(xml / filename, encoding="utf-8")
        return xml.parents[1]

    def test_command_cues_on_arm_disarm_execute_and_empty_stop(self):
        with tempfile.TemporaryDirectory(prefix="eawr-command-cues-") as temporary:
            directory = pathlib.Path(temporary)
            mod = self._command_audio_overlay(directory)
            inputs = (
                "30:click:hud=attack", "60:key:A", "90:key:A", "120:click:hud=attack",
                "150:click:hud=move", "180:key:M", "210:key:M", "240:click:hud=move",
                "270:key:T", "300:click:hud=attack_move", "330:click:hud=attack_move", "360:key:T",
                "390:key:G", "420:click:hud=guard", "450:click:hud=guard", "480:key:G",
                "510:click:hud=stop", "540:key:S", f"570:click:unit={NEBULON}",
                "690:click:hud=move", "720:rclick:@-4500,5600,0", "810:click:hud=stop", "930:key:S",
            )
            script = tuple(part for row in inputs for part in ("--eawr-live-input", row))
            code, report = self._run(directory, "command-cues", (
                "--eawr-mod-root", str(mod), "--eawr-live-ai", "off", "--eawr-live-step", "1",
                "--eawr-live-ticks", "1050", "--eawr-live-audio-pace", "on", "--eawr-map-timed-frames", "1", *script))
            self.assertEqual(code, 0, report.get("failure"))
            self.assertTrue(report["live_session"]["headless_hashes_equal"])
            self.assertEqual(report["live_session"]["rejected"], [])
            audio = report["battle_audio"]
            expected = {"attack": [30, 90], "move": [150, 210, 690], "attack_move": [270, 330],
                        "guard": [390, 450], "stop": [510, 540, 810, 930]}
            mouse_cues = {30, 150, 330, 450, 510, 690, 810}
            starts = audio["ability_starts"]
            for mode, ticks in expected.items():
                reason = "command_" + mode
                self.assertEqual(audio["requested"].get(reason + ":GUI_Bad_Sound"), len(ticks), audio)
                emitted = [row["tick"] for row in starts if row["reason"] == reason]
                self.assertEqual(len(emitted), len(ticks), starts)
                # Godot queues the GUI callback through one more update than a key event.
                # SND-08/18: admission queues initialization, then the following service
                # allocates the first sample. Both services precede its start timestamp.
                queued_services = 2
                for actual, wanted in zip(emitted, ticks):
                    request_tick = wanted + (2 if wanted in mouse_cues else 1)
                    self.assertEqual(actual, request_tick + queued_services)
            self.assertEqual(sum(n for key, n in audio["requested"].items() if key.startswith("response_stop:")), 2)
            self.assertEqual(sum(n for key, n in audio["requested"].items() if key.startswith("response_move:")), 1)
            self.assertFalse(any(row["reason"].startswith("command_") and row["tick"] == 721 for row in starts))

    def test_order_button_hud_input_parser_rejects_unknown_and_modifiers(self):
        with tempfile.TemporaryDirectory(prefix="eawr-order-hud-parser-") as temporary:
            for name, gesture in (("unknown", "1:click:hud=unknown_order"),
                                  ("modified", "1:click:hud=move+shift")):
                with self.subTest(name=name):
                    code, report = self._run(pathlib.Path(temporary), name,
                                            ("--eawr-live-ticks", "8", "--eawr-live-input", gesture))
                    self.assertEqual(code, 2)
                    self.assertIn("--eawr-live-input expects", report["failure"])

    def test_negative_feedback_cooldown_invalid_target_and_silent_cancel(self):
        with tempfile.TemporaryDirectory(prefix="eawr-command-refusal-") as temporary:
            directory = pathlib.Path(temporary)
            # DEFEND can deactivate while active, but a second activation during recharge is refused.
            code, report = self._run(directory, "refusal", (
                "--eawr-live-ai", "off", "--eawr-live-step", "1", "--eawr-live-ticks", "450",
                "--eawr-live-audio-pace", "on", "--eawr-map-timed-frames", "1",
                "--eawr-live-input", f"10:click:unit={NEBULON}",
                "--eawr-live-input", "120:click:ability=0", "--eawr-live-input", "150:click:ability=0",
                "--eawr-live-input", "180:click:ability=0", "--eawr-live-input", "240:key:O+shift"))
            self.assertEqual(code, 0, report.get("failure"))
            self.assertTrue(report["live_session"]["headless_hashes_equal"])
            audio = report["battle_audio"]
            self.assertEqual(audio["requested"].get("negative_feedback:GUI_Bad_Sound"), 2, audio)
            starts = [row["tick"] for row in audio["ability_starts"] if row["reason"] == "negative_feedback"]
            self.assertEqual(len(starts), 2, audio)
            # Refusal feedback is requested one tick after the scripted input.
            # SND-08/18: initialization and first-sample allocation take two queued services.
            queued_services = 2
            for actual, wanted in zip(starts, (180, 240)):
                request_tick = wanted + 1
                self.assertEqual(actual, request_tick + queued_services)
            # A ready Y-wing target press on empty terrain is refused once; explicit cancels are silent.
            code, report = self._run(directory, "target-refusal", (
                "--eawr-live-ai", "off", "--eawr-live-step", "1", "--eawr-live-ticks", "400",
                "--eawr-live-audio-pace", "on", "--eawr-map-timed-frames", "1",
                "--eawr-live-input", "10:click:icon=4", "--eawr-live-input", "120:click:ability=0",
                "--eawr-live-input", "150:click:@-4500,5600,0", "--eawr-live-input", "210:key:I+shift",
                "--eawr-live-input", "240:key:Escape", "--eawr-live-input", "270:key:I+shift",
                "--eawr-live-input", "300:rclick:@-4500,5600,0"))
            self.assertEqual(code, 0, report.get("failure"))
            self.assertTrue(report["live_session"]["headless_hashes_equal"])
            self.assertEqual(report["battle_audio"]["requested"].get("negative_feedback:GUI_Bad_Sound"), 1, report)
            self.assertEqual(report["live_session"]["ability_requests"]["issued"], 0, report)

    def test_response_assist_waits_for_primary_and_refused_primary_adds_none(self):
        from space_hazard_cases import focused_camera, place_ship

        with tempfile.TemporaryDirectory(prefix="eawr-assist-response-") as temporary:
            directory = pathlib.Path(temporary)
            mod = self._command_audio_overlay(directory, assists=True)
            replay = directory / "assist.eawr-replay"
            code, seed = self._run(directory, "seed", (
                "--eawr-mod-root", str(mod), "--eawr-live-ai", "off", "--eawr-live-ticks", "8",
                "--eawr-live-replay-out", str(replay), "--eawr-map-timed-frames", "1"))
            self.assertEqual(code, 0, seed.get("failure"))
            # Separate the two physical pick meshes; their stock spawn projections overlap.
            place_ship(replay, CORVETTE, -5500, 5300)
            place_ship(replay, NEBULON, -4500, 5300)
            place_ship(replay, MC80, -8000, 8000)
            camera = focused_camera(directory, seed, -5000, 5300)
            code, report = self._run(directory, "assist", (
                "--eawr-mod-root", str(mod), "--eawr-live-ai", "off", "--eawr-live-step", "1",
                "--eawr-live-ticks", "600", "--eawr-live-audio-pace", "on", "--eawr-map-timed-frames", "1",
                "--eawr-live-input", f"10:click:unit={NEBULON}",
                "--eawr-live-input", f"30:click:unit={CORVETTE}+shift",
                "--eawr-live-input", "210:rclick:@-5000,5900,0", "--eawr-live-input", "211:rclick:@-5100,5900,0"),
                session=("--eawr-live-session", "replay", "--eawr-live-replay", str(replay)), camera=camera)
            self.assertEqual(code, 0, report.get("failure"))
            self.assertTrue(report["live_session"]["headless_hashes_equal"])
            self.assertEqual(report["live_session"]["rejected"], [])
            audio = report["battle_audio"]
            self.assertEqual(audio["requested"].get("response_move:Unit_Move_Nebulon"), 2, audio)
            self.assertEqual(audio["results"].get("response_move"), {"playing": 1, "instance_limit": 1}, audio)
            self.assertEqual(audio["requested"].get("response_assist_move:Unit_Move_Corvette"), 1, audio)
            primary = next(row for row in audio["ability_starts"] if row["reason"] == "response_move")
            assist = next(row for row in audio["ability_starts"] if row["reason"] == "response_assist_move")
            self.assertGreater(assist["tick"], primary["tick"] + 3, audio)

    def test_removed_assist_source_cancels_pending_move_and_attack(self):
        from space_hazard_cases import focused_camera, place_ship

        with tempfile.TemporaryDirectory(prefix="eawr-assist-source-removal-") as temporary:
            directory = pathlib.Path(temporary)
            mod = self._command_audio_overlay(directory, assists=True)
            replay = directory / "source-removal.eawr-replay"
            code, seed = self._run(directory, "seed", (
                "--eawr-mod-root", str(mod), "--eawr-live-ai", "off", "--eawr-live-ticks", "8",
                "--eawr-live-replay-out", str(replay), "--eawr-map-timed-frames", "1"))
            self.assertEqual(code, 0, seed.get("failure"))
            place_ship(replay, CORVETTE, -5500, 5300)
            place_ship(replay, NEBULON, -4500, 5300)
            place_ship(replay, MC80, -8000, 8000)
            place_ship(replay, ACCLAMATOR, -5000, 6500)
            camera = focused_camera(directory, seed, -5000, 5800)
            for kind, gesture, primary_event, assist_event in (
                ("move", "210:rclick:@-5000,5900,0", "Unit_Move_Nebulon", "Unit_Move_Corvette"),
                ("attack", f"210:rclick:unit={ACCLAMATOR}", "Unit_Attack_Nebulon", "Unit_Attack_Corvette"),
            ):
                for removed in (False, True):
                    with self.subTest(kind=kind, removed=removed):
                        removal = ("--eawr-live-order", f"220:damage:{CORVETTE}@1000000") if removed else ()
                        code, report = self._run(directory, f"{kind}-{removed}", (
                            "--eawr-mod-root", str(mod), "--eawr-live-ai", "off", "--eawr-live-reveal", "on",
                            "--eawr-live-step", "1", "--eawr-live-ticks", "450", "--eawr-live-audio-pace", "on",
                            "--eawr-map-timed-frames", "1", "--eawr-live-input", f"10:click:unit={NEBULON}",
                            "--eawr-live-input", f"30:click:unit={CORVETTE}+shift",
                            "--eawr-live-input", gesture, *removal),
                            session=("--eawr-live-session", "replay", "--eawr-live-replay", str(replay)), camera=camera)
                        self.assertEqual(code, 0, report.get("failure"))
                        self.assertTrue(report["live_session"]["headless_hashes_equal"])
                        self.assertEqual(report["live_session"]["rejected"], [])
                        audio = report["battle_audio"]
                        self.assertEqual(audio["requested"].get(f"response_{kind}:{primary_event}"), 1, audio)
                        self.assertEqual(audio["results"].get("response_" + kind), {"playing": 1}, audio)
                        primary = next(row for row in audio["ability_starts"] if row["reason"] == "response_" + kind)
                        self.assertLess(primary["tick"], 220)
                        self.assertEqual(audio["requested"].get(f"response_assist_{kind}:{assist_event}", 0),
                                         0 if removed else 1, audio)
                        if removed:
                            self.assertNotIn(CORVETTE, [row["entity"] for row in report["live_session"]["own_units"]])
                        else:
                            assist = next(row for row in audio["ability_starts"]
                                          if row["reason"] == "response_assist_" + kind)
                            self.assertGreater(assist["tick"], 220, audio)

    def test_environment_move_responses_use_live_hazard_footprints(self):
        from space_hazard_cases import focused_camera, place_ship

        for name, map_name, mask, event in (
            ("asteroid", "data/art/maps/_mp_space_bespin.ted", 1, "Unit_Asteroids_Nebulon"),
            ("nebula", "data/art/maps/_mp_space_endor.ted", 4, "Unit_Nebula_Nebulon"),
        ):
            with self.subTest(name=name), tempfile.TemporaryDirectory(prefix="eawr-environment-cue-") as temporary:
                directory = pathlib.Path(temporary)
                replay = directory / "fresh.eawr-replay"
                code, seed = self._run(directory, "seed", (
                    "--eawr-skirmish-players", "3,4", "--eawr-skirmish-slot", "3:Rebel:1:human",
                    "--eawr-skirmish-slot", "4:Empire:0:ai", "--eawr-skirmish-fleet", "3:Nebulon_B_Frigate",
                    "--eawr-skirmish-fleet", "4:none", "--eawr-live-replay-out", str(replay),
                    "--eawr-live-ai", "off", "--eawr-live-ticks", "8", "--eawr-map-timed-frames", "1"),
                    session=("--eawr-live-session", "skirmish"), camera=None, map_name=map_name)
                self.assertEqual(code, 0, seed.get("failure"))
                hazard = next(row for row in seed["hud"]["minimap"]["hazards"] if row[4] & mask)
                ship = next(row["entity"] for row in seed["live_session"]["start_fleet"]
                            if row["player"] == 3 and row["type"].lower() == "nebulon_b_frigate")
                x, y = hazard[:2]
                place_ship(replay, ship, x - hazard[2] - 400, y)
                # Keep both the outside speaker and the field destination on screen.
                camera = focused_camera(directory, seed, x - (hazard[2] + 400) / 2, y)
                code, report = self._run(directory, name, (
                    "--eawr-live-player", "3", "--eawr-live-ai", "off", "--eawr-live-reveal", "on",
                    "--eawr-live-step", "1", "--eawr-live-ticks", "450", "--eawr-live-audio-pace", "on",
                    "--eawr-map-timed-frames", "1", "--eawr-live-input", f"10:click:unit={ship}",
                    # Move mode orders through the neutral asteroid mesh under its centre.
                    "--eawr-live-input", "150:key:M",
                    "--eawr-live-input", f"180:rclick:@{x},{y},0"),
                    session=("--eawr-live-session", "replay", "--eawr-live-replay", str(replay)),
                    camera=camera, map_name=map_name)
                self.assertEqual(code, 0, report.get("failure"))
                self.assertTrue(report["live_session"]["headless_hashes_equal"])
                self.assertEqual(report["battle_audio"]["requested"].get(f"response_move_{name}:{event}"), 1, report)
                self.assertTrue(any(row["reason"] == "response_move_" + name
                                    for row in report["battle_audio"]["ability_starts"]), report)

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
            # SND-04/11: attached fog zeros gain, rather than supplying the separate
            # explicit visibility object that refuses an event at admission.
            self.assertEqual(audio["results"].get("ambient_moving", {}).get("hidden", 0), 0, audio)
            starts = [row for row in audio["ability_starts"]
                      if row["reason"] == "ambient_moving" and row["attached"] in fighter_ids]
            visible_starts, hidden_starts = [], []
            for start in starts:
                requests = [row for row in moving
                            if row["unit"] == start["attached"] and row["tick"] <= start["tick"]]
                self.assertTrue(requests, start)
                latest = max(requests, key=lambda row: row["tick"])
                (hidden_starts if latest["hidden"] else visible_starts).append(start)
            self.assertTrue(any(row["volume"] > 0 for row in visible_starts), starts)
            self.assertTrue(hidden_starts, starts)
            for start in hidden_starts:
                self.assertEqual(start["volume"], 0, start)

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
            # SND-18: 2D head insertion precedes the native distance sort.
            # Equal-distance 2D tie order is unverified; require both same-tick starts.
            self.assertCountEqual([row["reason"] for row in starts], ["sighting_type", "sighting_enemy"], starts)
            # A flagged friendly type is eligible too. The enemy on a later
            # roster entry announces independently, including in the same tick.
            self.assertEqual(starts[0]["tick"], starts[1]["tick"])
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
            # Input acknowledgements reach audio on the following update.
            # SND-08/18: initialization and first-sample allocation take two services.
            queued_services = 2
            for reason, tick in (("response_stop", 420), ("response_guard", 550)):
                rows = [row for row in starts if row["reason"] == reason]
                self.assertEqual(len(rows), 1, starts)
                request_tick = tick + 1
                self.assertEqual(rows[0]["tick"], request_tick + queued_services)

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
            # Reticle input acknowledgements reach audio on the following update.
            # SND-08/18: initialization and first-sample allocation take two services.
            queued_services = 2
            for start, tick in zip(starts, (3000, 3120)):
                request_tick = tick + 1
                self.assertEqual(start["tick"], request_tick + queued_services)

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
        # craft (#424, test_battle_input.py: squadron 4, resolved from the live roster) before the
        # icon is clicked. Every M2 squadron is one craft type, so a leader-agnostic roster walk
        # would happen to land on the same sound anyway; the regression this guards is a dropped
        # response (the resolution falling through to the team container's own, missing fields), not
        # a wrong one.
        y_wing_squadron = 4
        spread = ("--eawr-live-order", "1:move:5@-4450,5050,0", "--eawr-live-order", "1:move:2@-5450,5250,0",
                  "--eawr-live-order", "1:move:3@-4650,4250,0")
        with tempfile.TemporaryDirectory(prefix="eawr-live-squadron-leader-death-") as temporary:
            directory = pathlib.Path(temporary)
            code, probe = self._run(directory, "leader-roster", ("--eawr-live-ticks", "1"))
            self.assertEqual(code, 0, probe.get("failure"))
            y_wings = squadron_members(probe, y_wing_squadron, "Y-Wing_Squadron")
            self.assertEqual(len(y_wings), 3)
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
            starts = [row for row in sound["ability_starts"] if row["reason"] == "ability_toggle"]
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
            starts = [row for row in sound["ability_starts"] if row["reason"] == "ability_toggle"]
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
            self.assertEqual([row for row in sound["ability_starts"] if row["reason"] == "ability_toggle"], [])
